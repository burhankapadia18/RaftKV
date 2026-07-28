package config

import (
	"flag"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/hashicorp/raft"

	"my-raft-sidecar/internal/tlsconfig"
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
	// R6.1: the cluster-admin token. Its DEFAULT is deliberately not pinned here
	// — it comes from RAFTKV_MGMT_TOKEN, so a fixed expectation would fail on any
	// machine that happens to have the variable set, including a real deployment.
	// The name and usage are the contract; the value is environment.
	{name: "mgmt-token", defValue: anyValue, usage: "Cluster-admin bearer token for /join and /remove (prefer RAFTKV_MGMT_TOKEN)"},
	// R6.3/R6.4. All six default to empty: security is opt-in, so a node started
	// without them behaves exactly as it did before Phase 6.
	{name: "raft-tls-cert", defValue: "", usage: "Certificate this node presents to raft peers (enables mutual TLS on the raft port)"},
	{name: "raft-tls-key", defValue: "", usage: "Private key for -raft-tls-cert"},
	{name: "raft-tls-ca", defValue: "", usage: "CA that must have signed a raft peer's certificate"},
	{name: "mgmt-tls-cert", defValue: "", usage: "Certificate for the management HTTPS listener"},
	{name: "mgmt-tls-key", defValue: "", usage: "Private key for -mgmt-tls-cert"},
	{name: "mgmt-tls-ca", defValue: "", usage: "CA used to verify a PEER's management API when joining or relaying over HTTPS"},
}

// anyValue marks a flag whose default is environment-derived and therefore not
// pinnable. The table check skips comparing defValue when it sees this.
const anyValue = "\x00any"

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
			// anyValue: the default is environment-derived, so pinning it would
			// fail wherever the variable happens to be set — including production.
			if tt.defValue != anyValue && f.DefValue != tt.defValue {
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
		"SnapshotInterval: 5s, SnapshotThreshold: 64, TrailingLogs: 32, RaftTLS: false, MgmtTLS: false, MgmtAuth: false}"

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
		"SnapshotInterval: 0s, SnapshotThreshold: 0, TrailingLogs: 0, RaftTLS: false, MgmtTLS: false, MgmtAuth: false}"

	if got := cfg.String(); got != want {
		t.Errorf("String() = %q, want %q", got, want)
	}
}

// TestConfigStringReportsTLSStateWithoutLeakingIt is the test that matters more
// than the exact format: String() is logged at startup, and the log stream is
// read by anyone with access to `docker compose logs`. An operator needs to know
// whether a surface is protected; they must not learn the token from it.
func TestConfigStringReportsTLSStateWithoutLeakingIt(t *testing.T) {
	cfg := Config{
		NodeID:    "node1",
		MgmtToken: "s3cr3t-admin-token",
		RaftTLS: tlsconfig.Material{
			CertFile: "/certs/node1.pem",
			KeyFile:  "/certs/node1-key.pem",
			CAFile:   "/certs/ca.pem",
		},
		MgmtTLS: tlsconfig.Material{
			CertFile: "/certs/mgmt.pem",
			KeyFile:  "/certs/mgmt-key.pem",
		},
	}

	got := cfg.String()

	for _, want := range []string{"RaftTLS: true", "MgmtTLS: true", "MgmtAuth: true"} {
		if !strings.Contains(got, want) {
			t.Errorf("String() = %q, missing %q", got, want)
		}
	}
	for _, leak := range []string{"s3cr3t-admin-token", "/certs/node1-key.pem", "/certs/mgmt-key.pem"} {
		if strings.Contains(got, leak) {
			t.Errorf("String() leaked %q into the log stream: %q", leak, got)
		}
	}
}

func TestConfigValidate(t *testing.T) {
	pair := tlsconfig.Material{CertFile: "c", KeyFile: "k", CAFile: "a"}

	tests := []struct {
		name    string
		cfg     Config
		wantErr string
	}{
		{
			name: "no TLS at all is valid",
			cfg:  Config{},
		},
		{
			name: "full raft material is valid",
			cfg:  Config{RaftTLS: pair},
		},
		{
			name:    "raft cert without CA is rejected",
			cfg:     Config{RaftTLS: tlsconfig.Material{CertFile: "c", KeyFile: "k"}},
			wantErr: "-raft-tls-ca is required",
		},
		{
			name:    "raft cert without key is rejected",
			cfg:     Config{RaftTLS: tlsconfig.Material{CertFile: "c", CAFile: "a"}},
			wantErr: "needs both",
		},
		{
			// The management listener is deliberately NOT mutual — probes and
			// Prometheus hold no cluster certificate — so a CA is optional here.
			name: "management cert without CA is valid",
			cfg:  Config{MgmtTLS: tlsconfig.Material{CertFile: "c", KeyFile: "k"}},
		},
		{
			name:    "management key without cert is rejected",
			cfg:     Config{MgmtTLS: tlsconfig.Material{KeyFile: "k"}},
			wantErr: "needs both",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.cfg.Validate()
			switch {
			case tt.wantErr == "" && err != nil:
				t.Fatalf("unexpected error: %v", err)
			case tt.wantErr != "" && err == nil:
				t.Fatalf("expected an error containing %q, got nil", tt.wantErr)
			case tt.wantErr != "" && !strings.Contains(err.Error(), tt.wantErr):
				t.Fatalf("error %q does not contain %q", err, tt.wantErr)
			}
		})
	}
}

func TestTLSEnabledPredicates(t *testing.T) {
	off := Config{}
	if off.RaftTLSEnabled() || off.MgmtTLSEnabled() {
		t.Error("a zero Config must report TLS off, or the default demo would try to negotiate it")
	}

	on := Config{
		RaftTLS: tlsconfig.Material{CertFile: "c", KeyFile: "k", CAFile: "a"},
		MgmtTLS: tlsconfig.Material{CertFile: "c", KeyFile: "k"},
	}
	if !on.RaftTLSEnabled() || !on.MgmtTLSEnabled() {
		t.Error("configured material must report TLS on")
	}

	// A CA alone must NOT flip the switch. It cannot: there is no identity to
	// present, so a listener would have nothing to serve — Validate rejects it,
	// and the predicate agreeing keeps the two from disagreeing about the same
	// config.
	caOnly := Config{RaftTLS: tlsconfig.Material{CAFile: "a"}}
	if caOnly.RaftTLSEnabled() {
		t.Error("a CA with no key pair must not count as TLS enabled")
	}
}
