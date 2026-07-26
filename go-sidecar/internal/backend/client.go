// Package backend provides client connectivity to the C++ backend.
package backend

import (
	"context"
	"fmt"
	"log"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/backoff"
	"google.golang.org/grpc/connectivity"
	"google.golang.org/grpc/credentials/insecure"

	pb "my-raft-sidecar/pb"
)

const (
	// defaultMaxRetries and defaultRetryDelay give the C++ engine a ~15s window
	// to start accepting gRPC connections before the sidecar gives up.
	defaultMaxRetries = 15
	defaultRetryDelay = 1 * time.Second

	// minConnectTimeout bounds a single TCP + HTTP/2 handshake attempt. It only
	// matters for a backend that accepts the connection but never completes the
	// handshake; a refused connection fails immediately regardless.
	minConnectTimeout = 5 * time.Second

	// connectSlack is added to the nominal MaxRetries * RetryDelay budget so the
	// overall deadline never cuts the last attempt short.
	connectSlack = 2 * time.Second
)

// ConnectionConfig holds configuration for connecting to the backend.
type ConnectionConfig struct {
	Address    string
	MaxRetries int
	RetryDelay time.Duration
}

// DefaultConnectionConfig returns default connection configuration.
func DefaultConnectionConfig(address string) *ConnectionConfig {
	return &ConnectionConfig{
		Address:    address,
		MaxRetries: defaultMaxRetries,
		RetryDelay: defaultRetryDelay,
	}
}

// Client represents a connection to the C++ backend.
type Client struct {
	conn               *grpc.ClientConn
	StateMachineClient pb.StateMachineClient
}

// Connect establishes a connection to the C++ backend and blocks until that
// connection is actually usable, retrying for up to MaxRetries * RetryDelay.
//
// grpc.NewClient only validates the target and hands back an IDLE connection;
// it never touches the network, so a nil error says nothing about the backend
// being up. The deprecated grpc.Dial behaved the same way, which is why the
// retry loop here used to be a no-op: the first "attempt" always succeeded and
// the sidecar happily carried on against a backend that was not listening.
// Readiness is therefore established explicitly, by driving the connection out
// of IDLE and watching its connectivity state until it reports READY.
func Connect(cfg *ConnectionConfig) (*Client, error) {
	// A non-positive delay would turn the gRPC reconnect backoff below into a
	// busy loop, so fall back to the default rather than trusting the caller.
	retryDelay := cfg.RetryDelay
	if retryDelay <= 0 {
		retryDelay = defaultRetryDelay
	}

	conn, err := grpc.NewClient(
		cfg.Address,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		// Align gRPC's own reconnect schedule with the configured retry cadence:
		// a constant RetryDelay between attempts instead of the default backoff
		// that grows to minutes.
		grpc.WithConnectParams(grpc.ConnectParams{
			Backoff: backoff.Config{
				BaseDelay:  retryDelay,
				Multiplier: 1.0,
				Jitter:     0.2,
				MaxDelay:   retryDelay,
			},
			MinConnectTimeout: minConnectTimeout,
		}),
	)
	if err != nil {
		return nil, fmt.Errorf("failed to create gRPC client for C++ backend at %s: %w",
			cfg.Address, err)
	}

	// Overall budget on top of the per-attempt windows, so a wedged backend that
	// neither refuses connections nor completes a handshake cannot hang startup.
	budget := time.Duration(cfg.MaxRetries)*retryDelay + connectSlack
	ctx, cancel := context.WithTimeout(context.Background(), budget)
	defer cancel()

	state := conn.GetState()
	attempts := 0
	for attempts < cfg.MaxRetries {
		attempts++

		state = waitForReady(ctx, conn, retryDelay)
		if state == connectivity.Ready {
			log.Printf("Connected to C++ backend at %s", cfg.Address)
			return &Client{
				conn:               conn,
				StateMachineClient: pb.NewStateMachineClient(conn),
			}, nil
		}
		if state == connectivity.Shutdown {
			break
		}

		log.Printf("Waiting for C++ backend at %s (attempt %d/%d, state %s)",
			cfg.Address, attempts, cfg.MaxRetries, state)

		if ctx.Err() != nil {
			// The overall budget is gone: the remaining attempt windows would
			// return instantly and report nothing new.
			break
		}
	}

	// Never hand back a half-open connection: the caller only gets an error, so
	// nobody would be left to Close this one.
	if cerr := conn.Close(); cerr != nil {
		log.Printf("Failed to close unready connection to %s: %v", cfg.Address, cerr)
	}

	return nil, fmt.Errorf("failed to connect to C++ backend at %s after %d attempts within %v (last state %s)",
		cfg.Address, attempts, budget, state)
}

// waitForReady nudges conn towards READY and waits up to timeout for it to get
// there, returning the last observed connectivity state. A CONNECTING or
// TRANSIENT_FAILURE connection is left alone: gRPC retries it on its own using
// the backoff configured in Connect.
func waitForReady(ctx context.Context, conn *grpc.ClientConn, timeout time.Duration) connectivity.State {
	attemptCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	for {
		state := conn.GetState()
		if state == connectivity.Ready || state == connectivity.Shutdown {
			return state
		}
		if state == connectivity.Idle {
			// grpc.NewClient never dials on its own; this is what starts (and,
			// after a lost connection, restarts) the connection attempts.
			conn.Connect()
		}
		if !conn.WaitForStateChange(attemptCtx, state) {
			return state
		}
	}
}

// Close closes the connection to the backend.
func (c *Client) Close() error {
	if c.conn != nil {
		return c.conn.Close()
	}
	return nil
}
