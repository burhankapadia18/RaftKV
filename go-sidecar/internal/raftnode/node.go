// Package raftnode provides Raft node setup and management.
package raftnode

import (
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"time"

	"github.com/hashicorp/raft"
	raftboltdb "github.com/hashicorp/raft-boltdb/v2"

	"my-raft-sidecar/internal/config"
)

// snapshotsRetained is how many snapshots raft.FileSnapshotStore keeps on
// disk. Two is the conventional choice: the newest one plus a fallback if it
// turns out to be unreadable. The store rejects anything below one.
const snapshotsRetained = 2

// Node wraps the Raft instance and provides high-level operations.

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

type Node struct {
	Raft      *raft.Raft
	Transport *raft.NetworkTransport
	config    *config.Config

	// logStore is retained (not just handed to raft) because it is the only
	// component that knows where the log now *begins* — see FirstLogIndex.
	logStore *raftboltdb.BoltStore
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

// newRaftConfig builds the raft.Config for this node.
//
// Split out of New so the tunables that actually bound the log can be asserted
// without standing up a Raft instance: SnapshotInterval/SnapshotThreshold
// decide when a snapshot is taken, and TrailingLogs decides how much of the log
// survives it. Getting any of the three wrong is invisible until the log has
// grown for days, so it is worth a unit test.
func newRaftConfig(cfg *config.Config) *raft.Config {
	raftConfig := raft.DefaultConfig()
	raftConfig.LocalID = raft.ServerID(cfg.NodeID)
	raftConfig.SnapshotInterval = cfg.SnapshotInterval
	raftConfig.SnapshotThreshold = cfg.SnapshotThreshold
	raftConfig.TrailingLogs = cfg.TrailingLogs
	return raftConfig
}

// newSnapshotStore creates the on-disk snapshot store under dir.
//
// Phase 3 replaces raft.NewDiscardSnapshotStore(), which accepted a snapshot
// and threw it away — so the log was never truncated and every restart
// replayed all history. Unlike the discard store, FileSnapshotStore can fail
// (it creates dir/snapshots and write-tests it), so the error is returned
// rather than ignored.
func newSnapshotStore(dir string) (raft.SnapshotStore, error) {
	store, err := raft.NewFileSnapshotStore(dir, snapshotsRetained, os.Stderr)
	if err != nil {
		return nil, fmt.Errorf("failed to create snapshot store in %s: %w", dir, err)
	}
	return store, nil
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
	raftConfig := newRaftConfig(cfg)

	// Setup log store
	logStore, err := raftboltdb.NewBoltStore(filepath.Join(cfg.DataDir, "logs.dat"))
	if err != nil {
		return nil, fmt.Errorf("failed to create log store: %w", err)
	}

	// Setup snapshot store. Every failure below has to release the BoltDB
	// handle: it holds an exclusive flock on logs.dat, so leaking it turns a
	// recoverable startup error into a file that the next attempt cannot open.
	snapshots, err := newSnapshotStore(cfg.DataDir)
	if err != nil {
		closeLogStore(logStore)
		return nil, err
	}

	// Create transport
	transport, err := createTransport(cfg, opts)
	if err != nil {
		closeLogStore(logStore)
		return nil, fmt.Errorf("failed to create transport: %w", err)
	}

	// Create Raft instance
	r, err := raft.NewRaft(
		raftConfig,
		fsm,
		logStore,
		logStore, // Use same store for stable store
		snapshots,
		transport,
	)
	if err != nil {
		if closeErr := transport.Close(); closeErr != nil {
			logger.Error(fmt.Sprintf("Failed to close transport after Raft setup error: %v", closeErr))
		}
		closeLogStore(logStore)
		return nil, fmt.Errorf("failed to create raft instance: %w", err)
	}

	return &Node{
		Raft:      r,
		Transport: transport,
		config:    cfg,
		logStore:  logStore,
	}, nil
}

// closeLogStore releases the BoltDB handle on a startup failure path. The close
// error cannot be returned (it would mask the failure that caused the unwind)
// and must not be dropped silently, so it is logged.
func closeLogStore(logStore *raftboltdb.BoltStore) {
	if err := logStore.Close(); err != nil {
		logger.Error(fmt.Sprintf("Failed to close log store after Raft setup error: %v", err))
	}
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
	logger.Info("Bootstrapping cluster...")
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

// LeadershipTransfer asks Raft to hand leadership to another voter (R5.8).
//
// Called on shutdown so a planned stop does not cost an election. Without it the
// cluster notices the leader is gone only when heartbeats time out, so every
// client write fails for the election window — for a deploy or a restart, that is
// downtime nobody needed to take.
//
// Fails on a follower and when there is no other voter to hand off to; both are
// expected, so callers log and carry on rather than treating it as fatal.
func (n *Node) LeadershipTransfer() error {
	return n.Raft.LeadershipTransfer().Error()
}

// Shutdown stops Raft and waits for it to finish.
func (n *Node) Shutdown() error {
	return n.Raft.Shutdown().Error()
}

// Barrier blocks until every entry committed before the call has been applied to
// this node's FSM.
//
// It works by proposing a no-op entry and waiting for it to apply, so it only
// succeeds on the leader — which is exactly the property a linearizable read
// needs (R4.5).
func (n *Node) Barrier(timeout time.Duration) error {
	return n.Raft.Barrier(timeout).Error()
}

// VerifyLeader confirms with a quorum that this node is still the leader.
//
// Distinct from IsLeader(), which only reports this node's own belief. A
// partitioned old leader still thinks it leads; VerifyLeader is what catches
// that, which is why a linearizable read cannot rely on IsLeader alone.
func (n *Node) VerifyLeader() error {
	return n.Raft.VerifyLeader().Error()
}

// RemoveServer drops a peer from the cluster configuration.
//
// Leader-only, like AddVoter: on a follower the future fails with
// raft.ErrNotLeader, which is why management forwards the request rather than
// calling this on whichever node happened to receive it.
func (n *Node) RemoveServer(id string) error {
	future := n.Raft.RemoveServer(raft.ServerID(id), 0, 0)
	return future.Error()
}

// Apply proposes a command to the Raft cluster and waits for it to be applied
// locally.
//
// Both halves of the outcome are surfaced, because they fail independently:
//   - the returned error is the Raft-level failure (not leader, enqueue
//     timeout, leadership lost) — the entry was never committed;
//   - the returned value is whatever the FSM's Apply returned for this entry
//     (nil on success, an *fsm.ApplyError when the local state machine
//     rejected a committed entry). Collapsing that to a bare nil error is what
//     made state-machine failures invisible to callers.
//
// Response() is only read once Error() has returned nil: Raft populates the
// future's response before unblocking it, and the value is meaningless when
// the entry never committed.
func (n *Node) Apply(data []byte, timeout time.Duration) (interface{}, error) {
	future := n.Raft.Apply(data, timeout)
	if err := future.Error(); err != nil {
		return nil, err
	}
	return future.Response(), nil
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

// Stats returns Raft's runtime counters as decimal strings, keyed by the names
// hashicorp/raft uses ("last_log_index", "last_snapshot_index",
// "applied_index", "commit_index", ...).
//
// The map shape is Raft's, not ours, on purpose: exposing it as-is keeps this
// package free of any type that the management API would also have to know
// about, which is what lets management.RaftControl stay a consumer-side
// interface with no import back into raftnode.
func (n *Node) Stats() map[string]string {
	return n.Raft.Stats()
}

// FirstLogIndex returns the index of the oldest entry still in the Raft log.
//
// It is the only direct evidence that compaction actually happened: Stats()
// reports where the log ends and where the last snapshot was taken, but only
// the log store knows where the log now begins. After a snapshot at index N
// Raft truncates everything below N-TrailingLogs, and this value is what moves.
// Zero means the log is empty.
func (n *Node) FirstLogIndex() (uint64, error) {
	index, err := n.logStore.FirstIndex()
	if err != nil {
		return 0, fmt.Errorf("failed to read first log index: %w", err)
	}
	return index, nil
}
