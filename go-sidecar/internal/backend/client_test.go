package backend

import (
	"context"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/connectivity"

	pb "my-raft-sidecar/pb"
)

// TestMain silences the per-attempt retry chatter Connect writes to the
// standard logger.
func TestMain(m *testing.M) {
	log.SetOutput(io.Discard)
	code := m.Run()
	log.SetOutput(os.Stderr)
	os.Exit(code)
}

const (
	// bindTimeout bounds how long a test waits for its own listener to come up.
	bindTimeout = 10 * time.Second

	// connectWatchdog must exceed every budget the tests configure: it exists so
	// that a Connect which never returns fails the suite instead of wedging it.
	connectWatchdog = 30 * time.Second

	// rpcTimeout bounds the "is this client actually usable" probe RPC.
	rpcTimeout = 5 * time.Second

	// quickConnect is the ceiling for connecting to a backend that is already
	// listening — no retry window should be burned in that case.
	quickConnect = 1 * time.Second
)

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// stubStateMachine is a real gRPC StateMachine implementation. Connect's
// readiness wait can only be exercised against something that completes a TCP
// and HTTP/2 handshake, so these tests bind real listeners instead of faking
// the generated client.
type stubStateMachine struct {
	pb.UnimplementedStateMachineServer
	applies atomic.Int64
}

func (s *stubStateMachine) Apply(ctx context.Context, cmd *pb.Command) (*pb.ApplyResponse, error) {
	s.applies.Add(1)
	return &pb.ApplyResponse{Success: true}, nil
}

// backendServer is a gRPC StateMachine server that a test can start whenever it
// likes: immediately, or after a delay to stand in for a C++ engine that is
// still booting. Everything the test goroutine reads back is channel-delivered
// or atomic, so the suite stays clean under -race.
type backendServer struct {
	srv       *grpc.Server
	stub      *stubStateMachine
	bound     chan struct{}
	listenErr chan error
}

func newBackendServer(t *testing.T) *backendServer {
	t.Helper()

	s := &backendServer{
		srv:       grpc.NewServer(),
		stub:      &stubStateMachine{},
		bound:     make(chan struct{}),
		listenErr: make(chan error, 1),
	}
	pb.RegisterStateMachineServer(s.srv, s.stub)
	// Stop unblocks Serve; a Serve that has not started yet closes its listener
	// and returns ErrServerStopped, so neither ordering leaks anything.
	t.Cleanup(s.srv.Stop)
	return s
}

// startAfter binds addr and starts serving once delay has elapsed. It runs on
// its own goroutine and reports the bind outcome over channels, so it never
// touches *testing.T off the test goroutine.
func (s *backendServer) startAfter(addr string, delay time.Duration) {
	go func() {
		if delay > 0 {
			time.Sleep(delay)
		}

		lis, err := net.Listen("tcp", addr)
		if err != nil {
			s.listenErr <- err
			return
		}

		close(s.bound)
		_ = s.srv.Serve(lis)
	}()
}

// awaitBound blocks until the server has bound its address. It is safe to call
// more than once because bound is closed rather than sent to.
func (s *backendServer) awaitBound(t *testing.T) {
	t.Helper()

	select {
	case <-s.bound:
	case err := <-s.listenErr:
		t.Fatalf("backend server failed to listen: %v", err)
	case <-time.After(bindTimeout):
		t.Fatal("backend server never bound its address")
	}
}

// isLive reports whether the server has already bound its address.
func (s *backendServer) isLive() bool {
	select {
	case <-s.bound:
		return true
	default:
		return false
	}
}

// reserveLoopbackAddr binds 127.0.0.1:0 to learn a free port and releases it
// again, so a test decides when — or whether — something starts listening
// there. Reading the assigned port back beats guessing one.
func reserveLoopbackAddr(t *testing.T) string {
	t.Helper()

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("failed to reserve a loopback port: %v", err)
	}
	addr := lis.Addr().String()
	if err := lis.Close(); err != nil {
		t.Fatalf("failed to release the reserved loopback port %s: %v", addr, err)
	}
	return addr
}

// connectResult carries Connect's outcome back from the goroutine the tests run
// it on, so a hang surfaces as a test failure instead of a stuck suite.
type connectResult struct {
	client *Client
	err    error
}

func connectAsync(cfg *ConnectionConfig) <-chan connectResult {
	out := make(chan connectResult, 1)
	go func() {
		client, err := Connect(cfg)
		out <- connectResult{client: client, err: err}
	}()
	return out
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

func TestConnect(t *testing.T) {
	tests := []struct {
		name string
		// listenAfter < 0 means nothing ever listens on the reserved address; 0
		// means the backend is already serving before Connect is called.
		listenAfter time.Duration
		maxRetries  int
		retryDelay  time.Duration
		wantErr     bool
	}{
		{
			name:        "backend already listening",
			listenAfter: 0,
			maxRetries:  100,
			retryDelay:  50 * time.Millisecond,
			wantErr:     false,
		},
		{
			// The regression test for R1.9. grpc.NewClient (like the deprecated
			// grpc.Dial before it) returns a nil error against a dead address, so
			// asserting "err == nil" here would pass vacuously — the assertions
			// below therefore require the connection to be READY and the backend
			// to have come up first.
			name:        "late binding backend appears while Connect retries",
			listenAfter: 150 * time.Millisecond,
			maxRetries:  100,
			retryDelay:  50 * time.Millisecond,
			wantErr:     false,
		},
		{
			name:        "nothing ever listens",
			listenAfter: -1,
			maxRetries:  3,
			retryDelay:  20 * time.Millisecond,
			wantErr:     true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			addr := reserveLoopbackAddr(t)

			var srv *backendServer
			if tt.listenAfter >= 0 {
				srv = newBackendServer(t)
			}
			// The already-listening case must be serving before Connect starts;
			// the late-binding case must emphatically not be.
			if srv != nil && tt.listenAfter == 0 {
				srv.startAfter(addr, 0)
				srv.awaitBound(t)
			}

			cfg := &ConnectionConfig{
				Address:    addr,
				MaxRetries: tt.maxRetries,
				RetryDelay: tt.retryDelay,
			}

			// start is taken before either goroutine exists, so the listener can
			// only appear at start+listenAfter or later.
			start := time.Now()
			done := connectAsync(cfg)
			if srv != nil && tt.listenAfter > 0 {
				srv.startAfter(addr, tt.listenAfter)
			}

			var res connectResult
			select {
			case res = <-done:
			case <-time.After(connectWatchdog):
				t.Fatal("Connect never returned: the retry loop is not bounded")
			}
			elapsed := time.Since(start)

			if tt.wantErr {
				if res.err == nil {
					t.Fatalf("Connect() = %v, want an error when nothing listens on %s", res.client, addr)
				}
				if res.client != nil {
					t.Error("Connect() returned a client alongside its error")
				}
				for _, want := range []string{addr, fmt.Sprintf("after %d attempts", tt.maxRetries)} {
					if !strings.Contains(res.err.Error(), want) {
						t.Errorf("Connect() error = %q, want it to contain %q", res.err, want)
					}
				}
				// The point of R1.9: the attempts are real, so giving up cannot
				// happen before the configured schedule has elapsed.
				if minWait := time.Duration(tt.maxRetries) * tt.retryDelay; elapsed < minWait {
					t.Errorf("Connect() gave up after %v, want it to keep retrying for at least %v",
						elapsed, minWait)
				}
				return
			}

			if res.err != nil {
				t.Fatalf("Connect() error = %v, want nil", res.err)
			}
			if res.client == nil {
				t.Fatal("Connect() returned a nil client with a nil error")
			}
			t.Cleanup(func() {
				if err := res.client.Close(); err != nil {
					t.Errorf("Close() error = %v, want nil", err)
				}
			})

			// Readiness, not merely "no error": a lazily dialled connection would
			// still be IDLE here and the late-binding case would pass vacuously.
			if state := res.client.conn.GetState(); state != connectivity.Ready {
				t.Errorf("connection state = %s, want %s", state, connectivity.Ready)
			}
			if !srv.isLive() {
				t.Error("Connect() reported success before the backend ever started listening")
			}
			if elapsed < tt.listenAfter {
				t.Errorf("Connect() returned after %v, i.e. before the backend came up at %v",
					elapsed, tt.listenAfter)
			}
			if tt.listenAfter == 0 && elapsed > quickConnect {
				t.Errorf("Connect() took %v against an already-listening backend, want under %v",
					elapsed, quickConnect)
			}

			// A ready connection has to be a usable one.
			if res.client.StateMachineClient == nil {
				t.Fatal("Connect() returned a client without a StateMachineClient")
			}

			ctx, cancel := context.WithTimeout(context.Background(), rpcTimeout)
			defer cancel()

			resp, err := res.client.StateMachineClient.Apply(ctx, &pb.Command{Data: []byte("payload")})
			if err != nil {
				t.Fatalf("Apply() error = %v, want nil", err)
			}
			if !resp.GetSuccess() {
				t.Error("Apply() success = false, want true")
			}
			if n := srv.stub.applies.Load(); n != 1 {
				t.Errorf("backend saw %d Apply calls, want exactly 1", n)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Config and Close
// ---------------------------------------------------------------------------

func TestDefaultConnectionConfig(t *testing.T) {
	cfg := DefaultConnectionConfig("localhost:50051")

	if cfg.Address != "localhost:50051" {
		t.Errorf("Address = %q, want %q", cfg.Address, "localhost:50051")
	}
	// Pinned defaults: 15 attempts spaced 1s apart, i.e. a ~15s startup window
	// for the C++ engine.
	if cfg.MaxRetries != 15 {
		t.Errorf("MaxRetries = %d, want 15", cfg.MaxRetries)
	}
	if cfg.RetryDelay != time.Second {
		t.Errorf("RetryDelay = %v, want 1s", cfg.RetryDelay)
	}
}

func TestClientCloseWithoutConnection(t *testing.T) {
	if err := (&Client{}).Close(); err != nil {
		t.Errorf("Close() error = %v, want nil for a client with no connection", err)
	}
}
