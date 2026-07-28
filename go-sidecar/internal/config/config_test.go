package config

import (
	"flag"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/hashicorp/raft"
)

// NOTE ON TESTABILITY (Phase 0 pins the current shape, it does not change it):
//
// The flags are registered on the *global* flag.CommandLine inside this package's
// init(), and Parse() calls the global flag.Parse(). Under `go test` the global
// FlagSet is owned by the testing package (it holds -test.* flags and is parsed by
// testing.M.Run), so calling config.Parse() from a test would either re-parse the
// test binary's own arguments or fight with the test harness. There is therefore no
// test here that calls Parse().
//
// Instead we pin the CLI contract by inspecting the registered flags directly, and
// test the pure methods on a hand-built Config. Extracting a testable
// `ParseArgs(args []string) (*Config, error)` over a private *flag.FlagSet belongs
// to a later phase; when that lands, add a real parsing test alongside these.

// expectedFlags is the complete CLI surface of the sidecar. Adding, renaming or
// re-defaulting a flag is a user-visible contract change (entrypoint.sh and
// docker-compose.yml pass these) and must break this test deliberately.
var expectedFlags = []struct {
	name     string
	defValue string
	usage    string
}{
	{name: "id", defValue: "node1", usage: "Unique Node ID"},
	{name: "raft", defValue: "8088", usage: "Raft TCP Port"},
	{name: "srv", defValue: "50052", usage: "Sidecar gRPC Port"},
	{name: "peer-rpc-port", defValue: "50052",
		usage: "Port a PEER's RaftNode gRPC listens on, for forwarding to the leader"},
	{name: "app", defValue: "localhost:50051", usage: "Address of C++ App gRPC"},
	{name: "mgmt", defValue: "6000", usage: "Management HTTP Port"},
	{name: "bootstrap", defValue: "false", usage: "Bootstrap the cluster (Leader only)"},
	{name: "data", defValue: "raft-data", usage: "Directory to store Raft logs"},
	{name: "join", defValue: "", usage: "Address of Leader's Management API to join"},
	{name: "advertise", defValue: "", usage: "Address to advertise to other nodes"},
	// Phase 3 (R3.6). These three are not decoration: the e2e snapshot suite
	// drives all of them far below the production defaults, so their names are
	// part of the contract with docker-compose/entrypoint.sh just as much as
	// -data or -bootstrap are.
	{name: "snapshot-interval", defValue: "2m0s", usage: "How often to check whether a Raft snapshot is due"},
	{name: "snapshot-threshold", defValue: "8192", usage: "Applied entries since the last snapshot before a new one is taken"},
	{name: "trailing-logs", defValue: "10240", usage: "Log entries to retain behind a snapshot"},
	// R5.1: the slog threshold. Part of the same contract — an operator turns
	// debug on through this flag or the LOG_LEVEL env var entrypoint.sh forwards.
	{name: "log-level", defValue: "info", usage: "Log threshold: debug|info|warn|error"},
}

// TestSnapshotFlagDefaultsMatchConstants ties the flag defaults to the exported
// Default* constants. Without this the two can drift: raftnode and any future
// programmatic caller read the constants, while a container reads the flags.
func TestSnapshotFlagDefaultsMatchConstants(t *testing.T) {
	tests := []struct {
		flagName string
		want     string
	}{
		{flagName: "snapshot-interval", want: DefaultSnapshotInterval.String()},
		{flagName: "snapshot-threshold", want: strconv.FormatUint(DefaultSnapshotThreshold, 10)},
		{flagName: "trailing-logs", want: strconv.FormatUint(DefaultTrailingLogs, 10)},
		{flagName: "log-level", want: DefaultLogLevel},
	}

	for _, tt := range tests {
		t.Run(tt.flagName, func(t *testing.T) {
			f := flag.Lookup(tt.flagName)
			if f == nil {
				t.Fatalf("flag -%s is not registered on flag.CommandLine", tt.flagName)
			}
			if f.DefValue != tt.want {
				t.Errorf("flag -%s default = %q, constant = %q", tt.flagName, f.DefValue, tt.want)
			}
		})
	}
}

// TestSnapshotDefaultsMatchRaftDefaults checks the values against the library
// they are copied from, rather than against literals — a literal here would
// only restate the constant.
//
// The point is that a node started with no snapshot flags must behave exactly
// as hashicorp/raft intends: Phase 3 changes whether snapshots are *kept*, not
// how often they are taken. If a raft upgrade retunes these, this test is the
// notification, and the decision is whether to follow the library or to pin our
// own values on purpose.
func TestSnapshotDefaultsMatchRaftDefaults(t *testing.T) {
	raftDefaults := raft.DefaultConfig()

	tests := []struct {
		name string
		got  uint64
		want uint64
	}{
		{name: "SnapshotInterval", got: uint64(DefaultSnapshotInterval), want: uint64(raftDefaults.SnapshotInterval)},
		{name: "SnapshotThreshold", got: DefaultSnapshotThreshold, want: raftDefaults.SnapshotThreshold},
		{name: "TrailingLogs", got: DefaultTrailingLogs, want: raftDefaults.TrailingLogs},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.got != tt.want {
				t.Errorf("Default%s = %d, raft.DefaultConfig() uses %d", tt.name, tt.got, tt.want)
			}
		})
	}
}

func TestRegisteredFlags(t *testing.T) {
	for _, tt := range expectedFlags {
		t.Run(tt.name, func(t *testing.T) {
			f := flag.Lookup(tt.name)
			if f == nil {
				t.Fatalf("flag -%s is not registered on flag.CommandLine", tt.name)
			}
			if f.DefValue != tt.defValue {
				t.Errorf("flag -%s default = %q, want %q", tt.name, f.DefValue, tt.defValue)
			}
			if f.Usage != tt.usage {
				t.Errorf("flag -%s usage = %q, want %q", tt.name, f.Usage, tt.usage)
			}
		})
	}
}

func TestNoUnexpectedFlagsAreRegistered(t *testing.T) {
	known := make(map[string]bool, len(expectedFlags))
	for _, tt := range expectedFlags {
		known[tt.name] = true
	}

	seen := make(map[string]bool, len(expectedFlags))
	flag.VisitAll(func(f *flag.Flag) {
		// The test binary registers its own -test.* flags on the same global
		// FlagSet. Every flag this package owns is a bare, undotted name.
		if strings.Contains(f.Name, ".") {
			return
		}
		if !known[f.Name] {
			t.Errorf("unexpected flag -%s registered on flag.CommandLine", f.Name)
			return
		}
		seen[f.Name] = true
	})

	for name := range known {
		if !seen[name] {
			t.Errorf("flag -%s was not visited by flag.VisitAll", name)
		}
	}
}

func TestBindAddr(t *testing.T) {
	tests := []struct {
		name string
		cfg  Config
		want string
	}{
		{
			name: "default raft port",
			cfg:  Config{RaftPort: "8088"},
			want: "0.0.0.0:8088",
		},
		{
			name: "custom raft port",
			cfg:  Config{RaftPort: "9999"},
			want: "0.0.0.0:9999",
		},
		{
			// PIN: there is no validation of RaftPort; an empty port yields a
			// syntactically broken address that only fails later, at listen time.
			name: "PIN: empty raft port produces a trailing colon",
			cfg:  Config{RaftPort: ""},
			want: "0.0.0.0:",
		},
		{
			// PIN: RaftAdvertise never influences the bind address.
			name: "advertise address is ignored",
			cfg:  Config{RaftPort: "8088", RaftAdvertise: "node2"},
			want: "0.0.0.0:8088",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := tt.cfg.BindAddr(); got != tt.want {
				t.Errorf("BindAddr() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestAdvertiseAddr(t *testing.T) {
	tests := []struct {
		name string
		cfg  Config
		want string
	}{
		{
			name: "falls back to bind addr when advertise is unset",
			cfg:  Config{RaftPort: "8088"},
			want: "0.0.0.0:8088",
		},
		{
			name: "hostname advertise gets the raft port appended",
			cfg:  Config{RaftPort: "8088", RaftAdvertise: "node2"},
			want: "node2:8088",
		},
		{
			name: "ip advertise gets the raft port appended",
			cfg:  Config{RaftPort: "9000", RaftAdvertise: "10.0.0.7"},
			want: "10.0.0.7:9000",
		},
		{
			// PIN: the advertise value is concatenated blindly. Passing an already
			// host:port value produces a double port rather than an error.
			name: "PIN: advertise already containing a port is not detected",
			cfg:  Config{RaftPort: "8088", RaftAdvertise: "node2:8088"},
			want: "node2:8088:8088",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := tt.cfg.AdvertiseAddr(); got != tt.want {
				t.Errorf("AdvertiseAddr() = %q, want %q", got, tt.want)
			}
		})
	}
}

// TestConfigString pins the startup log line.
//
// PIN: Phase 3 (R3.6) appended the three snapshot tunables. That is deliberate
// — main.go logs this line, and it is the only place an operator can confirm
// that the aggressive values a test container was launched with arrived. The
// pre-Phase-3 prefix is unchanged, so anything grepping for the earlier fields
// still matches.
func TestConfigString(t *testing.T) {
	cfg := Config{
		NodeID:            "node1",
		RaftPort:          "8088",
		SidecarPort:       "50052",
		PeerRPCPort:       "50052",
		AppAddr:           "localhost:50051",
		MgmtPort:          "6000",
		Bootstrap:         true,
		DataDir:           "/data",
		JoinAddr:          "node1:6000",
		RaftAdvertise:     "node3",
		SnapshotInterval:  5 * time.Second,
		SnapshotThreshold: 64,
		TrailingLogs:      32,
	}

	want := "Config{NodeID: node1, RaftPort: 8088, SidecarPort: 50052, " +
		"PeerRPCPort: 50052, " +
		"AppAddr: localhost:50051, MgmtPort: 6000, Bootstrap: true, DataDir: /data, " +
		"SnapshotInterval: 5s, SnapshotThreshold: 64, TrailingLogs: 32}"

	if got := cfg.String(); got != want {
		t.Errorf("String() =\n\t%q\nwant\n\t%q", got, want)
	}
}

func TestConfigStringOmitsJoinAndAdvertise(t *testing.T) {
	// PIN: String() deliberately (or accidentally) leaves JoinAddr and RaftAdvertise
	// out of the startup log line. Anything changing the log format must update this.
	cfg := Config{
		NodeID:            "node1",
		RaftPort:          "8088",
		SidecarPort:       "50052",
		AppAddr:           "localhost:50051",
		MgmtPort:          "6000",
		DataDir:           "/data",
		JoinAddr:          "leader-mgmt:6000",
		RaftAdvertise:     "advertised-host",
		SnapshotInterval:  DefaultSnapshotInterval,
		SnapshotThreshold: DefaultSnapshotThreshold,
		TrailingLogs:      DefaultTrailingLogs,
	}

	got := cfg.String()
	for _, omitted := range []string{"leader-mgmt:6000", "advertised-host", "JoinAddr", "RaftAdvertise"} {
		if strings.Contains(got, omitted) {
			t.Errorf("String() = %q, must not contain %q", got, omitted)
		}
	}
}

func TestConfigStringZeroValue(t *testing.T) {
	var cfg Config

	want := "Config{NodeID: , RaftPort: , SidecarPort: , PeerRPCPort: , AppAddr: , MgmtPort: , Bootstrap: false, DataDir: , " +
		"SnapshotInterval: 0s, SnapshotThreshold: 0, TrailingLogs: 0}"

	if got := cfg.String(); got != want {
		t.Errorf("String() = %q, want %q", got, want)
	}
}
