package management

import (
	"context"
	"crypto/tls"
	"io"
	"net/http"
	"testing"
	"time"

	"my-raft-sidecar/internal/testcerts"
	"my-raft-sidecar/internal/tlsconfig"
)

func asMaterial(f testcerts.Files) tlsconfig.Material {
	return tlsconfig.Material{CertFile: f.CertFile, KeyFile: f.KeyFile, CAFile: f.CAFile}
}

// TestStartServesHTTPS is the R6.3 acceptance check: the management listener
// serves TLS, an HTTPS client that trusts the cluster CA can reach it, and a
// plaintext client cannot.
//
// The plaintext half is the one that earns its keep. Without it a bug that
// silently fell back to HTTP would still pass the happy path — the HTTPS client
// would fail, yes, but only after the mistake had already shipped a listener
// carrying the cluster-admin token in clear.
func TestStartServesHTTPS(t *testing.T) {
	ca := testcerts.NewCA(t)
	serverMaterial := asMaterial(ca.Issue(t, "mgmt", "127.0.0.1", "localhost"))

	node := &fakeRaftControl{isLeader: true, leaderAddr: "node1:8088"}
	port := freePort(t)
	server := NewServer(node, port, nil, nil).
		WithAuthToken(testAuthToken).
		WithTLS(serverMaterial)

	server.Start()
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = server.Stop(ctx)
	})

	clientCfg, err := tlsconfig.ClientConfig(tlsconfig.Material{CAFile: ca.CAPath})
	if err != nil {
		t.Fatalf("ClientConfig: %v", err)
	}
	httpsClient := &http.Client{
		Timeout:   2 * time.Second,
		Transport: &http.Transport{TLSClientConfig: clientCfg},
	}

	baseURL := "https://127.0.0.1:" + port
	body := pollForBody(t, httpsClient, baseURL+"/health")
	if body != "OK" {
		t.Errorf("GET /health over HTTPS = %q, want %q", body, "OK")
	}

	t.Run("plaintext is not served", func(t *testing.T) {
		// NOT "the connection errors". Go's http.Server sniffs a plaintext request
		// arriving on a TLS listener and answers 400 with an explanatory line, so
		// the round trip completes — the first version of this test asserted err
		// != nil and failed for that reason. The property that matters is that the
		// request was never routed to a handler.
		plain := &http.Client{Timeout: 2 * time.Second}
		resp, err := plain.Get("http://127.0.0.1:" + port + "/health")
		if err != nil {
			return // Refused outright; also acceptable.
		}
		defer resp.Body.Close()

		body, readErr := io.ReadAll(resp.Body)
		if readErr != nil {
			t.Fatalf("reading plaintext response: %v", readErr)
		}
		if resp.StatusCode == http.StatusOK || string(body) == "OK" {
			t.Fatalf("plaintext GET was SERVED (status %d, body %q); the listener is "+
				"not actually requiring TLS", resp.StatusCode, body)
		}
		if resp.StatusCode != http.StatusBadRequest {
			t.Errorf("plaintext GET status = %d, want %d (Go's HTTP-to-HTTPS diagnostic)",
				resp.StatusCode, http.StatusBadRequest)
		}
	})

	t.Run("TLS 1.1 is refused", func(t *testing.T) {
		// The floor must be enforced by the SERVER. A client can always ask for
		// an old version; what matters is that this listener says no.
		old := &http.Client{
			Timeout: 2 * time.Second,
			Transport: &http.Transport{TLSClientConfig: &tls.Config{
				RootCAs:    clientCfg.RootCAs,
				MinVersion: tls.VersionTLS10,
				MaxVersion: tls.VersionTLS11,
			}},
		}
		resp, err := old.Get(baseURL + "/health")
		if err == nil {
			resp.Body.Close()
			t.Fatal("a TLS 1.1 client completed the handshake; the 1.2 floor is not enforced")
		}
	})
}

// TestForwarderRelaysOverHTTPS proves the relay follows the listener: a follower
// relaying a /join to an HTTPS leader must dial https and present the token.
//
// A relay that fell back to http would fail closed here rather than leaking, but
// only because the peer refuses plaintext — that is the peer protecting us, not
// us protecting the token. This test pins that we dial the right scheme.
func TestForwarderRelaysOverHTTPS(t *testing.T) {
	ca := testcerts.NewCA(t)
	leaderMaterial := asMaterial(ca.Issue(t, "leader", "127.0.0.1"))

	type received struct {
		forwarded string
		auth      string
		query     string
	}
	got := make(chan received, 1)

	mux := http.NewServeMux()
	mux.HandleFunc("/join", func(w http.ResponseWriter, r *http.Request) {
		got <- received{
			forwarded: r.Header.Get(ForwardedHeader),
			auth:      r.Header.Get(AuthHeader),
			query:     r.URL.RawQuery,
		}
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("Joined successfully"))
	})

	serverCfg, err := tlsconfig.ServerConfig(leaderMaterial, false)
	if err != nil {
		t.Fatalf("ServerConfig: %v", err)
	}
	port := freePort(t)
	srv := &http.Server{
		Addr:              "127.0.0.1:" + port,
		Handler:           mux,
		TLSConfig:         serverCfg,
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() { _ = srv.ListenAndServeTLS(leaderMaterial.CertFile, leaderMaterial.KeyFile) }()
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
	})

	forwarder, err := NewHTTPForwarder(5 * time.Second).
		WithAuthToken(testAuthToken).
		WithTLS(tlsconfig.Material{CAFile: ca.CAPath})
	if err != nil {
		t.Fatalf("WithTLS: %v", err)
	}

	// The listener starts in a goroutine, so retry rather than sleep.
	var result *ForwardResult
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		result, err = forwarder.Forward(context.Background(),
			"127.0.0.1:"+port, "/join", "peerID=node2&peerAddress=node2:8088")
		if err == nil {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err != nil {
		t.Fatalf("Forward over HTTPS: %v", err)
	}
	if result.Status != http.StatusOK {
		t.Errorf("relayed status = %d, want %d", result.Status, http.StatusOK)
	}

	select {
	case r := <-got:
		if r.forwarded != "1" {
			t.Errorf("%s = %q, want %q; without it two nodes would bounce a join forever",
				ForwardedHeader, r.forwarded, "1")
		}
		if r.auth != bearerPrefix+testAuthToken {
			t.Errorf("Authorization = %q, want the relaying node's own credential", r.auth)
		}
		if r.query != "peerID=node2&peerAddress=node2:8088" {
			t.Errorf("query = %q, want the original preserved verbatim", r.query)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("the leader never saw the relayed request")
	}
}

func TestWithTLSRejectsUnusableMaterial(t *testing.T) {
	if _, err := NewHTTPForwarder(time.Second).
		WithTLS(tlsconfig.Material{CAFile: "/nonexistent/ca.pem"}); err == nil {
		t.Fatal("expected an error: a relay that fell back to HTTP would put the " +
			"cluster-admin token on the wire in clear")
	}
}

// pollForBody waits for a listener started in a goroutine to accept, then returns
// the response body.
func pollForBody(t *testing.T, client *http.Client, url string) string {
	t.Helper()

	var lastErr error
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		resp, err := client.Get(url)
		if err != nil {
			lastErr = err
			time.Sleep(10 * time.Millisecond)
			continue
		}
		body, readErr := io.ReadAll(resp.Body)
		resp.Body.Close()
		if readErr != nil {
			t.Fatalf("reading %s: %v", url, readErr)
		}
		return string(body)
	}
	t.Fatalf("%s never became reachable: %v", url, lastErr)
	return ""
}
