// Package management provides the HTTP management API for cluster operations.
package management

import (
	"context"
	"fmt"
	"log"
	"net/http"
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
}

// Server represents the HTTP management server.
type Server struct {
	node       RaftControl
	httpServer *http.Server
	port       string
}

// NewServer creates a new management server.
func NewServer(node RaftControl, port string) *Server {
	return &Server{
		node: node,
		port: port,
	}
}

// Start starts the HTTP management server in a goroutine.
func (s *Server) Start() {
	mux := http.NewServeMux()
	mux.HandleFunc("/join", s.handleJoin)
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

	if err := s.node.AddVoter(peerID, peerAddress); err != nil {
		log.Printf("Failed to add voter: %v", err)
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)
	w.Write([]byte("Joined successfully"))
}

// handleStatus returns the current status of the Raft node.
func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	status := struct {
		IsLeader   bool   `json:"is_leader"`
		LeaderAddr string `json:"leader_addr"`
	}{
		IsLeader:   s.node.IsLeader(),
		LeaderAddr: s.node.LeaderAddr(),
	}

	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"is_leader": %v, "leader_addr": %q}`, status.IsLeader, status.LeaderAddr)
}

// handleHealth returns a simple health check response.
func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	w.Write([]byte("OK"))
}
