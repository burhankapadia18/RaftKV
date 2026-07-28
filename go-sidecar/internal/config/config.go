// Package config provides configuration management for the Raft sidecar.
package config

import (
	"flag"
	"fmt"
	"time"
)

// Snapshot tunables (R3.6). The defaults are hashicorp/raft's own defaults, so
// a node started without these flags behaves exactly as the library intends.
// They are flags rather than constants because the e2e suite has to drive them
// far below production values: with the defaults a cluster would need 8192
// writes and up to two minutes before it ever snapshots, which no test can
// wait for.
const (
	// DefaultSnapshotInterval is how often a node checks whether a snapshot is
	// due. It is a check interval, not a snapshot interval: nothing happens
	// unless SnapshotThreshold entries have accumulated as well.
	DefaultSnapshotInterval = 120 * time.Second

	// DefaultSnapshotThreshold is how many committed entries must have been
	// applied since the last snapshot before the interval check takes one.
	DefaultSnapshotThreshold uint64 = 8192

	// DefaultTrailingLogs is how many entries Raft keeps behind a snapshot so a
	// slightly lagging follower can still be caught up with log entries instead
	// of a full snapshot transfer.
	//
	// This is also the floor on log truncation: after a snapshot at index N the
	// log is trimmed to N-TrailingLogs, so with the default a log shorter than
	// 10240 entries never shrinks at all, however often it is snapshotted. Any
	// test that wants to observe the first log index advancing must lower this.
	DefaultTrailingLogs uint64 = 10240
)

// Config holds all configuration values for the sidecar application.
type Config struct {
	NodeID        string
	RaftPort      string
	SidecarPort   string
	PeerRPCPort   string
	AppAddr       string
	MgmtPort      string
	Bootstrap     bool
	DataDir       string
	JoinAddr      string
	RaftAdvertise string

	// SnapshotInterval, SnapshotThreshold and TrailingLogs are passed straight
	// through to raft.Config; see the Default* constants above.
	SnapshotInterval  time.Duration
	SnapshotThreshold uint64
	TrailingLogs      uint64
}

// flags holds the command-line flag pointers
var flags struct {
	nodeID            *string
	raftPort          *string
	sidecarPort       *string
	peerRPCPort       *string
	appAddr           *string
	mgmtPort          *string
	bootstrap         *bool
	dataDir           *string
	joinAddr          *string
	raftAdvertise     *string
	snapshotInterval  *time.Duration
	snapshotThreshold *uint64
	trailingLogs      *uint64
}

func init() {
	flags.nodeID = flag.String("id", "node1", "Unique Node ID")
	flags.raftPort = flag.String("raft", "8088", "Raft TCP Port")
	flags.sidecarPort = flag.String("srv", "50052", "Sidecar gRPC Port")
	// R4.2: Raft only tells us a peer's Raft address (host:8088). Its RaftNode
	// gRPC lives on the same host at this port. That is a convention, so it is
	// a flag rather than a constant — a deployment that does not use the
	// default would otherwise forward into a black hole.
	flags.peerRPCPort = flag.String("peer-rpc-port", "50052",
		"Port a PEER's RaftNode gRPC listens on, for forwarding to the leader")
	flags.appAddr = flag.String("app", "localhost:50051", "Address of C++ App gRPC")
	flags.mgmtPort = flag.String("mgmt", "6000", "Management HTTP Port")
	flags.bootstrap = flag.Bool("bootstrap", false, "Bootstrap the cluster (Leader only)")
	flags.dataDir = flag.String("data", "raft-data", "Directory to store Raft logs")
	flags.joinAddr = flag.String("join", "", "Address of Leader's Management API to join")
	flags.raftAdvertise = flag.String("advertise", "", "Address to advertise to other nodes")
	flags.snapshotInterval = flag.Duration("snapshot-interval", DefaultSnapshotInterval, "How often to check whether a Raft snapshot is due")
	flags.snapshotThreshold = flag.Uint64("snapshot-threshold", DefaultSnapshotThreshold, "Applied entries since the last snapshot before a new one is taken")
	flags.trailingLogs = flag.Uint64("trailing-logs", DefaultTrailingLogs, "Log entries to retain behind a snapshot")
}

// Parse parses command-line flags and returns a Config.
func Parse() *Config {
	flag.Parse()
	return &Config{
		NodeID:        *flags.nodeID,
		RaftPort:      *flags.raftPort,
		SidecarPort:   *flags.sidecarPort,
		PeerRPCPort:   *flags.peerRPCPort,
		AppAddr:       *flags.appAddr,
		MgmtPort:      *flags.mgmtPort,
		Bootstrap:     *flags.bootstrap,
		DataDir:       *flags.dataDir,
		JoinAddr:      *flags.joinAddr,
		RaftAdvertise: *flags.raftAdvertise,

		SnapshotInterval:  *flags.snapshotInterval,
		SnapshotThreshold: *flags.snapshotThreshold,
		TrailingLogs:      *flags.trailingLogs,
	}
}

// BindAddr returns the address to bind the Raft transport to.
func (c *Config) BindAddr() string {
	return "0.0.0.0:" + c.RaftPort
}

// AdvertiseAddr returns the address to advertise to other nodes.
func (c *Config) AdvertiseAddr() string {
	if c.RaftAdvertise != "" {
		return c.RaftAdvertise + ":" + c.RaftPort
	}
	return c.BindAddr()
}

// String returns a human-readable representation of the config.
//
// The snapshot tunables are included deliberately: main.go logs this line at
// startup, and it is the only place an operator (or a failing e2e run) can
// confirm that the values a container was launched with actually arrived.
// JoinAddr and RaftAdvertise remain omitted, as they were before Phase 3.
func (c *Config) String() string {
	return fmt.Sprintf(
		"Config{NodeID: %s, RaftPort: %s, SidecarPort: %s, PeerRPCPort: %s, AppAddr: %s, MgmtPort: %s, Bootstrap: %v, DataDir: %s, "+
			"SnapshotInterval: %s, SnapshotThreshold: %d, TrailingLogs: %d}",
		c.NodeID, c.RaftPort, c.SidecarPort, c.PeerRPCPort, c.AppAddr, c.MgmtPort, c.Bootstrap, c.DataDir,
		c.SnapshotInterval, c.SnapshotThreshold, c.TrailingLogs,
	)
}
