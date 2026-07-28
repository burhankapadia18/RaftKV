// Package management provides the HTTP management API for cluster operations.
package management

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"strconv"
	"time"
)

// RaftControl is the consumer-side view of the Raft node that the management
// API needs. Keeping it here (rather than depending on *raftnode.Node) lets the
// handlers be tested with a fake and keeps package management free of any
// dependency on package raftnode.
//
// *raftnode.Node satisfies this interface; the compile-time guarantee is the
// call site in cmd/sidecar/main.go. The `var _ RaftControl = (*raftnode.Node)(nil)`
// assertion deliberately lives in this package's test file rather than in
// package raftnode, since putting it there would make raftnode import
// management and invert the dependency.
type RaftControl interface {
	IsLeader() bool
	LeaderAddr() string
	AddVoter(id, address string) error

	// RemoveServer drops a peer from the cluster configuration. Like AddVoter
	// it is leader-only, which is what makes the forwarding below necessary.
	RemoveServer(id string) error

	// Stats returns Raft's own runtime counters as decimal strings (the shape
	// of raft.Raft.Stats()). Keeping the library's map shape here, instead of a
	// struct, is what avoids a shared type that raftnode would have to import
	// from this package.
	Stats() map[string]string

	// FirstLogIndex is where the Raft log currently begins; it advances only
	// when entries are truncated after a snapshot, which is why it is reported
	// separately from Stats().
	FirstLogIndex() (uint64, error)
}

// BackendProbe reports whether the local C++ state machine is answering (R5.5).
//
// Readiness has to include this: a sidecar whose raft is perfectly healthy but
// whose state machine is unreachable cannot apply anything, so routing traffic to
// it is pointless. Liveness deliberately does NOT include it — the process is up,
// and restarting it would not fix a dead neighbour.
type BackendProbe interface {
	// Probe returns nil when the backend answered.
	Probe(ctx context.Context) error
}

// Server represents the HTTP management server.
type Server struct {
	node       RaftControl
	httpServer *http.Server
	port       string

	// resolver and forwarder are what let /join and /remove work on any node
	// (R4.12). Both may be nil, which disables forwarding and restores the
	// older behavior of failing the leader-only call where it landed — that
	// keeps a half-configured server explicit rather than silently degraded.
	resolver  PeerResolver
	forwarder Forwarder

	// probe is optional; when nil the backend check is reported as skipped
	// rather than silently passing. A readiness endpoint that quietly drops a
	// check it could not run is worse than one that says so.
	probe BackendProbe

	// metricsHandler is mounted at /metrics when set (R5.3).
	metricsHandler http.Handler
}

// WithBackendProbe adds the state-machine reachability check to /ready.
func (s *Server) WithBackendProbe(probe BackendProbe) *Server {
	s.probe = probe
	return s
}

// WithMetricsHandler mounts a Prometheus handler at /metrics.
func (s *Server) WithMetricsHandler(handler http.Handler) *Server {
	s.metricsHandler = handler
	return s
}

// NewServer creates a new management server.
//
// resolver and forwarder are optional; pass nil for both to disable relaying.
func NewServer(node RaftControl, port string, resolver PeerResolver, forwarder Forwarder) *Server {
	return &Server{
		node:      node,
		port:      port,
		resolver:  resolver,
		forwarder: forwarder,
	}
}

// Start starts the HTTP management server in a goroutine.
func (s *Server) Start() {
	mux := http.NewServeMux()
	mux.HandleFunc("/join", s.handleJoin)
	mux.HandleFunc("/remove", s.handleRemove)
	mux.HandleFunc("/status", s.handleStatus)
	mux.HandleFunc("/health", s.handleHealth)
	mux.HandleFunc("/ready", s.handleReady)
	if s.metricsHandler != nil {
		mux.Handle("/metrics", s.metricsHandler)
	}

	addr := "0.0.0.0:" + s.port
	s.httpServer = &http.Server{
		Addr:         addr,
		Handler:      mux,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
	}

	logger.Info(fmt.Sprintf("Management API listening on %s", addr))
	go func() {
		if err := s.httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			logger.Error(fmt.Sprintf("Management server error: %v", err))
		}
	}()
}

// Stop gracefully shuts down the management server.
func (s *Server) Stop(ctx context.Context) error {
	if s.httpServer != nil {
		return s.httpServer.Shutdown(ctx)
	}
	return nil
}

// handleJoin handles requests from nodes wanting to join the cluster.
func (s *Server) handleJoin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	peerAddress := r.URL.Query().Get("peerAddress")
	peerID := r.URL.Query().Get("peerID")

	if peerAddress == "" || peerID == "" {
		http.Error(w, "Missing peerAddress or peerID", http.StatusBadRequest)
		return
	}

	logger.Info(fmt.Sprintf("Received join request for %s at %s", peerID, peerAddress))

	if s.relayToLeader(w, r, "/join") {
		return
	}

	if err := s.node.AddVoter(peerID, peerAddress); err != nil {
		logger.Error(fmt.Sprintf("Failed to add voter: %v", err))
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)
	w.Write([]byte("Joined successfully"))
}

// handleRemove drops a peer from the cluster configuration (R4.12).
func (s *Server) handleRemove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	peerID := r.URL.Query().Get("peerID")
	if peerID == "" {
		http.Error(w, "Missing peerID", http.StatusBadRequest)
		return
	}

	logger.Info(fmt.Sprintf("Received remove request for %s", peerID))

	if s.relayToLeader(w, r, "/remove") {
		return
	}

	if err := s.node.RemoveServer(peerID); err != nil {
		logger.Error(fmt.Sprintf("Failed to remove server: %v", err))
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)
	w.Write([]byte("Removed successfully"))
}

// relayToLeader forwards a leader-only request and reports whether it answered.
//
// Returns true when it has fully handled the response — either by relaying the
// leader's answer or by refusing — so the caller must return immediately.
// Returns false when this node is the leader and should do the work itself.
//
// Every refusal path answers 503 rather than 500: none of them mean the request
// was wrong, they mean "not here, not now", which is the same thing the write
// path tells a client when it cannot find a leader.
func (s *Server) relayToLeader(w http.ResponseWriter, r *http.Request, path string) bool {
	if s.node.IsLeader() {
		return false
	}

	// Already relayed once: answer truthfully instead of bouncing it onward.
	// Leadership moved between the first hop and this one.
	if r.Header.Get(ForwardedHeader) != "" {
		logger.Warn(fmt.Sprintf("Refusing already-forwarded %s: this node is not the leader", path))
		http.Error(w,
			"not the leader, and this request was already forwarded once",
			http.StatusServiceUnavailable)
		return true
	}

	if s.resolver == nil || s.forwarder == nil {
		http.Error(w, "not the leader and forwarding is not configured",
			http.StatusServiceUnavailable)
		return true
	}

	mgmtAddr, err := s.resolver.MgmtAddr(s.node.LeaderAddr())
	if err != nil {
		logger.Info(fmt.Sprintf("Cannot forward %s: %v", path, err))
		http.Error(w, "not the leader: "+err.Error(),
			http.StatusServiceUnavailable)
		return true
	}

	logger.Info(fmt.Sprintf("Forwarding %s to the leader at %s", path, mgmtAddr))
	result, err := s.forwarder.Forward(r.Context(), mgmtAddr, path, r.URL.RawQuery)
	if err != nil {
		logger.Error(fmt.Sprintf("Forwarding %s to %s failed: %v", path, mgmtAddr, err))
		http.Error(w, "forwarding to the leader failed: "+err.Error(),
			http.StatusServiceUnavailable)
		return true
	}

	// Relay the leader's answer verbatim: the caller asked for a cluster
	// change, and what the leader said about it is the real answer.
	w.WriteHeader(result.Status)
	w.Write(result.Body)
	return true
}

// statusResponse is the /status payload.
//
// The index fields exist so that log compaction is observable from outside the
// process (R3.6): first_log_index rising is the only proof the log was actually
// truncated, and last_snapshot_index tells you which snapshot caused it. Field
// order here is the order they appear in the JSON.
type statusResponse struct {
	IsLeader   bool   `json:"is_leader"`
	LeaderAddr string `json:"leader_addr"`

	FirstLogIndex     uint64 `json:"first_log_index"`
	LastLogIndex      uint64 `json:"last_log_index"`
	AppliedIndex      uint64 `json:"applied_index"`
	CommitIndex       uint64 `json:"commit_index"`
	LastSnapshotIndex uint64 `json:"last_snapshot_index"`

	// LogStoreError reports a failed FirstLogIndex read. /status must keep
	// answering when the log store misbehaves — it is what the readiness polls
	// use — so the failure is surfaced as a field instead of a 500, and
	// first_log_index reads 0. Absent from the JSON when everything is fine.
	LogStoreError string `json:"log_store_error,omitempty"`
}

// handleStatus returns the current status of the Raft node.
func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	stats := s.node.Stats()

	status := statusResponse{
		IsLeader:          s.node.IsLeader(),
		LeaderAddr:        s.node.LeaderAddr(),
		LastLogIndex:      statIndex(stats, "last_log_index"),
		AppliedIndex:      statIndex(stats, "applied_index"),
		CommitIndex:       statIndex(stats, "commit_index"),
		LastSnapshotIndex: statIndex(stats, "last_snapshot_index"),
	}

	firstLogIndex, err := s.node.FirstLogIndex()
	if err != nil {
		logger.Error(fmt.Sprintf("Failed to read first log index for /status: %v", err))
		status.LogStoreError = err.Error()
	} else {
		status.FirstLogIndex = firstLogIndex
	}

	// Marshal before writing anything: an encoder streaming straight into w
	// would have already committed a 200 and half a body by the time it failed.
	payload, err := json.Marshal(status)
	if err != nil {
		logger.Error(fmt.Sprintf("Failed to encode status: %v", err))
		http.Error(w, "Failed to encode status", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.Write(payload)
}

// statIndex reads one decimal counter out of a raft.Stats() map.
//
// A missing or unparseable key yields 0. Raft always reports these four, so a
// miss means the library changed its stat names under us — worth noticing, but
// not worth failing a status poll that is otherwise fully answerable, and not
// worth logging on every poll either. The distinguishing signal is a counter
// that stays at 0 while the cluster is plainly making progress.
func statIndex(stats map[string]string, key string) uint64 {
	value, err := strconv.ParseUint(stats[key], 10, 64)
	if err != nil {
		return 0
	}
	return value
}

// handleHealth is pure LIVENESS: the process is running and serving (R5.5).
//
// Deliberately says nothing about raft or the backend. An orchestrator uses
// liveness to decide whether to RESTART a container, and restarting this node
// because it cannot see a leader would be actively harmful — it would destroy the
// one member that might still be needed for quorum. Readiness is the endpoint
// that answers "should traffic go here", and that is /ready.
func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	w.Write([]byte("OK"))
}

// readyCheck is one named condition and its outcome.
type readyCheck struct {
	Name   string `json:"name"`
	OK     bool   `json:"ok"`
	Detail string `json:"detail,omitempty"`
}

// readyResponse is the /ready payload: the verdict plus every check behind it.
//
// Listing the checks matters more than the status code. "503" tells an operator
// to look; "raft_state: Candidate" tells them what at.
type readyResponse struct {
	Ready  bool         `json:"ready"`
	Checks []readyCheck `json:"checks"`
}

// backendProbeTimeout bounds the state-machine probe. Short on purpose: /ready is
// polled by an orchestrator's healthcheck, so a slow probe must fail rather than
// hold the request open and make the healthcheck itself time out.
const backendProbeTimeout = 2 * time.Second

// logger is this package's structured logger (R5.1). Package-level and settable
// rather than threaded through every constructor: the alternative was changing
// the signature of every New* in the codebase for a cross-cutting concern, and
// these are libraries with one instance per process.
//
// Defaults to DISCARDING rather than to os.Stdout. A package used without
// SetLogger — which is every unit test — should be silent, not spray JSON through
// the test output. main.go is the only caller of SetLogger.
var logger = slog.New(slog.NewTextHandler(io.Discard, nil))

// SetLogger installs the process logger for this package.
func SetLogger(l *slog.Logger) {
	if l != nil {
		logger = l
	}
}

// handleReady reports whether this node should receive traffic (R5.5).
//
// 200 only when raft is in a serving state, a leader is known, and the local C++
// state machine answers. Any one of those failing means requests sent here cannot
// be served correctly — a Candidate cannot commit, an unknown leader means writes
// have nowhere to go, and a dead backend cannot apply.
func (s *Server) handleReady(w http.ResponseWriter, r *http.Request) {
	checks := make([]readyCheck, 0, 3)

	state := s.node.Stats()["state"]
	// Leader and Follower can both serve: a follower forwards writes and
	// barriers reads (Phase 4). Candidate and Shutdown cannot.
	serving := state == "Leader" || state == "Follower"
	checks = append(checks, readyCheck{
		Name:   "raft_state",
		OK:     serving,
		Detail: state,
	})

	leader := s.node.LeaderAddr()
	checks = append(checks, readyCheck{
		Name:   "leader_known",
		OK:     leader != "",
		Detail: leader,
	})

	if s.probe == nil {
		// Reported as a failed check rather than skipped-and-passing: a readiness
		// endpoint that silently drops a check is how a broken node looks healthy.
		checks = append(checks, readyCheck{
			Name:   "backend_reachable",
			OK:     false,
			Detail: "no backend probe configured",
		})
	} else {
		ctx, cancel := context.WithTimeout(r.Context(), backendProbeTimeout)
		defer cancel()
		err := s.probe.Probe(ctx)
		check := readyCheck{Name: "backend_reachable", OK: err == nil}
		if err != nil {
			check.Detail = err.Error()
		}
		checks = append(checks, check)
	}

	ready := true
	for _, check := range checks {
		if !check.OK {
			ready = false
		}
	}

	payload, err := json.Marshal(readyResponse{Ready: ready, Checks: checks})
	if err != nil {
		logger.Error(fmt.Sprintf("Failed to encode readiness: %v", err))
		http.Error(w, "Failed to encode readiness", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	if !ready {
		w.WriteHeader(http.StatusServiceUnavailable)
	}
	w.Write(payload)
}
