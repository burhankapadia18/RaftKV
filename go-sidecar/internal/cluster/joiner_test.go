package cluster

import (
	"errors"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

// TestMain silences the retry chatter that Join writes to the standard logger.
func TestMain(m *testing.M) {
	log.SetOutput(io.Discard)
	code := m.Run()
	log.SetOutput(os.Stderr)
	os.Exit(code)
}

const (
	testNodeID   = "node2"
	testRaftAddr = "node2:8088"
)

// joinRecorder is a concurrency-safe record of what the fake leader's /join
// endpoint observed. The httptest server serves each request on its own
// goroutine, so every field is mutex-guarded to stay clean under -race.
type joinRecorder struct {
	mu        sync.Mutex
	hits      int
	paths     []string
	peerIDs   []string
	peerAddrs []string
}

// record stores one observed request and returns the 1-based attempt number.
func (r *joinRecorder) record(req *http.Request) int {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.hits++
	q := req.URL.Query()
	r.paths = append(r.paths, req.URL.Path)
	r.peerIDs = append(r.peerIDs, q.Get("peerID"))
	r.peerAddrs = append(r.peerAddrs, q.Get("peerAddress"))
	return r.hits
}

func (r *joinRecorder) count() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.hits
}

func (r *joinRecorder) snapshot() (paths, peerIDs, peerAddrs []string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]string(nil), r.paths...),
		append([]string(nil), r.peerIDs...),
		append([]string(nil), r.peerAddrs...)
}

// newFakeLeader starts an httptest server and returns its host:port, which is the
// shape JoinConfig.LeaderMgmtAddr expects (Join prefixes the scheme itself).
func newFakeLeader(t *testing.T, handler http.HandlerFunc) string {
	t.Helper()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return srv.Listener.Addr().String()
}

// testJoinConfig builds a JoinConfig with a sub-millisecond retry interval so the
// retry loop's real time.Sleep does not slow the suite down.
func testJoinConfig(leaderAddr string, maxRetries int) *JoinConfig {
	return &JoinConfig{
		LeaderMgmtAddr: leaderAddr,
		NodeID:         testNodeID,
		RaftAddr:       testRaftAddr,
		MaxRetries:     maxRetries,
		RetryInterval:  time.Millisecond,
	}
}

func TestDefaultJoinConfig(t *testing.T) {
	cfg := DefaultJoinConfig("leader:6000", "node3", "node3:8088")

	if cfg.LeaderMgmtAddr != "leader:6000" {
		t.Errorf("LeaderMgmtAddr = %q, want %q", cfg.LeaderMgmtAddr, "leader:6000")
	}
	if cfg.NodeID != "node3" {
		t.Errorf("NodeID = %q, want %q", cfg.NodeID, "node3")
	}
	if cfg.RaftAddr != "node3:8088" {
		t.Errorf("RaftAddr = %q, want %q", cfg.RaftAddr, "node3:8088")
	}
	// Pinned defaults: 20 attempts spaced 2s apart, i.e. a ~38s join window.
	if cfg.MaxRetries != 20 {
		t.Errorf("MaxRetries = %d, want 20", cfg.MaxRetries)
	}
	if cfg.RetryInterval != 2*time.Second {
		t.Errorf("RetryInterval = %v, want 2s", cfg.RetryInterval)
	}
}

func TestNewJoiner(t *testing.T) {
	cfg := DefaultJoinConfig("leader:6000", "node3", "node3:8088")

	j := mustJoiner(t, cfg)

	if j == nil {
		t.Fatal("NewJoiner returned nil")
	}
	if j.config != cfg {
		t.Error("NewJoiner did not retain the supplied config")
	}
	if j.client == nil {
		t.Fatal("NewJoiner did not build an http.Client")
	}
	if j.client.Timeout != 10*time.Second {
		t.Errorf("http client timeout = %v, want 10s", j.client.Timeout)
	}
}

func TestJoinSucceedsOnFirstAttempt(t *testing.T) {
	rec := &joinRecorder{}
	addr := newFakeLeader(t, func(w http.ResponseWriter, r *http.Request) {
		rec.record(r)
		w.WriteHeader(http.StatusOK)
	})

	if err := mustJoiner(t, testJoinConfig(addr, 5)).Join(); err != nil {
		t.Fatalf("Join() error = %v, want nil", err)
	}

	if got := rec.count(); got != 1 {
		t.Fatalf("leader was contacted %d times, want exactly 1", got)
	}

	paths, peerIDs, peerAddrs := rec.snapshot()
	if paths[0] != "/join" {
		t.Errorf("request path = %q, want %q", paths[0], "/join")
	}
	if peerIDs[0] != testNodeID {
		t.Errorf("peerID = %q, want %q", peerIDs[0], testNodeID)
	}
	if peerAddrs[0] != testRaftAddr {
		t.Errorf("peerAddress = %q, want %q", peerAddrs[0], testRaftAddr)
	}
}

func TestJoinRetriesUntilSuccess(t *testing.T) {
	tests := []struct {
		name       string
		failures   int
		failStatus int
		maxRetries int
	}{
		{name: "one failure", failures: 1, failStatus: http.StatusInternalServerError, maxRetries: 5},
		{name: "two failures", failures: 2, failStatus: http.StatusServiceUnavailable, maxRetries: 5},
		{name: "failures exactly up to the last allowed attempt", failures: 3, failStatus: http.StatusInternalServerError, maxRetries: 4},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			rec := &joinRecorder{}
			addr := newFakeLeader(t, func(w http.ResponseWriter, r *http.Request) {
				if attempt := rec.record(r); attempt <= tt.failures {
					w.WriteHeader(tt.failStatus)
					return
				}
				w.WriteHeader(http.StatusOK)
			})

			if err := mustJoiner(t, testJoinConfig(addr, tt.maxRetries)).Join(); err != nil {
				t.Fatalf("Join() error = %v, want nil", err)
			}

			wantHits := tt.failures + 1
			if got := rec.count(); got != wantHits {
				t.Fatalf("leader was contacted %d times, want %d", got, wantHits)
			}

			// Every attempt must carry the identical peer identity.
			_, peerIDs, peerAddrs := rec.snapshot()
			for i := range peerIDs {
				if peerIDs[i] != testNodeID || peerAddrs[i] != testRaftAddr {
					t.Errorf("attempt %d sent peerID=%q peerAddress=%q, want %q/%q",
						i+1, peerIDs[i], peerAddrs[i], testNodeID, testRaftAddr)
				}
			}
		})
	}
}

func TestJoinExhaustsRetries(t *testing.T) {
	const (
		maxRetries = 3
		body       = "leader unavailable"
	)

	rec := &joinRecorder{}
	addr := newFakeLeader(t, func(w http.ResponseWriter, r *http.Request) {
		rec.record(r)
		w.WriteHeader(http.StatusInternalServerError)
		_, _ = io.WriteString(w, body)
	})

	err := mustJoiner(t, testJoinConfig(addr, maxRetries)).Join()
	if err == nil {
		t.Fatal("Join() = nil, want an error after exhausting all attempts")
	}

	msg := err.Error()
	for _, want := range []string{
		"failed to join cluster after 3 attempts",
		"server returned status 500",
		body,
	} {
		if !strings.Contains(msg, want) {
			t.Errorf("Join() error = %q, want it to contain %q", msg, want)
		}
	}

	// The last attempt's error is wrapped with %w, so it stays reachable.
	unwrapped := errors.Unwrap(err)
	if unwrapped == nil {
		t.Fatal("Join() error does not wrap the last attempt's error")
	}
	if !strings.Contains(unwrapped.Error(), "server returned status 500") {
		t.Errorf("wrapped error = %q, want it to contain %q", unwrapped.Error(), "server returned status 500")
	}

	if got := rec.count(); got != maxRetries {
		t.Errorf("leader was contacted %d times, want %d", got, maxRetries)
	}
}

func TestJoinUnreachableLeader(t *testing.T) {
	// Start and immediately stop a server so we hold an address that nothing is
	// listening on; capture the address before Close, while the listener is alive.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	addr := srv.Listener.Addr().String()
	srv.Close()

	err := mustJoiner(t, testJoinConfig(addr, 2)).Join()
	if err == nil {
		t.Fatal("Join() = nil, want an error when the leader is unreachable")
	}

	msg := err.Error()
	if !strings.Contains(msg, "failed to join cluster after 2 attempts") {
		t.Errorf("Join() error = %q, want it to report the attempt count", msg)
	}
	if !strings.Contains(msg, "connection failed") {
		t.Errorf("Join() error = %q, want it to contain %q", msg, "connection failed")
	}
}

func TestJoinWithZeroMaxRetriesNeverContactsLeader(t *testing.T) {
	// PIN: MaxRetries <= 0 makes the loop body unreachable, so Join returns an
	// error that wraps a nil lastErr (rendering as a malformed %!w(<nil>) suffix)
	// without ever talking to the leader. There is no config validation today.
	rec := &joinRecorder{}
	addr := newFakeLeader(t, func(w http.ResponseWriter, r *http.Request) {
		rec.record(r)
		w.WriteHeader(http.StatusOK)
	})

	err := mustJoiner(t, testJoinConfig(addr, 0)).Join()
	if err == nil {
		t.Fatal("Join() = nil, want an error when MaxRetries is 0")
	}
	if !strings.Contains(err.Error(), "failed to join cluster after 0 attempts") {
		t.Errorf("Join() error = %q, want it to report 0 attempts", err.Error())
	}
	if unwrapped := errors.Unwrap(err); unwrapped != nil {
		t.Errorf("errors.Unwrap = %v, want nil (there was no attempt error to wrap)", unwrapped)
	}
	if got := rec.count(); got != 0 {
		t.Errorf("leader was contacted %d times, want 0", got)
	}
}

func TestJoinDoesNotEscapeQueryParameters(t *testing.T) {
	// PIN: the join URL is assembled with fmt.Sprintf, not url.Values.Encode, so a
	// node ID containing '&' silently truncates the value and injects an extra
	// parameter. Pinned here so a later hardening pass has to change it on purpose.
	rec := &joinRecorder{}
	addr := newFakeLeader(t, func(w http.ResponseWriter, r *http.Request) {
		rec.record(r)
		w.WriteHeader(http.StatusOK)
	})

	cfg := testJoinConfig(addr, 2)
	cfg.NodeID = "node2&injected=1"

	if err := mustJoiner(t, cfg).Join(); err != nil {
		t.Fatalf("Join() error = %v, want nil", err)
	}

	_, peerIDs, peerAddrs := rec.snapshot()
	if len(peerIDs) != 1 {
		t.Fatalf("leader was contacted %d times, want exactly 1", len(peerIDs))
	}
	if peerIDs[0] != "node2" {
		t.Errorf("peerID = %q, want %q (the '&' truncates the unescaped value)", peerIDs[0], "node2")
	}
	if peerAddrs[0] != testRaftAddr {
		t.Errorf("peerAddress = %q, want %q", peerAddrs[0], testRaftAddr)
	}
}

func TestJoinAsync(t *testing.T) {
	// Buffered so the handler never blocks, and so the send happens-before the test
	// goroutine's receive: no sleeps, no polling, race-detector clean.
	hits := make(chan url.Values, 4)
	addr := newFakeLeader(t, func(w http.ResponseWriter, r *http.Request) {
		hits <- r.URL.Query()
		w.WriteHeader(http.StatusOK)
	})

	mustJoiner(t, testJoinConfig(addr, 5)).JoinAsync()

	select {
	case q := <-hits:
		if got := q.Get("peerID"); got != testNodeID {
			t.Errorf("peerID = %q, want %q", got, testNodeID)
		}
		if got := q.Get("peerAddress"); got != testRaftAddr {
			t.Errorf("peerAddress = %q, want %q", got, testRaftAddr)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("JoinAsync did not contact the leader within 10s")
	}
}

func TestJoinAsyncDoesNotBlockTheCaller(t *testing.T) {
	// The leader handler parks until the test releases it. If JoinAsync were
	// synchronous the call below would never return and the test would time out —
	// a deterministic assertion of non-blocking behaviour with no wall-clock checks.
	started := make(chan struct{}, 1)
	release := make(chan struct{})
	finished := make(chan struct{}, 1)

	addr := newFakeLeader(t, func(w http.ResponseWriter, r *http.Request) {
		started <- struct{}{}
		<-release
		w.WriteHeader(http.StatusOK)
		finished <- struct{}{}
	})

	mustJoiner(t, testJoinConfig(addr, 5)).JoinAsync()

	select {
	case <-started:
	case <-time.After(10 * time.Second):
		close(release)
		t.Fatal("JoinAsync never issued the join request")
	}

	// JoinAsync returned while the request was still in flight, which is the point.
	close(release)

	select {
	case <-finished:
	case <-time.After(10 * time.Second):
		t.Fatal("the join request never completed after being released")
	}
}

// mustJoiner is NewJoiner for tests that are not exercising its error path.
//
// NewJoiner gained an error return when it learned to dial over HTTPS (R6.3):
// unusable TLS material is not something the retry loop can fix, and joining
// over plaintext instead would put the cluster-admin token on the wire in clear.
// These tests configure no TLS, so the error is always nil — asserting that here
// keeps every call site from repeating the check.
func mustJoiner(t *testing.T, cfg *JoinConfig) *Joiner {
	t.Helper()
	j, err := NewJoiner(cfg)
	if err != nil {
		t.Fatalf("NewJoiner: %v", err)
	}
	return j
}
