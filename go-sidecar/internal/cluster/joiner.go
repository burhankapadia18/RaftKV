// Package cluster provides cluster management utilities.
package cluster

import (
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"time"

	"my-raft-sidecar/internal/tlsconfig"
)

// JoinConfig holds configuration for joining a cluster.

// logger is this package's structured logger (R5.1). Package-level and settable
// rather than threaded through every constructor: the alternative was changing
// the signature of every New* in the codebase for a cross-cutting concern, and
// these are libraries with one instance per process.
//
// Defaults to DISCARDING rather than to os.Stdout. A package used without
// SetLogger — which is every unit test — should be silent, not spray JSON through
// the test output. main.go is the only caller of SetLogger.
var logger = slog.New(slog.NewTextHandler(io.Discard, nil))

// SetLogger installs the process logger for this package.
func SetLogger(l *slog.Logger) {
	if l != nil {
		logger = l
	}
}

type JoinConfig struct {
	// AuthToken is the cluster-admin bearer token the leader requires (R6.1/R6.2).
	// Empty means none is sent, which the leader will refuse unless it too is
	// unconfigured.
	AuthToken string

	// TLS is the material used to dial the leader's management API over HTTPS
	// (R6.3). Only CAFile is strictly needed — that listener does not ask for a
	// client certificate — but a full Material is accepted so a caller can pass
	// the same value it gives every other TLS surface.
	//
	// Configured() on the CERT is what flips the scheme, matching how the sidecar
	// decides everywhere else: a cluster is configured uniformly, so this node
	// having a management certificate means its peers do too.
	TLS tlsconfig.Material

	LeaderMgmtAddr string
	NodeID         string
	RaftAddr       string
	MaxRetries     int
	RetryInterval  time.Duration
}

// DefaultJoinConfig returns default join configuration.
func DefaultJoinConfig(leaderAddr, nodeID, raftAddr string) *JoinConfig {
	return &JoinConfig{
		LeaderMgmtAddr: leaderAddr,
		NodeID:         nodeID,
		RaftAddr:       raftAddr,
		MaxRetries:     20,
		RetryInterval:  2 * time.Second,
	}
}

// Joiner handles the process of joining an existing cluster.
type Joiner struct {
	config *JoinConfig
	client *http.Client
	scheme string
}

// NewJoiner creates a new Joiner with the given configuration.
//
// Returns an error only for unusable TLS material. Joining is otherwise
// all-retries-and-no-failures by design, but a bad certificate path is not
// something retrying can fix, and continuing over plaintext would put the
// cluster-admin token on the wire in clear.
func NewJoiner(config *JoinConfig) (*Joiner, error) {
	j := &Joiner{
		config: config,
		client: &http.Client{
			Timeout: 10 * time.Second,
		},
		scheme: "http",
	}

	if config.TLS.Configured() || config.TLS.CAFile != "" {
		cfg, err := tlsconfig.ClientConfig(config.TLS)
		if err != nil {
			return nil, fmt.Errorf("join client TLS: %w", err)
		}
		j.scheme = "https"
		j.client.Transport = &http.Transport{TLSClientConfig: cfg}
	}

	return j, nil
}

// Join attempts to join the cluster, retrying on failure.
// Returns an error if all attempts fail.
func (j *Joiner) Join() error {
	url := fmt.Sprintf(
		"%s://%s/join?peerID=%s&peerAddress=%s",
		j.scheme,
		j.config.LeaderMgmtAddr,
		j.config.NodeID,
		j.config.RaftAddr,
	)

	var lastErr error
	for i := 0; i < j.config.MaxRetries; i++ {
		// Wait before retrying (but not on first attempt)
		if i > 0 {
			time.Sleep(j.config.RetryInterval)
		}

		logger.Info(fmt.Sprintf("Attempting to join cluster via %s (attempt %d/%d)...",
			url, i+1, j.config.MaxRetries))

		if err := j.attemptJoin(url); err != nil {
			lastErr = err
			logger.Error(fmt.Sprintf("Join attempt %d failed: %v", i+1, err))
			continue
		}

		logger.Info("Successfully joined the cluster!")
		return nil
	}

	return fmt.Errorf("failed to join cluster after %d attempts: %w",
		j.config.MaxRetries, lastErr)
}

// JoinAsync attempts to join the cluster in a goroutine.
// Logs a critical error if joining fails.
func (j *Joiner) JoinAsync() {
	go func() {
		if err := j.Join(); err != nil {
			logger.Error(fmt.Sprintf("CRITICAL: %v", err))
		}
	}()
}

// attemptJoin makes a single attempt to join the cluster.
func (j *Joiner) attemptJoin(url string) error {
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return fmt.Errorf("building join request: %w", err)
	}
	if j.config.AuthToken != "" {
		req.Header.Set("Authorization", "Bearer "+j.config.AuthToken)
	}

	resp, err := j.client.Do(req)
	if err != nil {
		return fmt.Errorf("connection failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusOK {
		return nil
	}

	body, _ := io.ReadAll(resp.Body)
	return fmt.Errorf("server returned status %d: %s", resp.StatusCode, body)
}
