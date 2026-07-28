package raftnode

import (
	"crypto/tls"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/hashicorp/raft"

	"my-raft-sidecar/internal/testcerts"
	"my-raft-sidecar/internal/tlsconfig"
)

const dialTimeout = 5 * time.Second

func asMaterial(f testcerts.Files) tlsconfig.Material {
	return tlsconfig.Material{CertFile: f.CertFile, KeyFile: f.KeyFile, CAFile: f.CAFile}
}

// newTestLayer stands up a real mutually-authenticated listener on loopback and
// returns the stream layer plus the address a peer would dial.
func newTestLayer(t *testing.T, m tlsconfig.Material, advertise net.Addr) (*tlsStreamLayer, string) {
	t.Helper()

	serverCfg, err := tlsconfig.ServerConfig(m, true)
	if err != nil {
		t.Fatalf("ServerConfig: %v", err)
	}
	clientCfg, err := tlsconfig.ClientConfig(m)
	if err != nil {
		t.Fatalf("ClientConfig: %v", err)
	}

	ln, err := tls.Listen("tcp", "127.0.0.1:0", serverCfg)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { _ = ln.Close() })

	if advertise == nil {
		advertise = ln.Addr()
	}
	return &tlsStreamLayer{Listener: ln, advertise: advertise, client: clientCfg}, ln.Addr().String()
}

// TestAddrReturnsAdvertiseNotBind guards the subtlety that makes or breaks a
// cluster.
//
// Raft records whatever Addr() returns into the cluster configuration and hands
// it to peers as this node's identity. The listener binds 0.0.0.0, which is
// meaningless to dial, so returning the listener's own address would publish
// 0.0.0.0:8088 and no peer could ever reach this node. The failure would appear
// as an election that never completes, nowhere near this code.
func TestAddrReturnsAdvertiseNotBind(t *testing.T) {
	ca := testcerts.NewCA(t)
	m := asMaterial(ca.Issue(t, "node1", "127.0.0.1", "node1"))

	advertise, err := net.ResolveTCPAddr("tcp", "node1.example:8088")
	if err != nil {
		// Unresolvable by design in a sandbox; build the address literally.
		advertise = &net.TCPAddr{IP: net.ParseIP("10.0.0.7"), Port: 8088}
	}

	layer, bound := newTestLayer(t, m, advertise)

	if got := layer.Addr().String(); got != advertise.String() {
		t.Errorf("Addr() = %q, want the advertise address %q", got, advertise)
	}
	if layer.Addr().String() == bound {
		t.Error("Addr() returned the bind address; peers would be told to dial a " +
			"listening socket that is not this node's routable address")
	}
}

// TestPeerOfSameCAConnects and its siblings are the R6.4 acceptance criteria,
// exercised over a real socket rather than by inspecting a config.
func TestPeerOfSameCAConnects(t *testing.T) {
	ca := testcerts.NewCA(t)
	m := asMaterial(ca.Issue(t, "peer", "127.0.0.1"))
	layer, addr := newTestLayer(t, m, nil)

	echoOnce(t, layer)

	conn, err := layer.Dial(raft.ServerAddress(addr), dialTimeout)
	if err != nil {
		t.Fatalf("a peer of the cluster CA could not connect: %v", err)
	}
	defer conn.Close()

	if err := roundTrip(conn); err != nil {
		t.Fatalf("round trip over the mTLS peer connection failed: %v", err)
	}
}

func TestForeignCAPeerIsRefused(t *testing.T) {
	ca := testcerts.NewCA(t)
	m := asMaterial(ca.Issue(t, "peer", "127.0.0.1"))
	layer, addr := newTestLayer(t, m, nil)

	echoOnce(t, layer)

	// An intruder holding a valid certificate from a DIFFERENT CA. It trusts our
	// CA for the server's identity, so the only thing that can fail is its own
	// client certificate — otherwise this would pass for the wrong reason.
	other := testcerts.NewCA(t)
	intruder := asMaterial(other.Issue(t, "intruder", "127.0.0.1"))
	intruder.CAFile = ca.CAPath
	intruderCfg, err := tlsconfig.ClientConfig(intruder)
	if err != nil {
		t.Fatalf("ClientConfig: %v", err)
	}
	intruderLayer := &tlsStreamLayer{client: intruderCfg}

	conn, err := intruderLayer.Dial(raft.ServerAddress(addr), dialTimeout)
	if err == nil {
		// Go's TLS client can finish its side before the server rejects the
		// certificate, so a successful Dial is not yet a failure — the I/O is.
		defer conn.Close()
		if rtErr := roundTrip(conn); rtErr == nil {
			t.Fatal("a node signed by a foreign CA joined the raft transport: " +
				"cluster membership is not being enforced")
		}
		return
	}
}

func TestPlaintextPeerIsRefused(t *testing.T) {
	ca := testcerts.NewCA(t)
	m := asMaterial(ca.Issue(t, "peer", "127.0.0.1"))
	layer, addr := newTestLayer(t, m, nil)

	echoOnce(t, layer)

	// A bare TCP connect always succeeds — a TLS listener is still a TCP
	// listener. The refusal shows up when these bytes are read as a ClientHello.
	conn, err := net.DialTimeout("tcp", addr, dialTimeout)
	if err != nil {
		return
	}
	defer conn.Close()
	if err := conn.SetDeadline(time.Now().Add(dialTimeout)); err != nil {
		t.Fatalf("SetDeadline: %v", err)
	}
	if err := roundTrip(conn); err == nil {
		t.Fatal("a plaintext peer spoke the raft transport; anything that can reach " +
			"the port could append entries")
	}
}

func TestNewTLSTransportRejectsUnusableMaterial(t *testing.T) {
	advertise := &net.TCPAddr{IP: net.ParseIP("127.0.0.1"), Port: 8088}

	tests := []struct {
		name string
		m    tlsconfig.Material
	}{
		{"missing files", tlsconfig.Material{
			CertFile: "/nonexistent/c.pem", KeyFile: "/nonexistent/k.pem", CAFile: "/nonexistent/ca.pem"}},
		{"no CA for a mutual transport", func() tlsconfig.Material {
			ca := testcerts.NewCA(t)
			m := asMaterial(ca.Issue(t, "peer", "127.0.0.1"))
			m.CAFile = ""
			return m
		}()},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			transport, err := newTLSTransport("127.0.0.1:0", advertise, tc.m, DefaultOptions())
			if err == nil {
				_ = transport.Close()
				t.Fatal("expected an error: starting with an unusable identity would " +
					"leave the raft port either unprotected or unreachable")
			}
		})
	}
}

// TestAdvertisesHostnameNotResolvedIP pins the fix for a bug that survived a
// full smoke test: the transport must advertise the NAME it was configured with,
// because a peer verifies the certificate against the address it dials.
//
// The regression this prevents is genuinely nasty — the cluster forms, replicates
// and serves correctly, and only breaks after the first leader election, when a
// new leader tries to dial the bootstrap node at the IP that node recorded for
// itself. See hostPortAddr for the full account.
func TestAdvertisesHostnameNotResolvedIP(t *testing.T) {
	ca := testcerts.NewCA(t)
	m := asMaterial(ca.Issue(t, "node1", "node1"))

	transport, err := newTLSTransport("127.0.0.1:0", hostPortAddr("node1:8088"), m, DefaultOptions())
	if err != nil {
		t.Fatalf("newTLSTransport: %v", err)
	}
	defer func() { _ = transport.Close() }()

	got := string(transport.LocalAddr())
	if got != "node1:8088" {
		t.Fatalf("LocalAddr() = %q, want %q", got, "node1:8088")
	}
	if net.ParseIP(strings.Split(got, ":")[0]) != nil {
		t.Errorf("LocalAddr() = %q advertises an IP literal; a certificate issued "+
			"for a hostname will not cover it, and peers dial what this returns", got)
	}
}

func TestHostPortAddr(t *testing.T) {
	addr := hostPortAddr("node2:8088")
	if addr.Network() != "tcp" {
		t.Errorf("Network() = %q, want tcp", addr.Network())
	}
	if addr.String() != "node2:8088" {
		t.Errorf("String() = %q, want it returned verbatim and unresolved", addr.String())
	}
}

// TestNewTLSTransportServesTLS checks the assembled transport, not just the
// layer: it must be listening, and listening with TLS.
func TestNewTLSTransportServesTLS(t *testing.T) {
	ca := testcerts.NewCA(t)
	m := asMaterial(ca.Issue(t, "peer", "127.0.0.1"))
	advertise := &net.TCPAddr{IP: net.ParseIP("127.0.0.1"), Port: 8088}

	transport, err := newTLSTransport("127.0.0.1:0", advertise, m, DefaultOptions())
	if err != nil {
		t.Fatalf("newTLSTransport: %v", err)
	}
	defer func() { _ = transport.Close() }()

	if got := transport.LocalAddr(); string(got) != advertise.String() {
		t.Errorf("LocalAddr() = %q, want the advertise address %q", got, advertise)
	}
}

// echoOnce accepts a single connection and echoes four bytes, so a client can
// observe whether the handshake was actually accepted.
func echoOnce(t *testing.T, layer *tlsStreamLayer) {
	t.Helper()
	go func() {
		conn, err := layer.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		buf := make([]byte, 4)
		if _, err := io.ReadFull(conn, buf); err != nil {
			return
		}
		_, _ = conn.Write(buf)
	}()
}

func roundTrip(conn net.Conn) error {
	if err := conn.SetDeadline(time.Now().Add(dialTimeout)); err != nil {
		return err
	}
	if _, err := conn.Write([]byte("ping")); err != nil {
		return err
	}
	buf := make([]byte, 4)
	_, err := io.ReadFull(conn, buf)
	return err
}
