package raftnode

import (
	"crypto/tls"
	"fmt"
	"net"
	"os"
	"time"

	"github.com/hashicorp/raft"

	"my-raft-sidecar/internal/tlsconfig"
)

// tlsStreamLayer carries the raft peer protocol over mutual TLS (R6.4).
//
// raft.NewTCPTransport is raft.NewNetworkTransport over an unencrypted
// tcpStreamLayer. Swapping in this layer changes nothing about the protocol
// above it: raft still writes framed RPCs into a net.Conn. What changes is who is
// allowed to open that conn. Without it, anything that can reach port 8088 can
// speak the raft protocol — append entries, request a vote, install a snapshot —
// which is total control of the store with no credential at all.
type tlsStreamLayer struct {
	net.Listener

	// advertise is what peers are told to dial, and it is NOT the listener's own
	// address. The listener binds 0.0.0.0, which is meaningless to a peer;
	// raft records whatever Addr() returns into the cluster configuration, so
	// returning the bind address here would publish 0.0.0.0:8088 as this node's
	// identity and no peer could ever reach it. raft's own tcpStreamLayer makes
	// exactly this substitution.
	advertise net.Addr

	// client is the shared dial-side config. Per-dial it is cloned with the
	// destination's ServerName; it is never mutated, because raft dials peers
	// from several goroutines at once.
	client *tls.Config
}

// Dial opens a verified connection to a peer.
func (s *tlsStreamLayer) Dial(address raft.ServerAddress, timeout time.Duration) (net.Conn, error) {
	dialer := &net.Dialer{Timeout: timeout}
	cfg := tlsconfig.WithServerName(s.client, string(address))

	conn, err := tls.DialWithDialer(dialer, "tcp", string(address), cfg)
	if err != nil {
		return nil, fmt.Errorf("TLS dial to peer %s: %w", address, err)
	}
	return conn, nil
}

// Addr returns the advertise address. See the field comment — this is load-bearing.
func (s *tlsStreamLayer) Addr() net.Addr {
	return s.advertise
}

// Ensure the layer satisfies raft's interface at compile time.
var _ raft.StreamLayer = (*tlsStreamLayer)(nil)

// hostPortAddr advertises a host:port string verbatim, without resolving it.
//
// This exists because of a bug that only appears under TLS, and only after the
// first leader election. createTransport resolves the advertise address to a
// *net.TCPAddr, which it must: raft.NewTCPTransport rejects an advertise address
// that is not a TCPAddr with a concrete IP (errNotAdvertisable). So on the
// plaintext path "node1:8088" becomes "172.18.0.2:8088", and that resolved
// address is what raft records as this node's identity and hands to peers.
//
// Under TLS that is fatal. A peer dialling 172.18.0.2:8088 verifies the
// certificate against the name it dialled, and a certificate issued for the host
// node1 does not cover the address the container happened to get from Docker:
//
//	x509: certificate is valid for 127.0.0.1, ::1, not 172.18.0.2
//
// It hides well. A leader dials its followers, and followers are registered by
// the hostname they sent in their own join request, so those connections verify
// fine — the cluster forms, replicates and passes a smoke test. The node
// registered by IP is the bootstrap node, which is the initial leader, and
// nothing dials it until it stops leading. The failure surfaces one election
// later: the new leader cannot reach the old one, ever, and no amount of
// restarting fixes it because the address is in the committed raft configuration.
//
// So the TLS path advertises the unresolved name. It can: NewNetworkTransport
// makes no demands on the address beyond String() being dialable, and the whole
// point of a name is that it survives a container getting a new IP. The
// asymmetry with the plaintext path is deliberate and forced by raft's own check.
type hostPortAddr string

func (a hostPortAddr) Network() string { return "tcp" }
func (a hostPortAddr) String() string  { return string(a) }

var _ net.Addr = hostPortAddr("")

// newTLSTransport builds a NetworkTransport whose peer connections are mutually
// authenticated against the cluster CA.
func newTLSTransport(
	bindAddr string,
	advertise net.Addr,
	material tlsconfig.Material,
	opts *Options,
) (*raft.NetworkTransport, error) {
	// requireClientCert is true and not configurable. A one-way-TLS raft port
	// would encrypt the traffic and still let any client append entries, which
	// is confidentiality without authentication — the wrong half of the problem.
	serverCfg, err := tlsconfig.ServerConfig(material, true)
	if err != nil {
		return nil, fmt.Errorf("raft peer TLS (server side): %w", err)
	}
	clientCfg, err := tlsconfig.ClientConfig(material)
	if err != nil {
		return nil, fmt.Errorf("raft peer TLS (client side): %w", err)
	}

	listener, err := tls.Listen("tcp", bindAddr, serverCfg)
	if err != nil {
		return nil, fmt.Errorf("listening for TLS raft peers on %s: %w", bindAddr, err)
	}

	stream := &tlsStreamLayer{
		Listener:  listener,
		advertise: advertise,
		client:    clientCfg,
	}

	// os.Stderr matches the plaintext path: raft's internal transport logging is
	// not routed through the structured logger, and diverging here would put half
	// the transport's output in a different format from the other half.
	return raft.NewNetworkTransport(stream, opts.MaxPool, opts.Timeout, os.Stderr), nil
}
