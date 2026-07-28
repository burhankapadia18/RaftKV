package tlsconfig

import (
	"crypto/tls"
	"path/filepath"
	"strings"
	"testing"

	"my-raft-sidecar/internal/testcerts"
)

// newCA and material adapt the shared generator to this package. testcerts
// cannot return a tlsconfig.Material without importing this package, which would
// be an import cycle — see its package comment.
func newCA(t *testing.T) *testcerts.CA { return testcerts.NewCA(t) }

func material(f testcerts.Files) Material {
	return Material{CertFile: f.CertFile, KeyFile: f.KeyFile, CAFile: f.CAFile}
}

func TestMaterialValidate(t *testing.T) {
	tests := []struct {
		name    string
		m       Material
		wantErr string
	}{
		{"empty is valid (TLS off)", Material{}, ""},
		{"pair is valid", Material{CertFile: "c", KeyFile: "k"}, ""},
		{"pair with CA is valid", Material{CertFile: "c", KeyFile: "k", CAFile: "a"}, ""},
		{"cert without key", Material{CertFile: "c"}, "needs both"},
		{"key without cert", Material{KeyFile: "k"}, "needs both"},
		{"CA alone", Material{CAFile: "a"}, "no identity to present"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := tc.m.Validate("raft")
			switch {
			case tc.wantErr == "" && err != nil:
				t.Fatalf("unexpected error: %v", err)
			case tc.wantErr != "" && err == nil:
				t.Fatalf("expected an error containing %q, got nil", tc.wantErr)
			case tc.wantErr != "" && !strings.Contains(err.Error(), tc.wantErr):
				t.Fatalf("error %q does not contain %q", err, tc.wantErr)
			}
		})
	}
}

func TestMaterialConfigured(t *testing.T) {
	// A half-specified pair must still report Configured, or a typo in one path
	// would take the caller down the "TLS is off" branch and silently serve
	// plaintext — the exact failure Validate exists to catch.
	if !(Material{CertFile: "c"}).Configured() {
		t.Error("cert alone should count as configured so Validate gets a chance to reject it")
	}
	if (Material{}).Configured() {
		t.Error("empty material must not count as configured")
	}
}

func TestServerConfigRequiresClientCertWhenMutual(t *testing.T) {
	ca := newCA(t)
	m := material(ca.Issue(t, "node1", "node1", "127.0.0.1"))

	mutual, err := ServerConfig(m, true)
	if err != nil {
		t.Fatalf("ServerConfig(mutual): %v", err)
	}
	if mutual.ClientAuth != tls.RequireAndVerifyClientCert {
		t.Errorf("ClientAuth = %v, want RequireAndVerifyClientCert; anything weaker "+
			"accepts a client presenting no certificate at all", mutual.ClientAuth)
	}
	if mutual.ClientCAs == nil {
		t.Error("ClientCAs is nil: the listener would demand a client cert and then " +
			"have nothing to verify it against")
	}

	oneWay, err := ServerConfig(m, false)
	if err != nil {
		t.Fatalf("ServerConfig(one-way): %v", err)
	}
	if oneWay.ClientAuth != tls.NoClientCert {
		t.Errorf("ClientAuth = %v, want NoClientCert for a one-way listener", oneWay.ClientAuth)
	}
	if oneWay.MinVersion != tls.VersionTLS12 {
		t.Errorf("MinVersion = %x, want TLS 1.2 floor", oneWay.MinVersion)
	}
}

func TestServerConfigMutualWithoutCAFails(t *testing.T) {
	ca := newCA(t)
	m := material(ca.Issue(t, "node1", "node1"))
	m.CAFile = ""

	if _, err := ServerConfig(m, true); err == nil {
		t.Fatal("expected an error: mutual TLS with no CA cannot verify anyone")
	}
}

func TestClientConfigReplacesSystemRoots(t *testing.T) {
	ca := newCA(t)
	m := material(ca.Issue(t, "node2", "node2"))

	cfg, err := ClientConfig(m)
	if err != nil {
		t.Fatalf("ClientConfig: %v", err)
	}
	if cfg.RootCAs == nil {
		t.Fatal("RootCAs is nil: peers would be verified against the system pool, " +
			"so a certificate from any public CA would authenticate as a cluster member")
	}
	if len(cfg.Certificates) != 1 {
		t.Fatalf("Certificates = %d, want 1: the client must present its own identity for mTLS",
			len(cfg.Certificates))
	}
	if cfg.InsecureSkipVerify {
		t.Error("InsecureSkipVerify is set; that would disable the whole point")
	}
}

func TestClientConfigWithCAOnlyIsRejectedByValidate(t *testing.T) {
	// ClientConfig itself tolerates a CA with no identity (a plain HTTPS client
	// verifying a server needs exactly that), so the guard lives in Validate.
	ca := newCA(t)
	cfg, err := ClientConfig(Material{CAFile: ca.CAPath})
	if err != nil {
		t.Fatalf("ClientConfig(CA only): %v", err)
	}
	if len(cfg.Certificates) != 0 {
		t.Error("no identity was supplied, so none should be presented")
	}
}

func TestLoadCARejectsNonPEM(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "not-a-cert.pem")
	testcerts.WritePEM(t, path, "CERTIFICATE", nil) // an empty block: parses as PEM, holds no cert

	if _, err := loadCA(path); err == nil {
		t.Fatal("expected an error: an empty trust pool rejects every peer with a " +
			"handshake error that says nothing about the real cause")
	}
}

func TestWithServerNameDerivesHostAndDoesNotMutate(t *testing.T) {
	base := &tls.Config{MinVersion: tls.VersionTLS12}

	got := WithServerName(base, "node3:8088")
	if got.ServerName != "node3" {
		t.Errorf("ServerName = %q, want %q", got.ServerName, "node3")
	}
	if base.ServerName != "" {
		t.Error("WithServerName mutated the shared config; raft dials peers concurrently")
	}

	// No port at all: the whole string is the host.
	if got := WithServerName(base, "node3"); got.ServerName != "node3" {
		t.Errorf("ServerName = %q for a portless address, want %q", got.ServerName, "node3")
	}
}

// TestHandshakeEndToEnd is the test that actually matters: it proves a peer
// signed by the cluster CA is accepted and one signed by a different CA is not.
// Every assertion above is about a field's value; this one is about behavior.
func TestHandshakeEndToEnd(t *testing.T) {
	ca := newCA(t)
	server := material(ca.Issue(t, "server", "127.0.0.1"))
	client := material(ca.Issue(t, "client", "client"))

	serverCfg, err := ServerConfig(server, true)
	if err != nil {
		t.Fatalf("ServerConfig: %v", err)
	}
	clientCfg, err := ClientConfig(client)
	if err != nil {
		t.Fatalf("ClientConfig: %v", err)
	}

	t.Run("same CA succeeds", func(t *testing.T) {
		if err := handshake(t, serverCfg, WithServerName(clientCfg, "127.0.0.1:0")); err != nil {
			t.Fatalf("handshake between two peers of the same CA failed: %v", err)
		}
	})

	t.Run("foreign client CA is refused", func(t *testing.T) {
		other := newCA(t)
		intruder := material(other.Issue(t, "intruder", "intruder"))
		// Trust our CA for the server's identity so the failure can only be the
		// client certificate — otherwise the test would pass for the wrong reason.
		intruder.CAFile = ca.CAPath
		intruderCfg, err := ClientConfig(intruder)
		if err != nil {
			t.Fatalf("ClientConfig(intruder): %v", err)
		}
		if err := handshake(t, serverCfg, WithServerName(intruderCfg, "127.0.0.1:0")); err == nil {
			t.Fatal("a client signed by a foreign CA completed the handshake: " +
				"cluster membership is not being enforced")
		}
	})

	t.Run("plaintext client is refused", func(t *testing.T) {
		if err := plaintextTo(t, serverCfg); err == nil {
			t.Fatal("a plaintext connection was accepted by a TLS listener")
		}
	})
}
