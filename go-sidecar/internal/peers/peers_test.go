package peers

import (
	"strings"
	"testing"
)

func TestRPCAddr(t *testing.T) {
	tests := []struct {
		name     string
		raftAddr string
		want     string
		wantErr  string
	}{
		{
			name:     "docker compose hostname",
			raftAddr: "node1:8088",
			want:     "node1:50052",
		},
		{
			name:     "resolved IPv4, which is what LeaderWithID actually returns",
			raftAddr: "172.18.0.2:8088",
			want:     "172.18.0.2:50052",
		},
		{
			name:     "IPv6 comes back bracketed",
			raftAddr: "[::1]:8088",
			want:     "[::1]:50052",
		},
		{
			name: "no leader known yet",
			// LeaderWithID returns "" during an election. This is the common
			// case, not an exotic one, so the message says so.
			raftAddr: "",
			wantErr:  "no leader address known",
		},
		{
			name:     "whitespace is not an address either",
			raftAddr: "   ",
			wantErr:  "no leader address known",
		},
		{
			name:     "missing port",
			raftAddr: "node1",
			wantErr:  "not a host:port raft address",
		},
		{
			name:     "port only",
			raftAddr: ":8088",
			wantErr:  "has no host part",
		},
	}

	resolver := New("50052", "6000")

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, err := resolver.RPCAddr(tc.raftAddr)

			if tc.wantErr != "" {
				if err == nil {
					t.Fatalf("RPCAddr(%q) = %q, want an error containing %q",
						tc.raftAddr, got, tc.wantErr)
				}
				if !strings.Contains(err.Error(), tc.wantErr) {
					t.Errorf("error = %q, want it to contain %q", err, tc.wantErr)
				}
				if got != "" {
					t.Errorf("RPCAddr returned %q alongside an error", got)
				}
				return
			}

			if err != nil {
				t.Fatalf("RPCAddr(%q): %v", tc.raftAddr, err)
			}
			if got != tc.want {
				t.Errorf("RPCAddr(%q) = %q, want %q", tc.raftAddr, got, tc.want)
			}
		})
	}
}

func TestMgmtAddr(t *testing.T) {
	resolver := New("50052", "6000")

	got, err := resolver.MgmtAddr("node2:8088")
	if err != nil {
		t.Fatalf("MgmtAddr: %v", err)
	}
	if want := "node2:6000"; got != want {
		t.Errorf("MgmtAddr = %q, want %q", got, want)
	}
}

// TestOnlyThePortChanges is the property the whole package exists to provide:
// the host is carried through untouched and ONLY the port is swapped. A
// resolver that helpfully rewrote the host would send a forwarded request to
// the wrong machine.
func TestOnlyThePortChanges(t *testing.T) {
	resolver := New("1234", "5678")

	for _, host := range []string{"node1", "10.0.0.7", "kv-2.svc.cluster.local"} {
		rpc, err := resolver.RPCAddr(host + ":8088")
		if err != nil {
			t.Fatalf("RPCAddr for %q: %v", host, err)
		}
		if want := host + ":1234"; rpc != want {
			t.Errorf("RPCAddr = %q, want %q", rpc, want)
		}

		mgmt, err := resolver.MgmtAddr(host + ":8088")
		if err != nil {
			t.Fatalf("MgmtAddr for %q: %v", host, err)
		}
		if want := host + ":5678"; mgmt != want {
			t.Errorf("MgmtAddr = %q, want %q", mgmt, want)
		}
	}
}

func TestUnconfiguredPortIsReported(t *testing.T) {
	resolver := New("", "")

	if _, err := resolver.RPCAddr("node1:8088"); err == nil ||
		!strings.Contains(err.Error(), "no target port configured") {
		t.Errorf("RPCAddr with no port = %v, want a 'no target port' error", err)
	}
}
