package cluster

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"my-raft-sidecar/internal/testcerts"
	"my-raft-sidecar/internal/tlsconfig"
)

// TestJoinOverHTTPS is the other half of R6.3: with the management API on TLS, a
// node still has to be able to join. The join is the one request that carries the
// cluster-admin token to a peer, so it is the request that most needs the channel
// encrypted — and the one that would be most damaging to silently downgrade.
func TestJoinOverHTTPS(t *testing.T) {
	ca := testcerts.NewCA(t)
	leader := ca.Issue(t, "leader", "127.0.0.1", "example.com")

	const token = "cluster-admin-token"
	var sawAuth, sawQuery string

	srv := httptest.NewUnstartedServer(http.HandlerFunc(
		func(w http.ResponseWriter, r *http.Request) {
			sawAuth = r.Header.Get("Authorization")
			sawQuery = r.URL.RawQuery
			w.WriteHeader(http.StatusOK)
		}))

	serverCfg, err := tlsconfig.ServerConfig(
		tlsconfig.Material{CertFile: leader.CertFile, KeyFile: leader.KeyFile}, false)
	if err != nil {
		t.Fatalf("ServerConfig: %v", err)
	}
	srv.TLS = serverCfg
	srv.StartTLS()
	defer srv.Close()

	cfg := DefaultJoinConfig(hostPort(t, srv.URL), "node2", "node2:8088")
	cfg.MaxRetries = 1
	cfg.AuthToken = token
	// Only the CA: this listener does not ask for a client certificate, because
	// the caller is authenticated by the bearer token instead.
	cfg.TLS = tlsconfig.Material{CAFile: ca.CAPath}

	joiner, err := NewJoiner(cfg)
	if err != nil {
		t.Fatalf("NewJoiner: %v", err)
	}
	if joiner.scheme != "https" {
		t.Fatalf("scheme = %q, want https; TLS material must flip the scheme, or the "+
			"token would go out in clear", joiner.scheme)
	}
	if err := joiner.Join(); err != nil {
		t.Fatalf("Join over HTTPS: %v", err)
	}
	if sawAuth != "Bearer "+token {
		t.Errorf("Authorization = %q, want the bearer token", sawAuth)
	}
	if sawQuery != "peerID=node2&peerAddress=node2:8088" {
		t.Errorf("query = %q, want the peer's ID and raft address", sawQuery)
	}
}

// TestJoinRejectsUntrustedLeader proves verification is real: the same listener,
// dialled with a foreign CA, must fail. Without this, TestJoinOverHTTPS would
// still pass with InsecureSkipVerify set — which would accept any certificate
// from anyone claiming to be the leader.
func TestJoinRejectsUntrustedLeader(t *testing.T) {
	ca := testcerts.NewCA(t)
	leader := ca.Issue(t, "leader", "127.0.0.1")

	srv := httptest.NewUnstartedServer(http.HandlerFunc(
		func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(http.StatusOK) }))
	serverCfg, err := tlsconfig.ServerConfig(
		tlsconfig.Material{CertFile: leader.CertFile, KeyFile: leader.KeyFile}, false)
	if err != nil {
		t.Fatalf("ServerConfig: %v", err)
	}
	srv.TLS = serverCfg
	srv.StartTLS()
	defer srv.Close()

	other := testcerts.NewCA(t)
	cfg := DefaultJoinConfig(hostPort(t, srv.URL), "node2", "node2:8088")
	cfg.MaxRetries = 1
	cfg.TLS = tlsconfig.Material{CAFile: other.CAPath}

	joiner, err := NewJoiner(cfg)
	if err != nil {
		t.Fatalf("NewJoiner: %v", err)
	}
	if err := joiner.Join(); err == nil {
		t.Fatal("joined a leader whose certificate was signed by an unknown CA")
	}
}

func TestNewJoinerRejectsUnusableTLS(t *testing.T) {
	cfg := DefaultJoinConfig("127.0.0.1:6000", "node2", "node2:8088")
	cfg.TLS = tlsconfig.Material{CAFile: "/nonexistent/ca.pem"}

	if _, err := NewJoiner(cfg); err == nil {
		t.Fatal("expected an error: retrying cannot fix a bad certificate path, and " +
			"joining over plaintext instead would leak the cluster-admin token")
	}
}

func TestNewJoinerDefaultsToHTTP(t *testing.T) {
	// The zero-config demo must keep working: no TLS material means plain HTTP.
	joiner, err := NewJoiner(DefaultJoinConfig("127.0.0.1:6000", "node2", "node2:8088"))
	if err != nil {
		t.Fatalf("NewJoiner: %v", err)
	}
	if joiner.scheme != "http" {
		t.Errorf("scheme = %q, want http when no TLS is configured", joiner.scheme)
	}
}

// hostPort strips the scheme from an httptest URL, since JoinConfig holds a bare
// host:port and derives the scheme itself.
func hostPort(t *testing.T, rawURL string) string {
	t.Helper()
	for _, prefix := range []string{"https://", "http://"} {
		if len(rawURL) > len(prefix) && rawURL[:len(prefix)] == prefix {
			return rawURL[len(prefix):]
		}
	}
	t.Fatalf("unexpected test server URL %q", rawURL)
	return ""
}
