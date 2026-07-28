// Package testcerts mints throwaway X.509 identities for tests.
//
// It is a normal (non _test.go) package because three packages need it —
// tlsconfig, cluster and management all have to stand up a real TLS listener —
// and Go has no way to share a test helper across packages otherwise. It lives
// under internal/ and is imported by nothing outside a _test.go file; `go build
// ./cmd/...` never pulls it in.
//
// It deliberately does NOT import internal/tlsconfig, even though Files mirrors
// tlsconfig.Material: tlsconfig's own tests exercise unexported functions, so
// they must stay in package tlsconfig, and that makes any import from here back
// to there an import cycle. Callers convert the three paths themselves.
//
// Generated in-process rather than checked in as fixture files: a committed test
// certificate has an expiry date, and the test starts failing on that date for
// reasons that have nothing to do with the code.
package testcerts

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// validity is deliberately short. These certificates exist for the duration of
// one test run; a long window would only hide a clock problem.
const validity = 24 * time.Hour

// CA is a throwaway certificate authority.
type CA struct {
	cert *x509.Certificate
	key  *ecdsa.PrivateKey

	// CAPath is the PEM file holding the CA certificate, for callers that need
	// to point a config flag at it.
	CAPath string

	dir string
}

// NewCA returns a self-signed CA whose files live in a per-test temp directory.
func NewCA(t *testing.T) *CA {
	t.Helper()

	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generating CA key: %v", err)
	}

	tmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "raftkv-test-ca"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(validity),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
		BasicConstraintsValid: true,
		IsCA:                  true,
	}

	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("self-signing CA: %v", err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parsing CA: %v", err)
	}

	dir := t.TempDir()
	caPath := filepath.Join(dir, "ca.pem")
	WritePEM(t, caPath, "CERTIFICATE", der)

	return &CA{cert: cert, key: key, CAPath: caPath, dir: dir}
}

// Files names one identity on disk. Field-for-field the same shape as
// tlsconfig.Material; see the package comment for why it is not that type.
type Files struct {
	CertFile string
	KeyFile  string
	CAFile   string
}

// Issue mints a leaf valid for the given hosts (DNS names or IP literals) and
// returns its paths with this CA attached.
//
// The leaf carries both server and client extended key usages, because a raft
// peer is a server for inbound append-entries and a client for outbound ones —
// the same identity on both sides of the connection.
func (ca *CA) Issue(t *testing.T, name string, hosts ...string) Files {
	t.Helper()

	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generating key for %s: %v", name, err)
	}

	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(time.Now().UnixNano()),
		Subject:      pkix.Name{CommonName: name},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(validity),
		KeyUsage:     x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage: []x509.ExtKeyUsage{
			x509.ExtKeyUsageServerAuth,
			x509.ExtKeyUsageClientAuth,
		},
	}
	for _, h := range hosts {
		if ip := net.ParseIP(h); ip != nil {
			tmpl.IPAddresses = append(tmpl.IPAddresses, ip)
			continue
		}
		tmpl.DNSNames = append(tmpl.DNSNames, h)
	}

	der, err := x509.CreateCertificate(rand.Reader, tmpl, ca.cert, &key.PublicKey, ca.key)
	if err != nil {
		t.Fatalf("signing %s: %v", name, err)
	}

	certPath := filepath.Join(ca.dir, name+".pem")
	keyPath := filepath.Join(ca.dir, name+"-key.pem")
	WritePEM(t, certPath, "CERTIFICATE", der)

	keyDER, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		t.Fatalf("marshalling key for %s: %v", name, err)
	}
	WritePEM(t, keyPath, "EC PRIVATE KEY", keyDER)

	return Files{CertFile: certPath, KeyFile: keyPath, CAFile: ca.CAPath}
}

// WritePEM writes a DER blob as a PEM file. Exported so a test can build a
// deliberately malformed file to check the error paths.
func WritePEM(t *testing.T, path, blockType string, der []byte) {
	t.Helper()
	buf := pem.EncodeToMemory(&pem.Block{Type: blockType, Bytes: der})
	if err := os.WriteFile(path, buf, 0600); err != nil {
		t.Fatalf("writing %s: %v", path, err)
	}
}
