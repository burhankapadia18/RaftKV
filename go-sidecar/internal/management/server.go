// Package management provides the HTTP management API for cluster operations.
package management

import (
	"context"
	"encoding/json"
	"log"
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

	addr := "0.0.0.0:" + s.port
	s.httpServer = &http.Server{
		Addr:         addr,
		Handler:      mux,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
	}

	log.Printf("Management API listening on %s", addr)
	go func() {
		if err := s.httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Printf("Management server error: %v", err)
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

	log.Printf("Received join request for %s at %s", peerID, peerAddress)

	if s.relayToLeader(w, r, "/join") {
		return
	}

	if err := s.node.AddVoter(peerID, peerAddress); err != nil {
		log.Printf("Failed to add voter: %v", err)
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

	log.Printf("Received remove request for %s", peerID)

	if s.relayToLeader(w, r, "/remove") {
		return
	}

	if err := s.node.RemoveServer(peerID); err != nil {
		log.Printf("Failed to remove server: %v", err)
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
		log.Printf("Refusing already-forwarded %s: this node is not the leader", path)
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
		log.Printf("Cannot forward %s: %v", path, err)
		http.Error(w, "not the leader: "+err.Error(),
			http.StatusServiceUnavailable)
		return true
	}

	log.Printf("Forwarding %s to the leader at %s", path, mgmtAddr)
	result, err := s.forwarder.Forward(r.Context(), mgmtAddr, path, r.URL.RawQuery)
	if err != nil {
		log.Printf("Forwarding %s to %s failed: %v", path, mgmtAddr, err)
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
		log.Printf("Failed to read first log index for /status: %v", err)
		status.LogStoreError = err.Error()
	} else {
		status.FirstLogIndex = firstLogIndex
	}

	// Marshal before writing anything: an encoder streaming straight into w
	// would have already committed a 200 and half a body by the time it failed.
	payload, err := json.Marshal(status)
	if err != nil {
		log.Printf("Failed to encode status: %v", err)
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

// handleHealth returns a simple health check response.
func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	w.Write([]byte("OK"))
}
