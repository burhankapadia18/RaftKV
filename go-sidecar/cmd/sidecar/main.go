// Package main is the entry point for the Raft sidecar application.
package main

import (
	"log"
	"os"
	"os/signal"
	"syscall"

	"my-raft-sidecar/internal/backend"
	"my-raft-sidecar/internal/cluster"
	"my-raft-sidecar/internal/config"
	"my-raft-sidecar/internal/fsm"
	"my-raft-sidecar/internal/management"
	"my-raft-sidecar/internal/raftnode"
	"my-raft-sidecar/internal/rpc"
)

func main() {
	// Parse configuration
	cfg := config.Parse()
	log.Printf("Starting sidecar with config: %s", cfg)

	// Connect to C++ backend
	backendClient, err := backend.Connect(backend.DefaultConnectionConfig(cfg.AppAddr))
	if err != nil {
		log.Fatalf("Failed to connect to backend: %v", err)
	}
	defer backendClient.Close()

	// Create FSM
	stateMachineClient := fsm.NewStateMachineClient(backendClient.StateMachineClient)
	raftFSM := fsm.NewCppFSM(stateMachineClient)

	// Create Raft node
	node, err := raftnode.New(cfg, raftFSM, nil)
	if err != nil {
		log.Fatalf("Failed to create Raft node: %v", err)
	}

	// Bootstrap if requested
	if cfg.Bootstrap {
		if err := node.Bootstrap(); err != nil {
			log.Printf("Warning: Bootstrap failed (may already be bootstrapped): %v", err)
		}
	}

	// Start management server
	mgmtServer := management.NewServer(node, cfg.MgmtPort)
	mgmtServer.Start()

	// Join cluster if requested
	if cfg.JoinAddr != "" {
		joiner := cluster.NewJoiner(cluster.DefaultJoinConfig(
			cfg.JoinAddr,
			cfg.NodeID,
			cfg.AdvertiseAddr(),
		))
		joiner.JoinAsync()
	}

	// Start gRPC server
	grpcServer := rpc.NewServer(node)

	// Setup graceful shutdown
	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
		<-sigCh

		log.Println("Shutting down...")
		grpcServer.Stop()
	}()

	// Log startup info
	log.Printf("Go Sidecar %s running (Bind: %s, Adv: %s). Mgmt: %s",
		cfg.NodeID,
		cfg.BindAddr(),
		cfg.AdvertiseAddr(),
		cfg.MgmtPort,
	)

	// Start serving (blocks until shutdown)
	if err := grpcServer.Start(cfg.SidecarPort); err != nil {
		log.Fatalf("gRPC server failed: %v", err)
	}
}
