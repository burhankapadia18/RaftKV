package management

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"my-raft-sidecar/internal/raftnode"
)

// TestMain silences the handlers' log chatter: /join logs every request and
// /status logs a failed log-store read, both of which these tests trigger on
// purpose.
func TestMain(m *testing.M) {
	log.SetOutput(io.Discard)
	code := m.Run()
	log.SetOutput(os.Stderr)
	os.Exit(code)
}

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
// isLeader/leaderAddr/addVoterErr/stats/firstLogIndex/firstLogErr are
// configured before Start and never written afterwards, so they need no lock.
type fakeRaftControl struct {
	isLeader    bool
	leaderAddr  string
	addVoterErr error

	// stats stands in for raft.Raft.Stats(). A nil map is a legitimate case:
	// it is what a status poll sees if the library ever renames these keys.
	stats         map[string]string
	firstLogIndex uint64
	firstLogErr   error

	mu            sync.Mutex
	addVoterCalls []addVoterCall
}

var _ RaftControl = (*fakeRaftControl)(nil)

func (f *fakeRaftControl) IsLeader() bool { return f.isLeader }

func (f *fakeRaftControl) LeaderAddr() string { return f.leaderAddr }

func (f *fakeRaftControl) Stats() map[string]string { return f.stats }

func (f *fakeRaftControl) FirstLogIndex() (uint64, error) {
	if f.firstLogErr != nil {
		return 0, f.firstLogErr
	}
	return f.firstLogIndex, nil
}

// raftStats builds a stats map with the four keys /status reads, in the decimal
// string form raft.Raft.Stats() produces.
func raftStats(lastLog, applied, commit, lastSnapshot uint64) map[string]string {
	return map[string]string{
		"last_log_index":      strconv.FormatUint(lastLog, 10),
		"applied_index":       strconv.FormatUint(applied, 10),
		"commit_index":        strconv.FormatUint(commit, 10),
		"last_snapshot_index": strconv.FormatUint(lastSnapshot, 10),
		// Raft reports a good deal more than this; carrying one extra key
		// proves the handler projects the ones it wants rather than dumping
		// the map.
		"state": "Leader",
	}
}

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

// TestHandleStatus pins the exact bytes of the /status payload.
//
// PIN: Phase 3 (R3.6) rewrote this handler with encoding/json and added the
// five index fields. The earlier pins described a hand-rolled fmt.Fprintf body
// with a space after each colon and Go %q quoting, and said Phase 5 (R5.6)
// would make this change — Phase 3 got there first, because compaction is only
// demonstrable if the log indices are exposed, and seven fields hand-formatted
// with %v/%q is exactly the liability encoding/json exists to remove.
//
// Two consequences of the switch are pinned deliberately below:
//   - no space after the colons (compact encoding/json output);
//   - JSON escaping rather than Go quoting, so "<" becomes "\u003c"
//     (encoding/json HTML-escapes by default and that default is kept).
func TestHandleStatus(t *testing.T) {
	tests := []struct {
		name     string
		node     *fakeRaftControl
		wantBody string
	}{
		{
			name: "leader with a compacted log",
			node: &fakeRaftControl{
				isLeader:      true,
				leaderAddr:    "10.0.0.1:8088",
				stats:         raftStats(9000, 9000, 9000, 8500),
				firstLogIndex: 8001,
			},
			wantBody: `{"is_leader":true,"leader_addr":"10.0.0.1:8088",` +
				`"first_log_index":8001,"last_log_index":9000,"applied_index":9000,` +
				`"commit_index":9000,"last_snapshot_index":8500}`,
		},
		{
			name: "follower lagging behind the commit index",
			node: &fakeRaftControl{
				isLeader:      false,
				leaderAddr:    "10.0.0.1:8088",
				stats:         raftStats(120, 100, 118, 0),
				firstLogIndex: 1,
			},
			wantBody: `{"is_leader":false,"leader_addr":"10.0.0.1:8088",` +
				`"first_log_index":1,"last_log_index":120,"applied_index":100,` +
				`"commit_index":118,"last_snapshot_index":0}`,
		},
		{
			name: "fresh node with no leader and an empty log",
			node: &fakeRaftControl{
				stats: raftStats(0, 0, 0, 0),
			},
			wantBody: `{"is_leader":false,"leader_addr":"",` +
				`"first_log_index":0,"last_log_index":0,"applied_index":0,` +
				`"commit_index":0,"last_snapshot_index":0}`,
		},
		{
			// PIN: JSON escaping, not Go quoting. The quote is backslashed by
			// both, but "<" only becomes a numeric escape under encoding/json.
			name: "address is escaped by encoding/json",
			node: &fakeRaftControl{
				leaderAddr: `a"b<c`,
				stats:      raftStats(0, 0, 0, 0),
			},
			wantBody: `{"is_leader":false,"leader_addr":"a\"b\u003cc",` +
				`"first_log_index":0,"last_log_index":0,"applied_index":0,` +
				`"commit_index":0,"last_snapshot_index":0}`,
		},
		{
			// PIN: unknown/absent stat keys read as 0 rather than failing the
			// whole poll. This is what a raft upgrade that renames a key would
			// look like from outside.
			name: "missing stat keys degrade to zero",
			node: &fakeRaftControl{
				isLeader:      true,
				leaderAddr:    "node1:8088",
				stats:         map[string]string{"last_log_index": "42"},
				firstLogIndex: 7,
			},
			wantBody: `{"is_leader":true,"leader_addr":"node1:8088",` +
				`"first_log_index":7,"last_log_index":42,"applied_index":0,` +
				`"commit_index":0,"last_snapshot_index":0}`,
		},
		{
			// PIN: a nil stats map is survivable — every index reads 0.
			name: "nil stats map is survivable",
			node: &fakeRaftControl{isLeader: true, leaderAddr: "node1:8088"},
			wantBody: `{"is_leader":true,"leader_addr":"node1:8088",` +
				`"first_log_index":0,"last_log_index":0,"applied_index":0,` +
				`"commit_index":0,"last_snapshot_index":0}`,
		},
		{
			// PIN: a non-numeric stat value is treated like a missing one.
			name: "unparseable stat value degrades to zero",
			node: &fakeRaftControl{
				stats: map[string]string{
					"last_log_index":      "not-a-number",
					"applied_index":       "-1",
					"commit_index":        "3",
					"last_snapshot_index": "",
				},
			},
			wantBody: `{"is_leader":false,"leader_addr":"",` +
				`"first_log_index":0,"last_log_index":0,"applied_index":0,` +
				`"commit_index":3,"last_snapshot_index":0}`,
		},
		{
			// PIN: a log-store failure is reported in-band. /status still
			// answers 200 with everything it does know, because the CI and e2e
			// readiness polls depend on it answering at all.
			name: "log store failure is reported as a field, not a 500",
			node: &fakeRaftControl{
				isLeader:      true,
				leaderAddr:    "node1:8088",
				stats:         raftStats(50, 50, 50, 40),
				firstLogIndex: 41,
				firstLogErr:   errors.New("database not open"),
			},
			wantBody: `{"is_leader":true,"leader_addr":"node1:8088",` +
				`"first_log_index":0,"last_log_index":50,"applied_index":50,` +
				`"commit_index":50,"last_snapshot_index":40,` +
				`"log_store_error":"database not open"}`,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			server := NewServer(tc.node, "6000")

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
		node := &fakeRaftControl{
			isLeader:      true,
			leaderAddr:    "10.0.0.1:8088",
			stats:         raftStats(5, 5, 5, 0),
			firstLogIndex: 1,
		}
		server := NewServer(node, "6000")

		rec := httptest.NewRecorder()
		server.handleStatus(rec, httptest.NewRequest(http.MethodDelete, "/status", nil))

		want := `{"is_leader":true,"leader_addr":"10.0.0.1:8088",` +
			`"first_log_index":1,"last_log_index":5,"applied_index":5,` +
			`"commit_index":5,"last_snapshot_index":0}`
		if rec.Code != http.StatusOK {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusOK)
		}
		if got := rec.Body.String(); got != want {
			t.Errorf("body = %q, want %q", got, want)
		}
	})

	// PIN: the log_store_error field is absent, not empty, on the happy path.
	t.Run("log_store_error is omitted when the read succeeds", func(t *testing.T) {
		node := &fakeRaftControl{stats: raftStats(1, 1, 1, 0), firstLogIndex: 1}
		server := NewServer(node, "6000")

		rec := httptest.NewRecorder()
		server.handleStatus(rec, httptest.NewRequest(http.MethodGet, "/status", nil))

		if strings.Contains(rec.Body.String(), "log_store_error") {
			t.Errorf("body = %q, must not mention log_store_error", rec.Body.String())
		}
	})
}

// ciReadinessPattern is the regexp CI's "Wait for cluster readiness" step greps
// the /status body with (.github/workflows/ci.yml). Phase 3 moved this handler
// to encoding/json, which drops the space after the colon — the pattern already
// tolerated that, and this test is what keeps the two in step. Breaking it does
// not fail any Go test on its own; it hangs the e2e job for 120s and then
// reports an unready cluster, which is a much worse way to find out.
var ciReadinessPattern = regexp.MustCompile(`"leader_addr"[[:space:]]*:[[:space:]]*"[^"]+"`)

func TestStatusBodyMatchesCIReadinessProbe(t *testing.T) {
	tests := []struct {
		name       string
		leaderAddr string
		wantMatch  bool
	}{
		{name: "elected leader matches", leaderAddr: "node1:8088", wantMatch: true},
		{name: "ipv4 leader matches", leaderAddr: "10.0.0.1:8088", wantMatch: true},
		{
			// The probe must NOT match before an election: an empty
			// leader_addr is precisely the "not ready yet" state.
			name:       "no leader yet does not match",
			leaderAddr: "",
			wantMatch:  false,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			node := &fakeRaftControl{leaderAddr: tc.leaderAddr, stats: raftStats(3, 3, 3, 0)}
			server := NewServer(node, "6000")

			rec := httptest.NewRecorder()
			server.handleStatus(rec, httptest.NewRequest(http.MethodGet, "/status", nil))

			body := rec.Body.String()
			if got := ciReadinessPattern.MatchString(body); got != tc.wantMatch {
				t.Errorf("CI readiness pattern matched %v for body %q, want %v", got, body, tc.wantMatch)
			}
		})
	}
}

// TestStatusIsValidJSON complements the byte-exact pins above: those would also
// pass for a string that merely looks like JSON, which is how the hand-rolled
// %q version managed to emit a raw "<" for years.
func TestStatusIsValidJSON(t *testing.T) {
	node := &fakeRaftControl{
		isLeader:      true,
		leaderAddr:    `weird"host<8088`,
		stats:         raftStats(9000, 8999, 9000, 8500),
		firstLogIndex: 8001,
	}
	server := NewServer(node, "6000")

	rec := httptest.NewRecorder()
	server.handleStatus(rec, httptest.NewRequest(http.MethodGet, "/status", nil))

	var decoded map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &decoded); err != nil {
		t.Fatalf("decoding %q: %v", rec.Body.String(), err)
	}

	if got := decoded["leader_addr"]; got != `weird"host<8088` {
		t.Errorf("leader_addr = %v, want %q", got, `weird"host<8088`)
	}

	// json.Unmarshal into `any` yields float64; every index must survive the
	// round trip as a JSON number, not a string.
	wantIndices := map[string]float64{
		"first_log_index":     8001,
		"last_log_index":      9000,
		"applied_index":       8999,
		"commit_index":        9000,
		"last_snapshot_index": 8500,
	}
	for key, want := range wantIndices {
		got, ok := decoded[key].(float64)
		if !ok {
			t.Errorf("%s = %#v, want a JSON number", key, decoded[key])
			continue
		}
		if got != want {
			t.Errorf("%s = %v, want %v", key, got, want)
		}
	}
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
	node := &fakeRaftControl{
		isLeader:      true,
		leaderAddr:    "node1:8088",
		stats:         raftStats(9000, 9000, 9000, 8500),
		firstLogIndex: 8001,
	}
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
		{"/status", `{"is_leader":true,"leader_addr":"node1:8088",` +
			`"first_log_index":8001,"last_log_index":9000,"applied_index":9000,` +
			`"commit_index":9000,"last_snapshot_index":8500}`},
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
