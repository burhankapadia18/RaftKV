// Package tlsconfig loads the X.509 material for every TLS surface the sidecar
// exposes or dials (R6.3, R6.4).
//
// One package rather than a *tls.Config built at each call site, because the
// dangerous parts of a TLS config are the ones that are easy to leave out. A
// server that loads a cert and a key but forgets ClientCAs still completes a
// handshake with any client at all — it looks like mutual TLS, logs like mutual
// TLS, and authenticates nobody. Centralizing it means that mistake can only be
// made once, and it is made here under test.
package tlsconfig

import (
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"net"
	"os"
)

// The TLS floor is tls.VersionTLS12, written out at each use rather than hoisted
// into a constant.
//
// That is not a style preference. gosec's G402 recognizes the literal selector
// and nothing else — behind a named constant it reports "TLS MinVersion too low"
// on correct code, and the only ways out are an inline suppression or this. A
// suppression would switch the check off permanently; spelling out the value
// keeps it on, so a future edit that actually lowers the floor gets caught.
//
// TLS 1.2 rather than 1.3: peers and the management API only ever talk to other
// instances of this binary, so 1.3 would be reachable — but Go's default floor is
// already 1.2, and pinning it explicitly is what makes the choice visible if a
// component is later swapped for one that would negotiate lower.

// Material names the files that make up one identity. All three fields are
// independent because the surfaces need different subsets: the management
// listener needs a cert and key with no CA (browsers and curl bring their own
// trust store), while a raft peer needs all three because it both proves and
// verifies identity.
type Material struct {
	CertFile string
	KeyFile  string
	CAFile   string
}

// Configured reports whether a cert/key pair was supplied at all.
//
// The distinction the callers need is "TLS off" versus "TLS misconfigured", and
// those must not be conflated: silently running plaintext because a path was
// wrong is precisely the failure mode this phase exists to remove.
func (m Material) Configured() bool {
	return m.CertFile != "" || m.KeyFile != ""
}

// Validate rejects a half-specified pair.
//
// Called from config parsing so a typo fails at startup with a readable message,
// rather than at the first peer connection with a handshake error.
func (m Material) Validate(surface string) error {
	if (m.CertFile == "") != (m.KeyFile == "") {
		return fmt.Errorf("%s TLS needs both a certificate and a key (cert=%q key=%q)",
			surface, m.CertFile, m.KeyFile)
	}
	if m.CAFile != "" && !m.Configured() {
		return fmt.Errorf("%s TLS CA given without a certificate and key: "+
			"a CA alone verifies peers but leaves this node with no identity to present", surface)
	}
	return nil
}

// ServerConfig builds the listener-side config.
//
// requireClientCert turns this into mutual TLS. It is a separate argument rather
// than being inferred from CAFile because those two decisions are genuinely
// different: the management listener may want to verify nothing about its callers
// (it authenticates them with a bearer token instead), while a raft peer must
// refuse anyone who cannot prove cluster membership. Inferring it would make
// "I supplied a CA" silently mean "I demand client certs".
func ServerConfig(m Material, requireClientCert bool) (*tls.Config, error) {
	cert, err := loadPair(m)
	if err != nil {
		return nil, err
	}

	cfg := &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	}

	if !requireClientCert {
		return cfg, nil
	}

	pool, err := loadCA(m.CAFile)
	if err != nil {
		return nil, err
	}
	cfg.ClientCAs = pool
	// RequireAndVerifyClientCert, not VerifyClientCertIfGiven: the weaker mode
	// accepts a client that presents no certificate at all, which is every
	// attacker, and only rejects one that presents a bad certificate — which is
	// nobody.
	cfg.ClientAuth = tls.RequireAndVerifyClientCert
	return cfg, nil
}

// ClientConfig builds the dialer-side config.
//
// The returned config has no ServerName: the caller knows which host it is
// dialing and must set it (see WithServerName), because a config shared across
// peers cannot carry one peer's name.
func ClientConfig(m Material) (*tls.Config, error) {
	cfg := &tls.Config{MinVersion: tls.VersionTLS12}

	if m.Configured() {
		cert, err := loadPair(m)
		if err != nil {
			return nil, err
		}
		cfg.Certificates = []tls.Certificate{cert}
	}

	if m.CAFile != "" {
		pool, err := loadCA(m.CAFile)
		if err != nil {
			return nil, err
		}
		// RootCAs replaces the system pool rather than adding to it. That is the
		// intent: a cluster peer signed by a public CA is not a cluster peer.
		cfg.RootCAs = pool
	}

	return cfg, nil
}

// WithServerName clones cfg for a single destination, deriving the name to verify
// from a host:port address.
//
// This is the step that makes verification mean anything. A *tls.Config with an
// empty ServerName used through crypto/tls.Client verifies the chain but not that
// the certificate belongs to the host being dialed, so any cluster member's
// certificate would authenticate any other member — and, worse, so would a
// decommissioned one. The clone matters too: mutating the shared config per dial
// is a data race, and raft dials peers concurrently.
func WithServerName(cfg *tls.Config, address string) *tls.Config {
	out := cfg.Clone()
	host, _, err := net.SplitHostPort(address)
	if err != nil {
		// No port to strip; the whole string is the host. Callers pass raft
		// addresses, which always carry a port, so this is the defensive branch.
		host = address
	}
	out.ServerName = host
	return out
}

func loadPair(m Material) (tls.Certificate, error) {
	cert, err := tls.LoadX509KeyPair(m.CertFile, m.KeyFile)
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("loading TLS key pair (%s, %s): %w",
			m.CertFile, m.KeyFile, err)
	}
	return cert, nil
}

func loadCA(path string) (*x509.CertPool, error) {
	if path == "" {
		return nil, fmt.Errorf("a CA file is required to verify peer certificates")
	}
	pem, err := os.ReadFile(path) // #nosec G304 -- operator-supplied path from a flag; reading it IS the feature
	if err != nil {
		return nil, fmt.Errorf("reading CA file %s: %w", path, err)
	}
	pool := x509.NewCertPool()
	// AppendCertsFromPEM returns false for "nothing parsed", which is what a
	// path pointing at the wrong file looks like. Left unchecked, the node would
	// start with an empty trust pool and reject every peer with a handshake
	// error that says nothing about the real cause.
	if !pool.AppendCertsFromPEM(pem) {
		return nil, fmt.Errorf("CA file %s contained no PEM certificates", path)
	}
	return pool, nil
}
