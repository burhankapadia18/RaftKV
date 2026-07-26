package config

import (
	"flag"
	"strings"
	"testing"
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
	{name: "app", defValue: "localhost:50051", usage: "Address of C++ App gRPC"},
	{name: "mgmt", defValue: "6000", usage: "Management HTTP Port"},
	{name: "bootstrap", defValue: "false", usage: "Bootstrap the cluster (Leader only)"},
	{name: "data", defValue: "raft-data", usage: "Directory to store Raft logs"},
	{name: "join", defValue: "", usage: "Address of Leader's Management API to join"},
	{name: "advertise", defValue: "", usage: "Address to advertise to other nodes"},
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

func TestConfigString(t *testing.T) {
	cfg := Config{
		NodeID:        "node1",
		RaftPort:      "8088",
		SidecarPort:   "50052",
		AppAddr:       "localhost:50051",
		MgmtPort:      "6000",
		Bootstrap:     true,
		DataDir:       "/data",
		JoinAddr:      "node1:6000",
		RaftAdvertise: "node3",
	}

	want := "Config{NodeID: node1, RaftPort: 8088, SidecarPort: 50052, " +
		"AppAddr: localhost:50051, MgmtPort: 6000, Bootstrap: true, DataDir: /data}"

	if got := cfg.String(); got != want {
		t.Errorf("String() =\n\t%q\nwant\n\t%q", got, want)
	}
}

func TestConfigStringOmitsJoinAndAdvertise(t *testing.T) {
	// PIN: String() deliberately (or accidentally) leaves JoinAddr and RaftAdvertise
	// out of the startup log line. Anything changing the log format must update this.
	cfg := Config{
		NodeID:        "node1",
		RaftPort:      "8088",
		SidecarPort:   "50052",
		AppAddr:       "localhost:50051",
		MgmtPort:      "6000",
		DataDir:       "/data",
		JoinAddr:      "leader-mgmt:6000",
		RaftAdvertise: "advertised-host",
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

	want := "Config{NodeID: , RaftPort: , SidecarPort: , AppAddr: , MgmtPort: , Bootstrap: false, DataDir: }"

	if got := cfg.String(); got != want {
		t.Errorf("String() = %q, want %q", got, want)
	}
}
