package management

import (
	"context"
	"errors"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"reflect"
	"sync"
	"testing"
	"time"

	"my-raft-sidecar/internal/raftnode"
)

// Compile-time proof that the real Raft node satisfies the consumer-side
// interface this package declares.
//
// This assertion deliberately lives in the management package's test file
// rather than in package raftnode: putting it there would force raftnode to
// import management and invert the dependency direction the RaftControl
// interface exists to break. The other (production) guarantee is the call site
// in cmd/sidecar/main.go, which passes a *raftnode.Node to NewServer.
var _ RaftControl = (*raftnode.Node)(nil)

// addVoterCall records the arguments of a single AddVoter invocation.
type addVoterCall struct {
	id      string
	address string
}

// fakeRaftControl is a configurable stand-in for the Raft node.
//
// Most tests call the handlers directly from the test goroutine, but
// TestStartServesAndStops drives them over a real socket, so the methods run on
// net/http serving goroutines while the test goroutine reads the recorded
// calls. A completed HTTP round trip is not a language-level happens-before
// edge, so the recorder is mutex-guarded — same reasoning as joinRecorder in
// internal/cluster/joiner_test.go.
//
// isLeader/leaderAddr/addVoterErr are configured before Start and never
// written afterwards, so they need no lock.
type fakeRaftControl struct {
	isLeader    bool
	leaderAddr  string
	addVoterErr error

	mu            sync.Mutex
	addVoterCalls []addVoterCall
}

var _ RaftControl = (*fakeRaftControl)(nil)

func (f *fakeRaftControl) IsLeader() bool { return f.isLeader }

func (f *fakeRaftControl) LeaderAddr() string { return f.leaderAddr }

func (f *fakeRaftControl) AddVoter(id, address string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.addVoterCalls = append(f.addVoterCalls, addVoterCall{id: id, address: address})
	return f.addVoterErr
}

// addVoterCallsSnapshot returns a copy of the recorded calls, safe to read from
// any goroutine.
func (f *fakeRaftControl) addVoterCallsSnapshot() []addVoterCall {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]addVoterCall(nil), f.addVoterCalls...)
}

// TestHandleJoin exercises /join. The handlers are unexported methods, so this
// is an in-package test that calls them directly; Start() is never used because
// it binds a real port.
//
// PIN: /join is unauthenticated and accepts its parameters from the query
// string on both GET and POST. Phase 6 (R6.1) puts bearer-token auth in front
// of it. Until then these tests describe the current contract, which
// cluster.Joiner depends on (it joins with a plain GET).
func TestHandleJoin(t *testing.T) {
	errNotLeader := errors.New("node is not the leader")

	tests := []struct {
		name        string
		method      string
		target      string
		addVoterErr error
		wantStatus  int
		wantBody    string
		wantCalls   []addVoterCall
	}{
		{
			name:        "GET with both params adds the voter",
			method:      http.MethodGet,
			target:      "/join?peerID=node2&peerAddress=10.0.0.2:8088",
			addVoterErr: nil,
			wantStatus:  http.StatusOK,
			wantBody:    "Joined successfully",
			wantCalls:   []addVoterCall{{id: "node2", address: "10.0.0.2:8088"}},
		},
		{
			name:        "POST with both params adds the voter",
			method:      http.MethodPost,
			target:      "/join?peerID=node3&peerAddress=10.0.0.3:8088",
			addVoterErr: nil,
			wantStatus:  http.StatusOK,
			wantBody:    "Joined successfully",
			wantCalls:   []addVoterCall{{id: "node3", address: "10.0.0.3:8088"}},
		},
		{
			name:        "PUT is rejected",
			method:      http.MethodPut,
			target:      "/join?peerID=node2&peerAddress=10.0.0.2:8088",
			addVoterErr: nil,
			wantStatus:  http.StatusMethodNotAllowed,
			wantBody:    "Method not allowed\n",
			wantCalls:   nil,
		},
		{
			name:        "DELETE is rejected",
			method:      http.MethodDelete,
			target:      "/join?peerID=node2&peerAddress=10.0.0.2:8088",
			addVoterErr: nil,
			wantStatus:  http.StatusMethodNotAllowed,
			wantBody:    "Method not allowed\n",
			wantCalls:   nil,
		},
		{
			name:        "missing peerID is rejected",
			method:      http.MethodGet,
			target:      "/join?peerAddress=10.0.0.2:8088",
			addVoterErr: nil,
			wantStatus:  http.StatusBadRequest,
			wantBody:    "Missing peerAddress or peerID\n",
			wantCalls:   nil,
		},
		{
			name:        "empty peerID is rejected",
			method:      http.MethodGet,
			target:      "/join?peerID=&peerAddress=10.0.0.2:8088",
			addVoterErr: nil,
			wantStatus:  http.StatusBadRequest,
			wantBody:    "Missing peerAddress or peerID\n",
			wantCalls:   nil,
		},
		{
			name:        "missing peerAddress is rejected",
			method:      http.MethodGet,
			target:      "/join?peerID=node2",
			addVoterErr: nil,
			wantStatus:  http.StatusBadRequest,
			wantBody:    "Missing peerAddress or peerID\n",
			wantCalls:   nil,
		},
		{
			name:        "both params missing is rejected",
			method:      http.MethodPost,
			target:      "/join",
			addVoterErr: nil,
			wantStatus:  http.StatusBadRequest,
			wantBody:    "Missing peerAddress or peerID\n",
			wantCalls:   nil,
		},
		{
			name:        "AddVoter failure is reported as 500 with the error text",
			method:      http.MethodGet,
			target:      "/join?peerID=node2&peerAddress=10.0.0.2:8088",
			addVoterErr: errNotLeader,
			wantStatus:  http.StatusInternalServerError,
			wantBody:    "node is not the leader\n",
			wantCalls:   []addVoterCall{{id: "node2", address: "10.0.0.2:8088"}},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			node := &fakeRaftControl{addVoterErr: tc.addVoterErr}
			server := NewServer(node, "6000")

			rec := httptest.NewRecorder()
			server.handleJoin(rec, httptest.NewRequest(tc.method, tc.target, nil))

			if rec.Code != tc.wantStatus {
				t.Errorf("status = %d, want %d", rec.Code, tc.wantStatus)
			}
			if got := rec.Body.String(); got != tc.wantBody {
				t.Errorf("body = %q, want %q", got, tc.wantBody)
			}
			if got := node.addVoterCallsSnapshot(); !reflect.DeepEqual(got, tc.wantCalls) {
				t.Errorf("AddVoter calls = %+v, want %+v", got, tc.wantCalls)
			}
		})
	}
}

// TestHandleStatus pins the exact bytes produced by the current hand-rolled
// fmt.Fprintf implementation, including the fact that the leader address is
// quoted with Go's %q verb rather than by encoding/json.
//
// PIN: Phase 5 (R5.6) rewrites this handler with encoding/json and extends the
// payload; these expectations are meant to change there, not before.
func TestHandleStatus(t *testing.T) {
	tests := []struct {
		name       string
		isLeader   bool
		leaderAddr string
		wantBody   string
	}{
		{
			name:       "leader",
			isLeader:   true,
			leaderAddr: "10.0.0.1:8088",
			wantBody:   `{"is_leader": true, "leader_addr": "10.0.0.1:8088"}`,
		},
		{
			name:       "follower with a known leader",
			isLeader:   false,
			leaderAddr: "10.0.0.1:8088",
			wantBody:   `{"is_leader": false, "leader_addr": "10.0.0.1:8088"}`,
		},
		{
			name:       "follower with no leader elected yet",
			isLeader:   false,
			leaderAddr: "",
			wantBody:   `{"is_leader": false, "leader_addr": ""}`,
		},
		{
			// %q applies Go quoting, not JSON escaping: the double quote is
			// backslash-escaped, but "<" is passed through verbatim, whereas
			// encoding/json (HTML-escaping by default) would emit it as a
			// numeric unicode escape. Pinned so Phase 5's rewrite shows here.
			name:       "address is escaped with Go quoting",
			isLeader:   false,
			leaderAddr: `a"b<c`,
			wantBody:   `{"is_leader": false, "leader_addr": "a\"b<c"}`,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			node := &fakeRaftControl{isLeader: tc.isLeader, leaderAddr: tc.leaderAddr}
			server := NewServer(node, "6000")

			rec := httptest.NewRecorder()
			server.handleStatus(rec, httptest.NewRequest(http.MethodGet, "/status", nil))

			if rec.Code != http.StatusOK {
				t.Errorf("status = %d, want %d", rec.Code, http.StatusOK)
			}
			if got := rec.Header().Get("Content-Type"); got != "application/json" {
				t.Errorf("Content-Type = %q, want %q", got, "application/json")
			}
			if got := rec.Body.String(); got != tc.wantBody {
				t.Errorf("body = %q, want %q", got, tc.wantBody)
			}
		})
	}

	// PIN: /status does not check the request method; every verb is served.
	t.Run("any method is served", func(t *testing.T) {
		node := &fakeRaftControl{isLeader: true, leaderAddr: "10.0.0.1:8088"}
		server := NewServer(node, "6000")

		rec := httptest.NewRecorder()
		server.handleStatus(rec, httptest.NewRequest(http.MethodDelete, "/status", nil))

		want := `{"is_leader": true, "leader_addr": "10.0.0.1:8088"}`
		if rec.Code != http.StatusOK {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusOK)
		}
		if got := rec.Body.String(); got != want {
			t.Errorf("body = %q, want %q", got, want)
		}
	})
}

// TestHandleHealth pins that /health is unconditionally 200 "OK": it never
// consults the Raft node, so a node with no elected leader still reports
// healthy.
//
// PIN: Phase 5 (R5.5) keeps /health as pure process liveness and adds a
// separate /ready endpoint that actually inspects Raft and backend state, so
// this handler's behavior is expected to stay as asserted here.
func TestHandleHealth(t *testing.T) {
	tests := []struct {
		name   string
		node   *fakeRaftControl
		method string
	}{
		{
			name:   "leader",
			node:   &fakeRaftControl{isLeader: true, leaderAddr: "10.0.0.1:8088"},
			method: http.MethodGet,
		},
		{
			name:   "follower",
			node:   &fakeRaftControl{isLeader: false, leaderAddr: "10.0.0.1:8088"},
			method: http.MethodGet,
		},
		{
			name:   "no leader elected",
			node:   &fakeRaftControl{},
			method: http.MethodGet,
		},
		{
			name:   "non-GET method",
			node:   &fakeRaftControl{},
			method: http.MethodPost,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			server := NewServer(tc.node, "6000")

			rec := httptest.NewRecorder()
			server.handleHealth(rec, httptest.NewRequest(tc.method, "/health", nil))

			if rec.Code != http.StatusOK {
				t.Errorf("status = %d, want %d", rec.Code, http.StatusOK)
			}
			if got := rec.Body.String(); got != "OK" {
				t.Errorf("body = %q, want %q", got, "OK")
			}
		})
	}
}

// TestStopWithoutStart covers the nil-httpServer branch: Stop must be safe on a
// server that was never started (the shutdown path runs even if Start failed).
func TestStopWithoutStart(t *testing.T) {
	server := NewServer(&fakeRaftControl{}, "6000")

	if err := server.Stop(context.Background()); err != nil {
		t.Fatalf("Stop() on a never-started server: %v", err)
	}
}

// freePort asks the kernel for an unused TCP port and immediately releases it.
//
// Start() builds its own listener from "0.0.0.0:"+port, so there is no way to
// hand it a pre-bound listener or to ask it which ephemeral port it landed on.
// Probing for a free port and then reusing it is the standard workaround; the
// window between close and re-bind is small enough not to be a practical flake
// source, and a collision would surface as a clear bind error rather than a
// wrong result.
//
// The probe binds 0.0.0.0 deliberately: Start() binds "0.0.0.0:"+port, and a
// free port on loopback does not prove the wildcard address is free.
func freePort(t *testing.T) string {
	t.Helper()

	listener, err := net.Listen("tcp", "0.0.0.0:0")
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

// TestStartServesAndStops covers the real lifecycle: Start binds the port and
// serves the routes, and Stop shuts it down so the port stops answering.
//
// The handler tests above call the methods directly, which leaves Start's
// mux wiring untested — this is the only test that proves /join, /status and
// /health are actually registered on the paths clients use.
func TestStartServesAndStops(t *testing.T) {
	node := &fakeRaftControl{isLeader: true, leaderAddr: "node1:8088"}
	port := freePort(t)
	server := NewServer(node, port)

	server.Start()
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = server.Stop(ctx)
	})

	baseURL := "http://127.0.0.1:" + port
	client := &http.Client{Timeout: 2 * time.Second}

	// Start serves in a goroutine, so the listener may not be accepting yet.
	// Poll rather than sleep.
	var lastErr error
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		resp, err := client.Get(baseURL + "/health")
		if err == nil {
			resp.Body.Close()
			lastErr = nil
			break
		}
		lastErr = err
		time.Sleep(10 * time.Millisecond)
	}
	if lastErr != nil {
		t.Fatalf("management server never became reachable on %s: %v", baseURL, lastErr)
	}

	// Every route Start registers must be reachable at its real path.
	for _, route := range []struct {
		path     string
		wantBody string
	}{
		{"/health", "OK"},
		{"/status", `{"is_leader": true, "leader_addr": "node1:8088"}`},
		{"/join?peerID=node2&peerAddress=node2:8088", "Joined successfully"},
	} {
		resp, err := client.Get(baseURL + route.path)
		if err != nil {
			t.Fatalf("GET %s: %v", route.path, err)
		}
		body, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			t.Fatalf("reading %s body: %v", route.path, err)
		}
		if resp.StatusCode != http.StatusOK {
			t.Errorf("GET %s status = %d, want %d", route.path, resp.StatusCode, http.StatusOK)
		}
		if got := string(body); got != route.wantBody {
			t.Errorf("GET %s body = %q, want %q", route.path, got, route.wantBody)
		}
	}

	// The /join above must have reached the node, proving the mux wiring is
	// real and not just a 200 from some catch-all.
	wantCalls := []addVoterCall{{id: "node2", address: "node2:8088"}}
	if got := node.addVoterCallsSnapshot(); !reflect.DeepEqual(got, wantCalls) {
		t.Errorf("AddVoter calls = %+v, want %+v", got, wantCalls)
	}

	// Stop must actually close the listener.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := server.Stop(ctx); err != nil {
		t.Fatalf("Stop(): %v", err)
	}

	deadline = time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		resp, err := client.Get(baseURL + "/health")
		if err != nil {
			return // refused, as expected
		}
		resp.Body.Close()
		time.Sleep(10 * time.Millisecond)
	}
	t.Errorf("management server still answering on %s after Stop()", baseURL)
}
