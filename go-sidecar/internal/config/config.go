// Package config provides configuration management for the Raft sidecar.
package config

import (
	"flag"
	"fmt"
	"os"
	"time"

	"my-raft-sidecar/internal/tlsconfig"
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

	// DefaultLogLevel is the slog threshold when -log-level is not given.
	DefaultLogLevel = "info"
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
	// LogLevel is the slog threshold: debug|info|warn|error (R5.1).
	LogLevel string

	// MgmtToken is the cluster-admin bearer token guarding /join and /remove
	// (R6.1). Empty DISABLES those endpoints rather than leaving them open.
	MgmtToken string

	// RaftTLS is the identity this node presents to, and demands from, its raft
	// peers (R6.4). Empty means the peer transport is plaintext.
	//
	// Mutual by construction: one-way TLS on the raft port would encrypt the
	// traffic and still let anyone who can reach it append entries, which is
	// confidentiality without authentication — the wrong half of the problem.
	RaftTLS tlsconfig.Material

	// MgmtTLS is the certificate the management listener serves (R6.3). Its
	// CAFile is used for the OTHER direction: verifying a peer's management API
	// when this node joins a cluster or relays a /join to the leader over HTTPS.
	MgmtTLS tlsconfig.Material
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
	logLevel          *string
	mgmtToken         *string
	raftTLSCert       *string
	raftTLSKey        *string
	raftTLSCA         *string
	mgmtTLSCert       *string
	mgmtTLSKey        *string
	mgmtTLSCA         *string
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
	flags.logLevel = flag.String("log-level", DefaultLogLevel, "Log threshold: debug|info|warn|error")
	// Default comes from the environment so the token never has to appear in a
	// process listing, where any local user could read it off `ps`.
	flags.mgmtToken = flag.String("mgmt-token", os.Getenv("RAFTKV_MGMT_TOKEN"), "Cluster-admin bearer token for /join and /remove (prefer RAFTKV_MGMT_TOKEN)")

	// R6.4 / R6.3. All six default to empty, so a node started without them
	// behaves exactly as it did before Phase 6 — the zero-config demo keeps
	// working, and security is something a deployment opts into.
	flags.raftTLSCert = flag.String("raft-tls-cert", "", "Certificate this node presents to raft peers (enables mutual TLS on the raft port)")
	flags.raftTLSKey = flag.String("raft-tls-key", "", "Private key for -raft-tls-cert")
	flags.raftTLSCA = flag.String("raft-tls-ca", "", "CA that must have signed a raft peer's certificate")
	flags.mgmtTLSCert = flag.String("mgmt-tls-cert", "", "Certificate for the management HTTPS listener")
	flags.mgmtTLSKey = flag.String("mgmt-tls-key", "", "Private key for -mgmt-tls-cert")
	flags.mgmtTLSCA = flag.String("mgmt-tls-ca", "", "CA used to verify a PEER's management API when joining or relaying over HTTPS")
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
		LogLevel:          *flags.logLevel,
		MgmtToken:         *flags.mgmtToken,

		RaftTLS: tlsconfig.Material{
			CertFile: *flags.raftTLSCert,
			KeyFile:  *flags.raftTLSKey,
			CAFile:   *flags.raftTLSCA,
		},
		MgmtTLS: tlsconfig.Material{
			CertFile: *flags.mgmtTLSCert,
			KeyFile:  *flags.mgmtTLSKey,
			CAFile:   *flags.mgmtTLSCA,
		},
	}
}

// Validate rejects a TLS configuration that cannot work, at startup.
//
// Separate from Parse and called explicitly by main, because the failure mode it
// prevents is silent: a mistyped certificate path leaves Material half-specified,
// and a node that treats that as "TLS is off" comes up serving plaintext on a
// port the operator believes is encrypted. Failing to boot is the correct
// response to being unable to secure a listener.
func (c *Config) Validate() error {
	if err := c.RaftTLS.Validate("raft peer"); err != nil {
		return err
	}
	// The raft port is the one surface where a CA is not optional: the transport
	// is mutual, so a node with a cert but no CA could present an identity and
	// verify nobody.
	if c.RaftTLS.Configured() && c.RaftTLS.CAFile == "" {
		return fmt.Errorf("-raft-tls-ca is required with -raft-tls-cert: " +
			"the raft peer transport is mutually authenticated, so this node must " +
			"be able to verify its peers, not only prove itself to them")
	}
	return c.MgmtTLS.Validate("management")
}

// RaftTLSEnabled reports whether the raft peer transport is encrypted.
func (c *Config) RaftTLSEnabled() bool {
	return c.RaftTLS.Configured()
}

// MgmtTLSEnabled reports whether the management listener serves HTTPS. It also
// decides the scheme the joiner and the join/remove forwarder dial peers with:
// a cluster is configured uniformly, so this node's own setting is the right
// predictor of a peer's.
func (c *Config) MgmtTLSEnabled() bool {
	return c.MgmtTLS.Configured()
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
			"SnapshotInterval: %s, SnapshotThreshold: %d, TrailingLogs: %d, RaftTLS: %v, MgmtTLS: %v, MgmtAuth: %v}",
		c.NodeID, c.RaftPort, c.SidecarPort, c.PeerRPCPort, c.AppAddr, c.MgmtPort, c.Bootstrap, c.DataDir,
		c.SnapshotInterval, c.SnapshotThreshold, c.TrailingLogs,
		// Booleans, never the paths and never the token: this line goes to the log
		// stream. An operator needs to know whether the surface is protected, not
		// what protects it.
		c.RaftTLSEnabled(), c.MgmtTLSEnabled(), c.MgmtToken != "",
	)
}
