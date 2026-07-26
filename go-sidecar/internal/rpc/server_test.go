package rpc

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/hashicorp/raft"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	"my-raft-sidecar/internal/fsm"
	"my-raft-sidecar/internal/raftnode"
	pb "my-raft-sidecar/pb"
)

// TestMain silences the package's log output: Propose logs on every failure
// path and the error-path tests would otherwise spam the test log.
func TestMain(m *testing.M) {
	log.SetOutput(io.Discard)
	code := m.Run()
	log.SetOutput(os.Stderr)
	os.Exit(code)
}

// Compile-time proof that the real Raft node satisfies the consumer-side
// interface this package declares.
//
// This assertion deliberately lives in the rpc package's test file rather than
// in package raftnode: putting it there would force raftnode to import rpc and
// invert the dependency direction the RaftProposer interface exists to break.
// The other (production) guarantee is the call site in cmd/sidecar/main.go,
// which passes a *raftnode.Node to NewServer.
var _ RaftProposer = (*raftnode.Node)(nil)

// applyCall records the arguments of a single Apply invocation.
type applyCall struct {
	data    []byte
	timeout time.Duration
}

// fakeProposer is a configurable stand-in for the Raft node.
//
// Most tests call Propose directly from the test goroutine, but
// TestStartServesProposeAndStops drives it over a real socket, so Apply runs on
// a gRPC serving goroutine while the test goroutine reads the recorded calls.
// A completed RPC is not a language-level happens-before edge, so the recorder
// is mutex-guarded — same reasoning as fakeRaftControl in
// internal/management/server_test.go.
//
// resp/err/leaderAddr are configured before Start and never written afterwards,
// so they need no lock.
type fakeProposer struct {
	resp       interface{}
	err        error
	leaderAddr string

	mu    sync.Mutex
	calls []applyCall
}

var _ RaftProposer = (*fakeProposer)(nil)

func (f *fakeProposer) Apply(data []byte, timeout time.Duration) (interface{}, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, applyCall{data: data, timeout: timeout})
	return f.resp, f.err
}

func (f *fakeProposer) LeaderAddr() string { return f.leaderAddr }

// applyCallsSnapshot returns a copy of the recorded calls, safe to read from
// any goroutine.
func (f *fakeProposer) applyCallsSnapshot() []applyCall {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]applyCall(nil), f.calls...)
}

// ---------------------------------------------------------------------------
// Propose
// ---------------------------------------------------------------------------

// TestPropose covers the three failure shapes Propose must keep distinguishable
// (R1.5), plus the success path.
//
// Two contracts are asserted on every case:
//   - the gRPC error is always nil, because the C++ client checks
//     reply.success() and would see a transport failure instead of the reason;
//   - only a genuine not-leader rejection carries NotLeaderPrefix. The C++ HTTP
//     layer turns that prefix into a 503 naming the leader, and Phase 4 builds
//     request forwarding on it, so leaking it onto any other failure would
//     make clients retry a write that will never succeed elsewhere.
func TestPropose(t *testing.T) {
	genericErr := errors.New("timed out enqueuing operation")
	fsmErr := &fsm.ApplyError{
		Index:  9,
		Term:   2,
		Reason: "invalid command: unknown op FOO",
	}

	tests := []struct {
		name       string
		resp       interface{}
		err        error
		leaderAddr string

		wantSuccess bool
		wantError   string
	}{
		{
			name:        "commit and apply both succeed",
			resp:        nil,
			err:         nil,
			leaderAddr:  "node1:8088",
			wantSuccess: true,
			wantError:   "",
		},
		{
			name:        "not the leader names the current leader",
			resp:        nil,
			err:         raft.ErrNotLeader,
			leaderAddr:  "node1:8088",
			wantSuccess: false,
			wantError:   "not_leader:node1:8088",
		},
		{
			// LeaderAddr() is empty during an election. The prefix must still
			// be emitted so the caller can tell "retry elsewhere, address
			// unknown" apart from "this write is broken".
			name:        "not the leader with no leader elected yet",
			resp:        nil,
			err:         raft.ErrNotLeader,
			leaderAddr:  "",
			wantSuccess: false,
			wantError:   "not_leader:",
		},
		{
			name:        "wrapped ErrNotLeader is still recognised",
			resp:        nil,
			err:         fmt.Errorf("raft apply: %w", raft.ErrNotLeader),
			leaderAddr:  "node2:8088",
			wantSuccess: false,
			wantError:   "not_leader:node2:8088",
		},
		{
			// The entry never committed, but not because of leadership. The
			// message is passed through verbatim and must stay untagged.
			name:        "other raft error is reported verbatim",
			resp:        nil,
			err:         genericErr,
			leaderAddr:  "node1:8088",
			wantSuccess: false,
			wantError:   "timed out enqueuing operation",
		},
		{
			// The entry DID commit and replicate; this node's state machine
			// refused it. Previously invisible: Propose only looked at
			// future.Error(), which is nil here.
			name:        "committed entry rejected by the state machine",
			resp:        fsmErr,
			err:         nil,
			leaderAddr:  "node1:8088",
			wantSuccess: false,
			wantError:   "fsm: failed to apply raft log entry index=9 term=2: invalid command: unknown op FOO",
		},
		{
			name:        "plain error from the FSM is reported verbatim",
			resp:        errors.New("state machine unreachable"),
			err:         nil,
			leaderAddr:  "node1:8088",
			wantSuccess: false,
			wantError:   "state machine unreachable",
		},
		{
			// The FSM contract is "nil means applied", but the response is an
			// interface{}: anything that is not an error must not be mistaken
			// for a failure.
			name:        "non-error response value is treated as success",
			resp:        "applied",
			err:         nil,
			leaderAddr:  "node1:8088",
			wantSuccess: true,
			wantError:   "",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			node := &fakeProposer{
				resp:       tc.resp,
				err:        tc.err,
				leaderAddr: tc.leaderAddr,
			}
			server := NewServer(node)

			payload := []byte{0x83, 0xa2, 'o', 'p'}
			resp, err := server.Propose(context.Background(), &pb.Command{Data: payload})

			// Failures live in the body, never in the gRPC status.
			if err != nil {
				t.Fatalf("Propose() gRPC error = %v, want nil", err)
			}
			if resp == nil {
				t.Fatal("Propose() returned a nil response")
			}
			if resp.GetSuccess() != tc.wantSuccess {
				t.Errorf("Success = %v, want %v", resp.GetSuccess(), tc.wantSuccess)
			}
			if got := resp.GetError(); got != tc.wantError {
				t.Errorf("Error = %q, want %q", got, tc.wantError)
			}

			wantTagged := strings.HasPrefix(tc.wantError, NotLeaderPrefix)
			if gotTagged := strings.HasPrefix(resp.GetError(), NotLeaderPrefix); gotTagged != wantTagged {
				t.Errorf("Error %q carries the %q prefix = %v, want %v",
					resp.GetError(), NotLeaderPrefix, gotTagged, wantTagged)
			}

			wantCalls := []applyCall{{data: payload, timeout: proposeTimeout}}
			if got := node.applyCallsSnapshot(); !reflect.DeepEqual(got, wantCalls) {
				t.Errorf("Apply calls = %+v, want %+v", got, wantCalls)
			}
		})
	}
}

// TestNotLeaderPrefixIsStable guards the exact wire token the C++ HTTP layer
// and Phase 4 forwarding key off. Renaming it is a cross-language break.
func TestNotLeaderPrefixIsStable(t *testing.T) {
	if NotLeaderPrefix != "not_leader:" {
		t.Fatalf("NotLeaderPrefix = %q, want %q", NotLeaderPrefix, "not_leader:")
	}
}

// TestProposeForwardsPayloadOpaquely pins that the MsgPack body is handed to
// Raft byte-for-byte: the Go side never parses it (see
// .claude/rules/protobuf.md), and the typed proto fields are unused in transit.
func TestProposeForwardsPayloadOpaquely(t *testing.T) {
	// MsgPack map {op: "SET", key: "k=\nz", value: bin(0xff 0x00)} — neither
	// line-safe nor valid UTF-8, so any re-encoding would show up here.
	payload := []byte{
		0x83,
		0xa2, 'o', 'p', 0xa3, 'S', 'E', 'T',
		0xa3, 'k', 'e', 'y', 0xa4, 'k', '=', '\n', 'z',
		0xa5, 'v', 'a', 'l', 'u', 'e', 0xc4, 0x02, 0xff, 0x00,
	}

	node := &fakeProposer{}
	server := NewServer(node)

	resp, err := server.Propose(context.Background(), &pb.Command{
		Data: payload,
		// Set the typed fields to prove they are ignored: only Data travels.
		Op:  "SET",
		Key: "ignored",
	})
	if err != nil {
		t.Fatalf("Propose() gRPC error = %v, want nil", err)
	}
	if !resp.GetSuccess() {
		t.Fatalf("Success = false (error %q), want true", resp.GetError())
	}

	calls := node.applyCallsSnapshot()
	if len(calls) != 1 {
		t.Fatalf("Apply called %d times, want exactly 1", len(calls))
	}
	if !reflect.DeepEqual(calls[0].data, payload) {
		t.Errorf("Apply data = % x, want % x", calls[0].data, payload)
	}
}

// TestProposeWithNilCommand covers the defensive path: cmd.GetData() must be
// used rather than cmd.Data so a nil message cannot panic the sidecar.
func TestProposeWithNilCommand(t *testing.T) {
	node := &fakeProposer{}
	server := NewServer(node)

	resp, err := server.Propose(context.Background(), nil)
	if err != nil {
		t.Fatalf("Propose() gRPC error = %v, want nil", err)
	}
	if !resp.GetSuccess() {
		t.Fatalf("Success = false (error %q), want true", resp.GetError())
	}

	calls := node.applyCallsSnapshot()
	if len(calls) != 1 {
		t.Fatalf("Apply called %d times, want exactly 1", len(calls))
	}
	if len(calls[0].data) != 0 {
		t.Errorf("Apply data = % x, want empty", calls[0].data)
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// TestStopWithoutStart covers the shutdown path on a server that never served:
// main.go's signal handler can fire before Start binds the port.
func TestStopWithoutStart(t *testing.T) {
	NewServer(&fakeProposer{}).Stop()
}

// TestStartRejectsBadPort proves Start surfaces a bind failure as a wrapped
// error instead of returning nil or panicking.
func TestStartRejectsBadPort(t *testing.T) {
	err := NewServer(&fakeProposer{}).Start("not-a-port")
	if err == nil {
		t.Fatal("Start() with a non-numeric port returned nil, want an error")
	}
	if !strings.Contains(err.Error(), "failed to listen") {
		t.Errorf("Start() error = %v, want it to mention the listen failure", err)
	}
}

// freePort asks the kernel for an unused TCP port and immediately releases it.
//
// Start() builds its own listener from ":"+port, so there is no way to hand it
// a pre-bound listener or to ask which ephemeral port it landed on. Probing and
// then reusing the port is the standard workaround; the window between close
// and re-bind is small enough not to be a practical flake source, and a
// collision would surface as a clear bind error rather than a wrong result.
//
// The probe binds the wildcard address deliberately, matching what Start does:
// a port that is free on loopback is not proof the wildcard address is free.
func freePort(t *testing.T) string {
	t.Helper()

	listener, err := net.Listen("tcp", ":0")
	if err != nil {
		t.Fatalf("reserving a free port: %v", err)
	}
	_, port, err := net.SplitHostPort(listener.Addr().String())
	if err != nil {
		listener.Close()
		t.Fatalf("splitting %q: %v", listener.Addr().String(), err)
	}
	if err := listener.Close(); err != nil {
		t.Fatalf("releasing the reserved port: %v", err)
	}
	return port
}

// TestStartServesProposeAndStops is the only test that proves RaftNode/Propose
// is actually registered and reachable over the wire, and that a failure still
// arrives as a nil gRPC error with the reason in the body — the contract the
// C++ GrpcRaftClient depends on.
func TestStartServesProposeAndStops(t *testing.T) {
	node := &fakeProposer{err: raft.ErrNotLeader, leaderAddr: "node1:8088"}
	server := NewServer(node)
	port := freePort(t)

	serveErr := make(chan error, 1)
	go func() { serveErr <- server.Start(port) }()

	stopped := false
	t.Cleanup(func() {
		if !stopped {
			server.Stop()
		}
	})

	// passthrough:// keeps the target literal instead of routing it through
	// the default DNS resolver.
	conn, err := grpc.NewClient(
		"passthrough:///127.0.0.1:"+port,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("creating gRPC client: %v", err)
	}
	defer conn.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Start serves in a goroutine, so the listener may not be accepting yet.
	// WaitForReady blocks the call on the connection instead of failing fast
	// on a transient refusal — a deterministic wait rather than a sleep.
	resp, err := pb.NewRaftNodeClient(conn).Propose(
		ctx,
		&pb.Command{Data: []byte("payload")},
		grpc.WaitForReady(true),
	)
	if err != nil {
		t.Fatalf("Propose over the wire: gRPC error = %v, want nil", err)
	}
	if resp.GetSuccess() {
		t.Error("Success = true, want false (the fake node is not the leader)")
	}
	if got, want := resp.GetError(), "not_leader:node1:8088"; got != want {
		t.Errorf("Error = %q, want %q", got, want)
	}

	// The RPC must have reached the node, proving the service registration is
	// real and not a stub answering.
	if calls := node.applyCallsSnapshot(); len(calls) != 1 {
		t.Fatalf("Apply called %d times, want exactly 1", len(calls))
	}

	// Drop the client transport before stopping: GracefulStop blocks until
	// every open connection has drained, so a live client would stall it. The
	// deferred Close above still runs on the t.Fatalf paths, and it runs before
	// the t.Cleanup that stops the server, so that ordering holds either way.
	_ = conn.Close()

	stopped = true
	server.Stop()

	select {
	case err := <-serveErr:
		if err != nil {
			t.Errorf("Start() returned %v after Stop(), want nil", err)
		}
	case <-time.After(10 * time.Second):
		t.Error("Start() did not return after Stop()")
	}
}
