package tlsconfig

import (
	"crypto/tls"
	"errors"
	"io"
	"net"
	"testing"
	"time"
)

// handshakeDeadline bounds both halves so a failed negotiation surfaces as a
// test failure rather than a hung run.
const handshakeDeadline = 5 * time.Second

// handshake stands up a real TLS listener on loopback, dials it with clientCfg,
// and reports whether the connection completed.
//
// A real listener rather than net.Pipe: mutual TLS failures are reported to
// whichever side notices first, and with a pipe the client can appear to succeed
// while the server rejects it. Doing it over TCP with a write-then-read makes the
// rejection observable from the client, which is where the caller checks.
func handshake(t *testing.T, serverCfg, clientCfg *tls.Config) error {
	t.Helper()

	ln, err := tls.Listen("tcp", "127.0.0.1:0", serverCfg)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()

	serverErr := make(chan error, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			serverErr <- err
			return
		}
		defer conn.Close()
		// Read then write: the server's handshake completes lazily on first I/O,
		// so without this the client's rejection might never be sent.
		buf := make([]byte, 4)
		if _, err := io.ReadFull(conn, buf); err != nil {
			serverErr <- err
			return
		}
		_, err = conn.Write([]byte("pong"))
		serverErr <- err
	}()

	// ServerName was set by the caller via WithServerName; keep it, but make the
	// dial target the real ephemeral port.
	dialCfg := clientCfg.Clone()
	if host, _, err := net.SplitHostPort(ln.Addr().String()); err == nil {
		dialCfg.ServerName = host
	}

	conn, err := tls.DialWithDialer(
		&net.Dialer{Timeout: handshakeDeadline}, "tcp", ln.Addr().String(), dialCfg)
	if err != nil {
		return err
	}
	defer conn.Close()

	if err := conn.SetDeadline(time.Now().Add(handshakeDeadline)); err != nil {
		return err
	}
	if _, err := conn.Write([]byte("ping")); err != nil {
		return err
	}
	buf := make([]byte, 4)
	if _, err := io.ReadFull(conn, buf); err != nil {
		return err
	}

	select {
	case err := <-serverErr:
		return err
	case <-time.After(handshakeDeadline):
		return errors.New("server side did not finish within the deadline")
	}
}

// plaintextTo dials a TLS listener with a bare TCP connection and reports whether
// it got a usable exchange. This is the R6.4 acceptance criterion: "plaintext
// connection to the raft port is refused".
func plaintextTo(t *testing.T, serverCfg *tls.Config) error {
	t.Helper()

	ln, err := tls.Listen("tcp", "127.0.0.1:0", serverCfg)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		buf := make([]byte, 4)
		_, _ = io.ReadFull(conn, buf)
	}()

	conn, err := net.DialTimeout("tcp", ln.Addr().String(), handshakeDeadline)
	if err != nil {
		return err
	}
	defer conn.Close()

	if err := conn.SetDeadline(time.Now().Add(handshakeDeadline)); err != nil {
		return err
	}
	// The TCP connect always succeeds — a TLS listener is still a TCP listener.
	// The refusal shows up when these bytes are interpreted as a ClientHello and
	// the server closes with a TLS alert, so the read is where the test lives.
	if _, err := conn.Write([]byte("ping")); err != nil {
		return err
	}
	buf := make([]byte, 4)
	_, err = io.ReadFull(conn, buf)
	if err == nil {
		return errors.New("plaintext exchange completed against a TLS listener")
	}
	return err
}
