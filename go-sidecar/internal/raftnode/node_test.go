package raftnode

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/hashicorp/raft"

	"my-raft-sidecar/internal/config"
)

// reopenTimeout bounds how long a second New is allowed to spend reopening a
// data directory the previous attempt failed on. Opening BoltDB twice takes
// milliseconds; anything near this budget means the first handle is still held.
const reopenTimeout = 15 * time.Second

// noopFSM stands in for the real CppFSM. Every test in this file either fails
// before Raft is constructed or never applies an entry, so the FSM only has to
// exist.
type noopFSM struct{}

var _ raft.FSM = (*noopFSM)(nil)

func (*noopFSM) Apply(*raft.Log) interface{}          { return nil }
func (*noopFSM) Snapshot() (raft.FSMSnapshot, error)  { return nil, nil }
func (*noopFSM) Restore(snapshot io.ReadCloser) error { return snapshot.Close() }

// testConfig returns a Config that New can actually get all the way through:
// a bindable port, an advertisable address, and valid snapshot tunables.
//
// RaftPort "0" lets the kernel pick the port. The advertise address must still
// be a concrete IP — raft refuses to advertise an unspecified address — so it
// is pinned to loopback, which is correct for a node that talks to nobody.
func testConfig(dataDir string) *config.Config {
	return &config.Config{
		NodeID:            "node1",
		RaftPort:          "0",
		RaftAdvertise:     "127.0.0.1",
		DataDir:           dataDir,
		SnapshotInterval:  config.DefaultSnapshotInterval,
		SnapshotThreshold: config.DefaultSnapshotThreshold,
		TrailingLogs:      config.DefaultTrailingLogs,
	}
}

// TestNewRaftConfig is the unit-level half of R3.6: the tunables that bound the
// log have to reach raft.Config, and nothing else may be disturbed on the way.
func TestNewRaftConfig(t *testing.T) {
	tests := []struct {
		name string
		cfg  *config.Config
	}{
		{
			name: "production defaults",
			cfg: &config.Config{
				NodeID:            "node1",
				SnapshotInterval:  config.DefaultSnapshotInterval,
				SnapshotThreshold: config.DefaultSnapshotThreshold,
				TrailingLogs:      config.DefaultTrailingLogs,
			},
		},
		{
			// What the e2e suite runs with: snapshot early and often, and keep
			// almost nothing behind the snapshot so truncation is observable.
			name: "aggressive test tunables",
			cfg: &config.Config{
				NodeID:            "node2",
				SnapshotInterval:  time.Second,
				SnapshotThreshold: 8,
				TrailingLogs:      4,
			},
		},
		{
			// PIN: zero values are passed through unmodified rather than being
			// silently replaced by the defaults. A Config that was never given
			// snapshot settings must fail loudly in raft.ValidateConfig (see
			// TestNewRejectsInvalidSnapshotInterval), not quietly run with
			// settings nobody asked for.
			name: "zero values are not substituted",
			cfg:  &config.Config{NodeID: "node3"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := newRaftConfig(tc.cfg)

			if string(got.LocalID) != tc.cfg.NodeID {
				t.Errorf("LocalID = %q, want %q", got.LocalID, tc.cfg.NodeID)
			}
			if got.SnapshotInterval != tc.cfg.SnapshotInterval {
				t.Errorf("SnapshotInterval = %s, want %s", got.SnapshotInterval, tc.cfg.SnapshotInterval)
			}
			if got.SnapshotThreshold != tc.cfg.SnapshotThreshold {
				t.Errorf("SnapshotThreshold = %d, want %d", got.SnapshotThreshold, tc.cfg.SnapshotThreshold)
			}
			if got.TrailingLogs != tc.cfg.TrailingLogs {
				t.Errorf("TrailingLogs = %d, want %d", got.TrailingLogs, tc.cfg.TrailingLogs)
			}

			// Everything else must still be raft's default. Compared against a
			// freshly built DefaultConfig rather than literals, so a library
			// upgrade that retunes the timeouts does not fail this test.
			defaults := raft.DefaultConfig()
			if got.HeartbeatTimeout != defaults.HeartbeatTimeout {
				t.Errorf("HeartbeatTimeout = %s, want the raft default %s", got.HeartbeatTimeout, defaults.HeartbeatTimeout)
			}
			if got.ElectionTimeout != defaults.ElectionTimeout {
				t.Errorf("ElectionTimeout = %s, want the raft default %s", got.ElectionTimeout, defaults.ElectionTimeout)
			}
			if got.MaxAppendEntries != defaults.MaxAppendEntries {
				t.Errorf("MaxAppendEntries = %d, want the raft default %d", got.MaxAppendEntries, defaults.MaxAppendEntries)
			}
		})
	}
}

// TestNewSnapshotStoreIsFileBacked is the other half of R3.6: the store must be
// the file store, not the discard store that made snapshots pointless.
func TestNewSnapshotStoreIsFileBacked(t *testing.T) {
	dir := t.TempDir()

	store, err := newSnapshotStore(dir)
	if err != nil {
		t.Fatalf("newSnapshotStore(%q): %v", dir, err)
	}

	if _, ok := store.(*raft.FileSnapshotStore); !ok {
		t.Fatalf("store is %T, want *raft.FileSnapshotStore", store)
	}

	// FileSnapshotStore keeps its snapshots in a subdirectory of the data dir,
	// which is where the e2e suite looks for evidence that one was taken.
	info, err := os.Stat(filepath.Join(dir, "snapshots"))
	if err != nil {
		t.Fatalf("stat of the snapshots directory: %v", err)
	}
	if !info.IsDir() {
		t.Errorf("%s is not a directory", filepath.Join(dir, "snapshots"))
	}

	snapshots, err := store.List()
	if err != nil {
		t.Fatalf("List() on a fresh store: %v", err)
	}
	if len(snapshots) != 0 {
		t.Errorf("List() on a fresh store returned %d snapshots, want 0", len(snapshots))
	}
}

// TestNewSnapshotStoreReportsFailure covers the error return that the discard
// store did not have — the reason New has to check it at all.
func TestNewSnapshotStoreReportsFailure(t *testing.T) {
	dir := t.TempDir()

	// A regular file where the store wants its directory. This is the
	// filesystem-level equivalent of an unwritable data volume, and unlike a
	// permission bit it still fails when the tests run as root (which they do
	// in the CI containers).
	if err := os.WriteFile(filepath.Join(dir, "snapshots"), []byte("not a directory"), 0600); err != nil {
		t.Fatalf("planting the blocking file: %v", err)
	}

	store, err := newSnapshotStore(dir)
	if err == nil {
		t.Fatalf("newSnapshotStore(%q) = %v, want an error", dir, store)
	}
	if !strings.Contains(err.Error(), "failed to create snapshot store") {
		t.Errorf("error = %q, want it to say the snapshot store could not be created", err)
	}
	if !strings.Contains(err.Error(), dir) {
		t.Errorf("error = %q, want it to name the directory %q", err, dir)
	}
}

// TestNewFailurePaths walks the startup failures New can hit before Raft
// exists. Each one is a real operational condition (a bad -data path, a
// half-deleted data volume), and each must be reported with enough context to
// tell which stage failed.
func TestNewFailurePaths(t *testing.T) {
	tests := []struct {
		name string
		// setup returns the DataDir to run New against.
		setup   func(t *testing.T) string
		wantErr string
	}{
		{
			name: "data directory cannot be created",
			setup: func(t *testing.T) string {
				base := t.TempDir()
				blocker := filepath.Join(base, "not-a-dir")
				if err := os.WriteFile(blocker, []byte("x"), 0600); err != nil {
					t.Fatalf("planting the blocking file: %v", err)
				}
				return filepath.Join(blocker, "data")
			},
			wantErr: "failed to create data directory",
		},
		{
			name: "log store cannot be opened",
			setup: func(t *testing.T) string {
				dir := t.TempDir()
				// BoltDB cannot open a directory as its database file.
				if err := os.Mkdir(filepath.Join(dir, "logs.dat"), 0700); err != nil {
					t.Fatalf("planting the blocking directory: %v", err)
				}
				return dir
			},
			wantErr: "failed to create log store",
		},
		{
			name: "snapshot store cannot be created",
			setup: func(t *testing.T) string {
				dir := t.TempDir()
				if err := os.WriteFile(filepath.Join(dir, "snapshots"), []byte("x"), 0600); err != nil {
					t.Fatalf("planting the blocking file: %v", err)
				}
				return dir
			},
			wantErr: "failed to create snapshot store",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			dataDir := tc.setup(t)

			node, err := New(testConfig(dataDir), &noopFSM{}, nil)
			if err == nil {
				t.Fatalf("New() succeeded, want an error; node = %v", node)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error = %q, want it to contain %q", err, tc.wantErr)
			}
			if node != nil {
				t.Errorf("New() returned a node alongside an error: %v", node)
			}
		})
	}
}

// TestNewReleasesTheLogStoreOnFailure is the reason the failure paths close the
// BoltDB handle: it holds an exclusive lock on logs.dat, so a leaked handle
// would turn one recoverable startup error into a node that can never start
// again. Retrying after the blocking condition is cleared must just work.
func TestNewReleasesTheLogStoreOnFailure(t *testing.T) {
	dir := t.TempDir()
	blocker := filepath.Join(dir, "snapshots")
	if err := os.WriteFile(blocker, []byte("x"), 0600); err != nil {
		t.Fatalf("planting the blocking file: %v", err)
	}

	if _, err := New(testConfig(dir), &noopFSM{}, nil); err == nil {
		t.Fatal("New() succeeded with a blocked snapshot path, want an error")
	}

	if err := os.Remove(blocker); err != nil {
		t.Fatalf("clearing the blocking file: %v", err)
	}

	// The second attempt reopens the same logs.dat. BoltDB is opened with no
	// lock timeout, so a leaked handle from the first attempt does not produce
	// an error — it blocks forever. Hence the goroutine and the deadline: a
	// regression here has to fail the test, not wedge the suite until the
	// package-level `go test` timeout kills it ten minutes later.
	type result struct {
		node *Node
		err  error
	}
	done := make(chan result, 1)
	go func() {
		node, err := New(testConfig(dir), &noopFSM{}, nil)
		done <- result{node: node, err: err}
	}()

	select {
	case got := <-done:
		if got.err != nil {
			t.Fatalf("New() after clearing the blocker: %v", got.err)
		}
		shutdown(t, got.node)
	case <-time.After(reopenTimeout):
		t.Fatalf("New() was still blocked reopening logs.dat after %s: "+
			"the failed attempt leaked the BoltDB handle and its exclusive lock", reopenTimeout)
	}
}

// TestNewRejectsInvalidSnapshotInterval proves the tunables are not merely
// stored on a raft.Config that New then ignores: an interval Raft considers
// invalid has to surface as a startup failure.
//
// This is the only test here that gets as far as binding a port, because the
// transport is built before raft.NewRaft validates anything.
func TestNewRejectsInvalidSnapshotInterval(t *testing.T) {
	cfg := testConfig(t.TempDir())
	cfg.SnapshotInterval = time.Nanosecond

	node, err := New(cfg, &noopFSM{}, nil)
	if err == nil {
		shutdown(t, node)
		t.Fatal("New() accepted a 1ns snapshot interval, want an error")
	}
	if !strings.Contains(err.Error(), "failed to create raft instance") {
		t.Errorf("error = %q, want it to name the raft setup stage", err)
	}
	// The wrapped cause is raft.ValidateConfig's own message; matching on the
	// field name rather than the whole sentence keeps this robust across
	// library versions while still proving it is *this* setting that was
	// rejected.
	if !strings.Contains(err.Error(), "SnapshotInterval") {
		t.Errorf("error = %q, want it to blame SnapshotInterval", err)
	}
}

// TestNewSucceedsAndReportsIndices covers the happy path and the two accessors
// the management API depends on.
//
// The Stats() assertions matter more than they look: management/server.go picks
// four keys out of that map by name, and nothing else in the tree would notice
// if a raft upgrade renamed one — the /status fields would just quietly read 0
// forever, and the e2e compaction assertions would quietly stop meaning
// anything.
func TestNewSucceedsAndReportsIndices(t *testing.T) {
	dir := t.TempDir()

	node, err := New(testConfig(dir), &noopFSM{}, nil)
	if err != nil {
		t.Fatalf("New(): %v", err)
	}
	shutdown(t, node)

	// A node that has never been bootstrapped has an empty log.
	firstIndex, err := node.FirstLogIndex()
	if err != nil {
		t.Fatalf("FirstLogIndex(): %v", err)
	}
	if firstIndex != 0 {
		t.Errorf("FirstLogIndex() on a fresh node = %d, want 0", firstIndex)
	}

	stats := node.Stats()
	for _, key := range []string{
		"last_log_index",
		"last_snapshot_index",
		"applied_index",
		"commit_index",
	} {
		if _, ok := stats[key]; !ok {
			t.Errorf("Stats() has no %q key; management/server.go reads it by name", key)
		}
	}

	if node.IsLeader() {
		t.Error("IsLeader() = true on a node that was never bootstrapped")
	}
}

// shutdown tears the node down in dependency order: Raft first (it is the only
// thing still using the transport and the log store), then the transport, then
// the store. Registered as a cleanup so it also runs when the test fails
// midway; without it the BoltDB handle would outlive t.TempDir()'s removal.
func shutdown(t *testing.T, node *Node) {
	t.Helper()

	t.Cleanup(func() {
		if err := node.Raft.Shutdown().Error(); err != nil {
			t.Errorf("Raft shutdown: %v", err)
		}
		if err := node.Transport.Close(); err != nil {
			t.Errorf("transport close: %v", err)
		}
		if err := node.logStore.Close(); err != nil {
			t.Errorf("log store close: %v", err)
		}
	})
}
