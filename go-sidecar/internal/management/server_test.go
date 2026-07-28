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

// testAuthToken is the cluster-admin token used by tests that exercise the
// MUTATING endpoints. Those endpoints are closed by default since R6.1, so a test
// that wants to reach the handler behind the auth check has to present one — the
// auth behavior itself is covered separately by TestJoinRequiresAToken.
const testAuthToken = "test-cluster-admin-token"

// authedRequest builds a request carrying the cluster-admin token.
func authedRequest(method, target string) *http.Request {
	req := httptest.NewRequest(method, target, nil)
	req.Header.Set("Authorization", "Bearer "+testAuthToken)
	return req
}

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

	removeServerErr error

	mu              sync.Mutex
	addVoterCalls   []addVoterCall
	removeServerIDs []string
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

func (f *fakeRaftControl) RemoveServer(id string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.removeServerIDs = append(f.removeServerIDs, id)
	return f.removeServerErr
}

// removeServerIDsSnapshot returns a copy, safe to read from any goroutine.
func (f *fakeRaftControl) removeServerIDsSnapshot() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.removeServerIDs...)
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
			// isLeader: only the leader does the work itself. Since R4.12 a
			// follower forwards instead, which TestJoinForwarding covers.
			node := &fakeRaftControl{isLeader: true, addVoterErr: tc.addVoterErr}
			server := NewServer(node, "6000", nil, nil).
				WithAuthToken(testAuthToken)

			rec := httptest.NewRecorder()
			server.handleJoin(rec, authedRequest(tc.method, tc.target))

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
			server := NewServer(tc.node, "6000", nil, nil)

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
		server := NewServer(node, "6000", nil, nil)

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
		server := NewServer(node, "6000", nil, nil)

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
			server := NewServer(node, "6000", nil, nil)

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
	server := NewServer(node, "6000", nil, nil)

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
			server := NewServer(tc.node, "6000", nil, nil)

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
	server := NewServer(&fakeRaftControl{}, "6000", nil, nil)

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
	server := NewServer(node, port, nil, nil).WithAuthToken(testAuthToken)

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
		path      string
		wantBody  string
		needsAuth bool
	}{
		{"/health", "OK", false},
		{"/status", `{"is_leader":true,"leader_addr":"node1:8088",` +
			`"first_log_index":8001,"last_log_index":9000,"applied_index":9000,` +
			`"commit_index":9000,"last_snapshot_index":8500}`, false},
		{"/join?peerID=node2&peerAddress=node2:8088", "Joined successfully", true},
	} {
		req, err := http.NewRequest(http.MethodGet, baseURL+route.path, nil)
		if err != nil {
			t.Fatalf("building GET %s: %v", route.path, err)
		}
		// Only the mutating route carries a credential. The read-only ones are
		// deliberately requested WITHOUT one, so this loop also proves probes and
		// scrapers are not gated (R6.1).
		if route.needsAuth {
			req.Header.Set("Authorization", "Bearer "+testAuthToken)
		}
		resp, err := client.Do(req)
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

// --- R4.12: join/remove forwarding ----------------------------------------

// fakeResolver maps a raft address to a management address, or refuses.
type fakeResolver struct {
	addr string
	err  error
}

func (f *fakeResolver) MgmtAddr(raftAddr string) (string, error) {
	if f.err != nil {
		return "", f.err
	}
	return f.addr, nil
}

// recordingForwarder captures the relay so a test can assert what was sent, and
// returns a canned answer.
type recordingForwarder struct {
	result *ForwardResult
	err    error

	mu       sync.Mutex
	gotAddr  string
	gotPath  string
	gotQuery string
	calls    int
}

func (f *recordingForwarder) Forward(ctx context.Context, mgmtAddr, path, rawQuery string) (*ForwardResult, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	f.gotAddr, f.gotPath, f.gotQuery = mgmtAddr, path, rawQuery
	return f.result, f.err
}

func (f *recordingForwarder) callCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

// TestJoinForwarding is the R4.12 contract: a leader-only cluster change sent to
// a follower is relayed rather than failed where it landed.
func TestJoinForwarding(t *testing.T) {
	t.Run("a follower relays to the leader and returns its answer verbatim", func(t *testing.T) {
		node := &fakeRaftControl{isLeader: false, leaderAddr: "node1:8088"}
		resolver := &fakeResolver{addr: "node1:6000"}
		fwd := &recordingForwarder{
			result: &ForwardResult{Status: http.StatusOK, Body: []byte("Joined successfully")},
		}
		server := NewServer(node, "6000", resolver, fwd).
			WithAuthToken(testAuthToken)

		rec := httptest.NewRecorder()
		server.handleJoin(rec, authedRequest(http.MethodPost,
			"/join?peerID=node2&peerAddress=10.0.0.2:8088"))

		if rec.Code != http.StatusOK {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusOK)
		}
		if got := rec.Body.String(); got != "Joined successfully" {
			t.Errorf("body = %q, want the leader's answer relayed verbatim", got)
		}
		if fwd.gotAddr != "node1:6000" {
			t.Errorf("forwarded to %q, want the resolved management address", fwd.gotAddr)
		}
		if fwd.gotPath != "/join" {
			t.Errorf("forwarded path = %q, want /join", fwd.gotPath)
		}
		// The query must survive the relay: it carries the whole request.
		if !strings.Contains(fwd.gotQuery, "peerID=node2") ||
			!strings.Contains(fwd.gotQuery, "peerAddress=10.0.0.2") {
			t.Errorf("forwarded query = %q, want it to carry peerID and peerAddress",
				fwd.gotQuery)
		}
		// The follower must not have tried the leader-only call itself.
		if calls := node.addVoterCallsSnapshot(); len(calls) != 0 {
			t.Errorf("follower called AddVoter itself: %+v", calls)
		}
	})

	t.Run("the leader does the work and does not forward", func(t *testing.T) {
		node := &fakeRaftControl{isLeader: true}
		fwd := &recordingForwarder{}
		server := NewServer(node, "6000", &fakeResolver{addr: "node1:6000"}, fwd).
			WithAuthToken(testAuthToken)

		rec := httptest.NewRecorder()
		server.handleJoin(rec, authedRequest(http.MethodPost,
			"/join?peerID=node2&peerAddress=10.0.0.2:8088"))

		if rec.Code != http.StatusOK {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusOK)
		}
		if fwd.callCount() != 0 {
			t.Errorf("the leader forwarded %d times, want 0", fwd.callCount())
		}
		want := []addVoterCall{{id: "node2", address: "10.0.0.2:8088"}}
		if got := node.addVoterCallsSnapshot(); !reflect.DeepEqual(got, want) {
			t.Errorf("AddVoter calls = %+v, want %+v", got, want)
		}
	})

	// THE LOOP GUARD. Without it, two nodes that each believe the other leads
	// relay the same join back and forth until something times out.
	t.Run("an already-forwarded request is refused, never relayed twice", func(t *testing.T) {
		node := &fakeRaftControl{isLeader: false, leaderAddr: "node1:8088"}
		fwd := &recordingForwarder{
			result: &ForwardResult{Status: http.StatusOK, Body: []byte("should never be used")},
		}
		server := NewServer(node, "6000", &fakeResolver{addr: "node1:6000"}, fwd).
			WithAuthToken(testAuthToken)

		req := authedRequest(http.MethodPost,
			"/join?peerID=node2&peerAddress=10.0.0.2:8088")
		req.Header.Set(ForwardedHeader, "1")

		rec := httptest.NewRecorder()
		server.handleJoin(rec, req)

		if rec.Code != http.StatusServiceUnavailable {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusServiceUnavailable)
		}
		if fwd.callCount() != 0 {
			t.Errorf("relayed an already-forwarded request %d times, want 0 — "+
				"this is the forwarding loop the header exists to prevent",
				fwd.callCount())
		}
		if calls := node.addVoterCallsSnapshot(); len(calls) != 0 {
			t.Errorf("non-leader called AddVoter anyway: %+v", calls)
		}
	})

	t.Run("no known leader is 503, not a relay to nowhere", func(t *testing.T) {
		node := &fakeRaftControl{isLeader: false, leaderAddr: ""}
		fwd := &recordingForwarder{}
		server := NewServer(node, "6000",
			&fakeResolver{err: errors.New("peers: no leader address known")}, fwd).
			WithAuthToken(testAuthToken)

		rec := httptest.NewRecorder()
		server.handleJoin(rec, authedRequest(http.MethodPost,
			"/join?peerID=node2&peerAddress=10.0.0.2:8088"))

		if rec.Code != http.StatusServiceUnavailable {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusServiceUnavailable)
		}
		if fwd.callCount() != 0 {
			t.Errorf("forwarded despite an unresolvable leader (%d calls)", fwd.callCount())
		}
	})

	t.Run("a failed relay is 503, not a masked success", func(t *testing.T) {
		node := &fakeRaftControl{isLeader: false, leaderAddr: "node1:8088"}
		fwd := &recordingForwarder{err: errors.New("connection refused")}
		server := NewServer(node, "6000", &fakeResolver{addr: "node1:6000"}, fwd).
			WithAuthToken(testAuthToken)

		rec := httptest.NewRecorder()
		server.handleJoin(rec, authedRequest(http.MethodPost,
			"/join?peerID=node2&peerAddress=10.0.0.2:8088"))

		if rec.Code != http.StatusServiceUnavailable {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusServiceUnavailable)
		}
		if !strings.Contains(rec.Body.String(), "connection refused") {
			t.Errorf("body = %q, want it to name the underlying failure", rec.Body.String())
		}
	})
}

// TestHandleRemove covers the new /remove endpoint (R4.12).
func TestHandleRemove(t *testing.T) {
	tests := []struct {
		name       string
		method     string
		target     string
		isLeader   bool
		removeErr  error
		wantStatus int
		wantIDs    []string
	}{
		{
			name:       "leader removes the peer",
			method:     http.MethodPost,
			target:     "/remove?peerID=node3",
			isLeader:   true,
			wantStatus: http.StatusOK,
			wantIDs:    []string{"node3"},
		},
		{
			name:       "missing peerID is rejected",
			method:     http.MethodPost,
			target:     "/remove",
			isLeader:   true,
			wantStatus: http.StatusBadRequest,
			wantIDs:    nil,
		},
		{
			name:       "PUT is rejected",
			method:     http.MethodPut,
			target:     "/remove?peerID=node3",
			isLeader:   true,
			wantStatus: http.StatusMethodNotAllowed,
			wantIDs:    nil,
		},
		{
			name:       "RemoveServer failure is reported as 500",
			method:     http.MethodPost,
			target:     "/remove?peerID=node3",
			isLeader:   true,
			removeErr:  errors.New("not enough voters"),
			wantStatus: http.StatusInternalServerError,
			wantIDs:    []string{"node3"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			node := &fakeRaftControl{isLeader: tc.isLeader, removeServerErr: tc.removeErr}
			server := NewServer(node, "6000", nil, nil).
				WithAuthToken(testAuthToken)

			rec := httptest.NewRecorder()
			server.handleRemove(rec, authedRequest(tc.method, tc.target))

			if rec.Code != tc.wantStatus {
				t.Errorf("status = %d, want %d (body %q)", rec.Code, tc.wantStatus,
					rec.Body.String())
			}
			got := node.removeServerIDsSnapshot()
			if len(got) == 0 && len(tc.wantIDs) == 0 {
				return
			}
			if !reflect.DeepEqual(got, tc.wantIDs) {
				t.Errorf("RemoveServer ids = %v, want %v", got, tc.wantIDs)
			}
		})
	}

	t.Run("a follower relays /remove to the leader", func(t *testing.T) {
		node := &fakeRaftControl{isLeader: false, leaderAddr: "node1:8088"}
		fwd := &recordingForwarder{
			result: &ForwardResult{Status: http.StatusOK, Body: []byte("Removed successfully")},
		}
		server := NewServer(node, "6000", &fakeResolver{addr: "node1:6000"}, fwd).
			WithAuthToken(testAuthToken)

		rec := httptest.NewRecorder()
		server.handleRemove(rec, authedRequest(http.MethodPost, "/remove?peerID=node3"))

		if rec.Code != http.StatusOK {
			t.Errorf("status = %d, want %d", rec.Code, http.StatusOK)
		}
		if fwd.gotPath != "/remove" {
			t.Errorf("forwarded path = %q, want /remove", fwd.gotPath)
		}
		if ids := node.removeServerIDsSnapshot(); len(ids) != 0 {
			t.Errorf("follower called RemoveServer itself: %v", ids)
		}
	})
}

// --- R5.5: readiness, distinct from liveness ------------------------------

// fakeProbe stands in for the C++ state machine reachability check.
type fakeProbe struct {
	err    error
	blocks time.Duration

	mu    sync.Mutex
	calls int
}

func (f *fakeProbe) Probe(ctx context.Context) error {
	f.mu.Lock()
	f.calls++
	f.mu.Unlock()
	if f.blocks > 0 {
		select {
		case <-time.After(f.blocks):
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	return f.err
}

func (f *fakeProbe) callCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

// TestHandleReady covers every combination that decides whether traffic should be
// routed here. A readiness endpoint that reports ready while one of these is
// broken is worse than having none at all.
func TestHandleReady(t *testing.T) {
	tests := []struct {
		name       string
		state      string
		leaderAddr string
		probeErr   error
		noProbe    bool
		wantStatus int
		wantFailed string // the check expected to be false, "" when all pass
	}{
		{
			name:       "leader with a reachable backend is ready",
			state:      "Leader",
			leaderAddr: "node1:8088",
			wantStatus: http.StatusOK,
		},
		{
			// A follower serves too since Phase 4: it forwards writes and
			// barriers reads. Requiring leadership would leave two of three nodes
			// permanently out of rotation.
			name:       "follower with a known leader is ready",
			state:      "Follower",
			leaderAddr: "node1:8088",
			wantStatus: http.StatusOK,
		},
		{
			name:       "candidate is not ready",
			state:      "Candidate",
			leaderAddr: "",
			wantStatus: http.StatusServiceUnavailable,
			wantFailed: "raft_state",
		},
		{
			name:       "shutdown is not ready",
			state:      "Shutdown",
			leaderAddr: "",
			wantStatus: http.StatusServiceUnavailable,
			wantFailed: "raft_state",
		},
		{
			// The lost-quorum shape: still a Follower, but no leader exists, so a
			// write sent here has nowhere to go.
			name:       "follower with no known leader is not ready",
			state:      "Follower",
			leaderAddr: "",
			wantStatus: http.StatusServiceUnavailable,
			wantFailed: "leader_known",
		},
		{
			// Raft is fine but the state machine is dead: this node cannot apply
			// anything, so routing to it is pointless.
			name:       "unreachable backend is not ready",
			state:      "Leader",
			leaderAddr: "node1:8088",
			probeErr:   errors.New("connection refused"),
			wantStatus: http.StatusServiceUnavailable,
			wantFailed: "backend_reachable",
		},
		{
			// A check that could not be run must FAIL, not silently pass.
			name:       "a missing probe fails rather than passing quietly",
			state:      "Leader",
			leaderAddr: "node1:8088",
			noProbe:    true,
			wantStatus: http.StatusServiceUnavailable,
			wantFailed: "backend_reachable",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			node := &fakeRaftControl{
				leaderAddr: tc.leaderAddr,
				stats:      map[string]string{"state": tc.state},
			}
			server := NewServer(node, "6000", nil, nil)
			if !tc.noProbe {
				server = server.WithBackendProbe(&fakeProbe{err: tc.probeErr})
			}

			rec := httptest.NewRecorder()
			server.handleReady(rec, httptest.NewRequest(http.MethodGet, "/ready", nil))

			if rec.Code != tc.wantStatus {
				t.Errorf("status = %d, want %d (body %s)", rec.Code, tc.wantStatus,
					rec.Body.String())
			}
			if got := rec.Header().Get("Content-Type"); got != "application/json" {
				t.Errorf("Content-Type = %q, want application/json", got)
			}

			var payload readyResponse
			if err := json.Unmarshal(rec.Body.Bytes(), &payload); err != nil {
				t.Fatalf("body is not valid JSON: %v (%s)", err, rec.Body.String())
			}
			if payload.Ready != (tc.wantStatus == http.StatusOK) {
				t.Errorf("ready = %v, want %v", payload.Ready,
					tc.wantStatus == http.StatusOK)
			}

			// The body must NAME the failing check. "503" tells an operator to
			// look; the check name tells them where, which is the whole point of
			// listing them.
			if tc.wantFailed != "" {
				found := false
				for _, check := range payload.Checks {
					if check.Name == tc.wantFailed {
						found = true
						if check.OK {
							t.Errorf("check %q reported ok, want it to be the failure",
								tc.wantFailed)
						}
					}
				}
				if !found {
					t.Errorf("no check named %q in %+v", tc.wantFailed, payload.Checks)
				}
			}
		})
	}
}

// TestReadyIsNotLiveness pins the distinction. Conflating them is actively
// harmful: an orchestrator restarts on failed LIVENESS, and restarting a node
// because it cannot see a leader would destroy the member that might still be
// needed for quorum.
func TestReadyIsNotLiveness(t *testing.T) {
	node := &fakeRaftControl{
		leaderAddr: "",
		stats:      map[string]string{"state": "Candidate"},
	}
	server := NewServer(node, "6000", nil, nil).
		WithBackendProbe(&fakeProbe{err: errors.New("down")})

	live := httptest.NewRecorder()
	server.handleHealth(live, httptest.NewRequest(http.MethodGet, "/health", nil))
	if live.Code != http.StatusOK {
		t.Errorf("/health = %d, want 200: the process IS up, and a restart would "+
			"not fix a missing leader", live.Code)
	}

	ready := httptest.NewRecorder()
	server.handleReady(ready, httptest.NewRequest(http.MethodGet, "/ready", nil))
	if ready.Code != http.StatusServiceUnavailable {
		t.Errorf("/ready = %d, want 503: nothing can be served here", ready.Code)
	}
}

// TestReadyBoundsASlowProbe: /ready is polled by a container healthcheck, so a
// hanging backend must produce a fast 503 rather than holding the request open
// until the healthcheck itself times out.
func TestReadyBoundsASlowProbe(t *testing.T) {
	node := &fakeRaftControl{
		leaderAddr: "node1:8088",
		stats:      map[string]string{"state": "Leader"},
	}
	probe := &fakeProbe{blocks: 30 * time.Second}
	server := NewServer(node, "6000", nil, nil).WithBackendProbe(probe)

	start := time.Now()
	rec := httptest.NewRecorder()
	server.handleReady(rec, httptest.NewRequest(http.MethodGet, "/ready", nil))
	elapsed := time.Since(start)

	if rec.Code != http.StatusServiceUnavailable {
		t.Errorf("status = %d, want 503", rec.Code)
	}
	if elapsed > 10*time.Second {
		t.Errorf("took %v; the probe timeout (%v) is not being applied",
			elapsed, backendProbeTimeout)
	}
	if probe.callCount() != 1 {
		t.Errorf("probe called %d times, want 1", probe.callCount())
	}
}

// --- R6.1/R6.2: cluster-admin token on the mutating endpoints -------------

// TestJoinRequiresAToken is the Phase 6 headline: before this, anyone who could
// reach port 6000 could add a voter to the cluster.
func TestJoinRequiresAToken(t *testing.T) {
	const token = "s3cret-cluster-admin"

	tests := []struct {
		name       string
		configured string // token the server is configured with
		header     string // raw Authorization header sent, "" for none
		wantStatus int
		wantAdded  bool
	}{
		{
			name:       "no token configured disables the endpoint",
			configured: "",
			header:     "Bearer " + token,
			wantStatus: http.StatusForbidden,
			wantAdded:  false,
		},
		{
			// THE test. An unauthenticated caller must not be able to grow the
			// cluster, and the assertion that matters is wantAdded=false — a 403
			// that still called AddVoter would be worse than no check at all.
			name:       "missing header is rejected and the peer is NOT added",
			configured: token,
			header:     "",
			wantStatus: http.StatusUnauthorized,
			wantAdded:  false,
		},
		{
			name:       "wrong token is rejected and the peer is NOT added",
			configured: token,
			header:     "Bearer wrong-token",
			wantStatus: http.StatusForbidden,
			wantAdded:  false,
		},
		{
			name:       "a token of the right length but wrong bytes is rejected",
			configured: token,
			header:     "Bearer " + strings.Repeat("x", len(token)),
			wantStatus: http.StatusForbidden,
			wantAdded:  false,
		},
		{
			// A prefix must not pass: this is what a timing attack would try to
			// build up one byte at a time.
			name:       "a correct prefix is rejected",
			configured: token,
			header:     "Bearer " + token[:5],
			wantStatus: http.StatusForbidden,
			wantAdded:  false,
		},
		{
			name:       "the wrong auth scheme is rejected",
			configured: token,
			header:     "Basic " + token,
			wantStatus: http.StatusUnauthorized,
			wantAdded:  false,
		},
		{
			name:       "the correct token joins",
			configured: token,
			header:     "Bearer " + token,
			wantStatus: http.StatusOK,
			wantAdded:  true,
		},
		{
			// RFC 7235 says the scheme is case-insensitive, and a client sending
			// "bearer" is not the attacker this defends against.
			name:       "the scheme is case-insensitive",
			configured: token,
			header:     "bearer " + token,
			wantStatus: http.StatusOK,
			wantAdded:  true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			node := &fakeRaftControl{isLeader: true}
			server := NewServer(node, "6000", nil, nil).WithAuthToken(tc.configured)

			req := httptest.NewRequest(http.MethodPost,
				"/join?peerID=node2&peerAddress=10.0.0.2:8088", nil)
			if tc.header != "" {
				req.Header.Set("Authorization", tc.header)
			}

			rec := httptest.NewRecorder()
			server.handleJoin(rec, req)

			if rec.Code != tc.wantStatus {
				t.Errorf("status = %d, want %d (body %q)", rec.Code, tc.wantStatus,
					rec.Body.String())
			}
			added := len(node.addVoterCallsSnapshot()) > 0
			if added != tc.wantAdded {
				t.Errorf("AddVoter called = %v, want %v — an unauthorized caller "+
					"must not be able to change cluster membership", added, tc.wantAdded)
			}
		})
	}
}

// TestRemoveRequiresAToken: /remove can shrink the cluster below quorum, so it is
// at least as sensitive as /join.
func TestRemoveRequiresAToken(t *testing.T) {
	const token = "s3cret"
	node := &fakeRaftControl{isLeader: true}
	server := NewServer(node, "6000", nil, nil).WithAuthToken(token)

	rec := httptest.NewRecorder()
	server.handleRemove(rec,
		httptest.NewRequest(http.MethodPost, "/remove?peerID=node3", nil))

	if rec.Code != http.StatusUnauthorized {
		t.Errorf("status = %d, want 401", rec.Code)
	}
	if ids := node.removeServerIDsSnapshot(); len(ids) != 0 {
		t.Errorf("unauthenticated caller removed %v from the cluster", ids)
	}
}

// TestReadOnlyEndpointsStayUnauthenticated: probes and scrapers must keep working.
// Requiring a token on /ready would mean an orchestrator could never see a node as
// healthy, and on /metrics that Prometheus could never scrape it.
func TestReadOnlyEndpointsStayUnauthenticated(t *testing.T) {
	node := &fakeRaftControl{
		isLeader:   true,
		leaderAddr: "node1:8088",
		stats:      map[string]string{"state": "Leader"},
	}
	server := NewServer(node, "6000", nil, nil).
		WithAuthToken("s3cret").
		WithBackendProbe(&fakeProbe{})

	for _, tc := range []struct {
		name    string
		handler func(http.ResponseWriter, *http.Request)
		path    string
	}{
		{"health", server.handleHealth, "/health"},
		{"ready", server.handleReady, "/ready"},
		{"status", server.handleStatus, "/status"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			rec := httptest.NewRecorder()
			tc.handler(rec, httptest.NewRequest(http.MethodGet, tc.path, nil))
			if rec.Code == http.StatusUnauthorized || rec.Code == http.StatusForbidden {
				t.Errorf("%s returned %d without a token; probes and scrapers "+
					"cannot authenticate and must not be gated", tc.path, rec.Code)
			}
		})
	}
}

// TestUnauthorizedJoinIsNotForwarded: a rejected request must be refused where it
// lands, not relayed to the leader for the leader to reject again. Forwarding it
// would let an unauthenticated caller generate traffic to the leader at will.
func TestUnauthorizedJoinIsNotForwarded(t *testing.T) {
	node := &fakeRaftControl{isLeader: false, leaderAddr: "node1:8088"}
	fwd := &recordingForwarder{
		result: &ForwardResult{Status: http.StatusOK, Body: []byte("should not happen")},
	}
	server := NewServer(node, "6000", &fakeResolver{addr: "node1:6000"}, fwd).
		WithAuthToken("s3cret")

	rec := httptest.NewRecorder()
	server.handleJoin(rec,
		httptest.NewRequest(http.MethodPost, "/join?peerID=n&peerAddress=a:1", nil))

	if rec.Code != http.StatusUnauthorized {
		t.Errorf("status = %d, want 401", rec.Code)
	}
	if fwd.callCount() != 0 {
		t.Errorf("relayed an unauthenticated request %d times, want 0",
			fwd.callCount())
	}
}

func TestExtractBearer(t *testing.T) {
	for _, tc := range []struct {
		header string
		want   string
	}{
		{"Bearer abc", "abc"},
		{"bearer abc", "abc"},
		{"BEARER abc", "abc"},
		{"Bearer   abc  ", "abc"},
		{"Basic abc", ""},
		{"abc", ""},
		{"", ""},
		{"Bearer", ""},
	} {
		req := httptest.NewRequest(http.MethodGet, "/", nil)
		if tc.header != "" {
			req.Header.Set("Authorization", tc.header)
		}
		if got := extractBearer(req); got != tc.want {
			t.Errorf("extractBearer(%q) = %q, want %q", tc.header, got, tc.want)
		}
	}
}
