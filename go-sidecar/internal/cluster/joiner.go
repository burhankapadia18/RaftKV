// Package cluster provides cluster management utilities.
package cluster

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"time"
)

// JoinConfig holds configuration for joining a cluster.
type JoinConfig struct {
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
}

// NewJoiner creates a new Joiner with the given configuration.
func NewJoiner(config *JoinConfig) *Joiner {
	return &Joiner{
		config: config,
		client: &http.Client{
			Timeout: 10 * time.Second,
		},
	}
}

// Join attempts to join the cluster, retrying on failure.
// Returns an error if all attempts fail.
func (j *Joiner) Join() error {
	url := fmt.Sprintf(
		"http://%s/join?peerID=%s&peerAddress=%s",
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

		log.Printf("Attempting to join cluster via %s (attempt %d/%d)...",
			url, i+1, j.config.MaxRetries)

		if err := j.attemptJoin(url); err != nil {
			lastErr = err
			log.Printf("Join attempt %d failed: %v", i+1, err)
			continue
		}

		log.Println("Successfully joined the cluster!")
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
			log.Printf("CRITICAL: %v", err)
		}
	}()
}

// attemptJoin makes a single attempt to join the cluster.
func (j *Joiner) attemptJoin(url string) error {
	resp, err := j.client.Get(url)
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
