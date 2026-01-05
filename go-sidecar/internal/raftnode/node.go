// Package raftnode provides Raft node setup and management.
package raftnode

import (
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"time"

	"github.com/hashicorp/raft"
	raftboltdb "github.com/hashicorp/raft-boltdb"

	"my-raft-sidecar/internal/config"
)

// Node wraps the Raft instance and provides high-level operations.
type Node struct {
	Raft      *raft.Raft
	Transport *raft.NetworkTransport
	config    *config.Config
}

// Options contains optional parameters for creating a Raft node.
type Options struct {
	// MaxPool is the maximum number of connections in the transport pool.
	MaxPool int
	// Timeout is the timeout for transport operations.
	Timeout time.Duration
}

// DefaultOptions returns sensible default options.
func DefaultOptions() *Options {
	return &Options{
		MaxPool: 3,
		Timeout: 10 * time.Second,
	}
}

// New creates and configures a new Raft node.
func New(cfg *config.Config, fsm raft.FSM, opts *Options) (*Node, error) {
	if opts == nil {
		opts = DefaultOptions()
	}

	// Create data directory
	if err := os.MkdirAll(cfg.DataDir, 0700); err != nil {
		return nil, fmt.Errorf("failed to create data directory: %w", err)
	}

	// Configure Raft
	raftConfig := raft.DefaultConfig()
	raftConfig.LocalID = raft.ServerID(cfg.NodeID)

	// Setup log store
	logStore, err := raftboltdb.NewBoltStore(filepath.Join(cfg.DataDir, "logs.dat"))
	if err != nil {
		return nil, fmt.Errorf("failed to create log store: %w", err)
	}

	// Create transport
	transport, err := createTransport(cfg, opts)
	if err != nil {
		return nil, fmt.Errorf("failed to create transport: %w", err)
	}

	// Create Raft instance
	r, err := raft.NewRaft(
		raftConfig,
		fsm,
		logStore,
		logStore, // Use same store for stable store
		raft.NewDiscardSnapshotStore(),
		transport,
	)
	if err != nil {
		return nil, fmt.Errorf("failed to create raft instance: %w", err)
	}

	return &Node{
		Raft:      r,
		Transport: transport,
		config:    cfg,
	}, nil
}

// createTransport creates and configures the Raft network transport.
func createTransport(cfg *config.Config, opts *Options) (*raft.NetworkTransport, error) {
	bindAddr := cfg.BindAddr()
	advertiseAddr := cfg.AdvertiseAddr()

	// Resolve the advertise address to a TCP address
	advAddr, err := net.ResolveTCPAddr("tcp", advertiseAddr)
	if err != nil {
		return nil, fmt.Errorf("failed to resolve advertise address %s: %w", advertiseAddr, err)
	}

	transport, err := raft.NewTCPTransport(
		bindAddr,
		advAddr,
		opts.MaxPool,
		opts.Timeout,
		os.Stderr,
	)
	if err != nil {
		return nil, fmt.Errorf("failed to create TCP transport: %w", err)
	}

	return transport, nil
}

// Bootstrap bootstraps the Raft cluster with this node as the initial leader.
func (n *Node) Bootstrap() error {
	log.Println("Bootstrapping cluster...")
	future := n.Raft.BootstrapCluster(raft.Configuration{
		Servers: []raft.Server{
			{
				ID:      raft.ServerID(n.config.NodeID),
				Address: n.Transport.LocalAddr(),
			},
		},
	})
	return future.Error()
}

// AddVoter adds a new voting member to the cluster.
func (n *Node) AddVoter(id, address string) error {
	future := n.Raft.AddVoter(
		raft.ServerID(id),
		raft.ServerAddress(address),
		0,
		0,
	)
	return future.Error()
}

// Apply proposes a command to the Raft cluster.
func (n *Node) Apply(data []byte, timeout time.Duration) error {
	future := n.Raft.Apply(data, timeout)
	return future.Error()
}

// IsLeader returns true if this node is currently the leader.
func (n *Node) IsLeader() bool {
	return n.Raft.State() == raft.Leader
}

// LeaderAddr returns the address of the current leader.
func (n *Node) LeaderAddr() string {
	addr, _ := n.Raft.LeaderWithID()
	return string(addr)
}
