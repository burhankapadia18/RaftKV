// Package backend provides client connectivity to the C++ backend.
package backend

import (
	"fmt"
	"log"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	pb "my-raft-sidecar/pb"
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
		MaxRetries: 15,
		RetryDelay: 1 * time.Second,
	}
}

// Client represents a connection to the C++ backend.
type Client struct {
	conn               *grpc.ClientConn
	StateMachineClient pb.StateMachineClient
}

// Connect establishes a connection to the C++ backend with retries.
func Connect(cfg *ConnectionConfig) (*Client, error) {
	var conn *grpc.ClientConn
	var err error

	for i := 0; i < cfg.MaxRetries; i++ {
		conn, err = grpc.Dial(
			cfg.Address,
			grpc.WithTransportCredentials(insecure.NewCredentials()),
		)
		if err == nil {
			log.Printf("Connected to C++ backend at %s", cfg.Address)
			return &Client{
				conn:               conn,
				StateMachineClient: pb.NewStateMachineClient(conn),
			}, nil
		}

		log.Printf("Waiting for C++ backend at %s (attempt %d/%d)",
			cfg.Address, i+1, cfg.MaxRetries)
		time.Sleep(cfg.RetryDelay)
	}

	return nil, fmt.Errorf("failed to connect to C++ backend at %s after %d attempts: %w",
		cfg.Address, cfg.MaxRetries, err)
}

// Close closes the connection to the backend.
func (c *Client) Close() error {
	if c.conn != nil {
		return c.conn.Close()
	}
	return nil
}
