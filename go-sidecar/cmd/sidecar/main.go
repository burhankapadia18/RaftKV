// Package main is the entry point for the Raft sidecar application.
package main

import (
	"context"
	"fmt"
	"log"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"

	"my-raft-sidecar/internal/backend"
	"my-raft-sidecar/internal/cluster"
	"my-raft-sidecar/internal/config"
	"my-raft-sidecar/internal/fsm"
	"my-raft-sidecar/internal/logging"
	"my-raft-sidecar/internal/management"
	"my-raft-sidecar/internal/metrics"
	"my-raft-sidecar/internal/peers"
	"my-raft-sidecar/internal/raftnode"
	"my-raft-sidecar/internal/rpc"
)

// mgmtForwardTimeout bounds a relayed /join or /remove. The original caller is
// blocked while we wait, so an unbounded relay would turn one slow peer into a
// stalled request on whichever node received it.
const mgmtForwardTimeout = 10 * time.Second

// leadershipTransferTimeout bounds the handoff on shutdown. Generous enough for a
// healthy cluster to complete one, short enough that a wedged transfer does not
// hold a container stop open until the orchestrator SIGKILLs it — which would
// throw away the graceful shutdown entirely.
const leadershipTransferTimeout = 5 * time.Second

// shutdownTimeout bounds draining the management server.
const shutdownTimeout = 5 * time.Second

func main() {
	// Parse configuration
	cfg := config.Parse()

	// R5.1: one root logger carrying node_id, then a component-scoped child per
	// package. With three nodes writing into a single `docker compose logs`
	// stream, a line that cannot be filtered by node and component is nearly
	// useless during an incident — which is the only time anyone reads logs.
	root := logging.New(cfg.NodeID, cfg.LogLevel)
	slog.SetDefault(root)
	fsm.SetLogger(logging.For(root, logging.ComponentFSM))
	rpc.SetLogger(logging.For(root, logging.ComponentRPC))
	management.SetLogger(logging.For(root, logging.ComponentMgmt))
	cluster.SetLogger(logging.For(root, logging.ComponentJoiner))
	backend.SetLogger(logging.For(root, logging.ComponentBackend))
	raftnode.SetLogger(logging.For(root, logging.ComponentRaft))

	mainLog := logging.For(root, logging.ComponentMain)
	mainLog.Info("starting sidecar", slog.String("config", cfg.String()))

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
			mainLog.Error(fmt.Sprintf("Warning: Bootstrap failed (may already be bootstrapped): %v", err))
		}
	}

	// peers.Resolver is the single place that knows a peer's RaftNode gRPC and
	// management API sit on the same host as its Raft transport, at different
	// ports. Both the write-forwarding path (R4.1) and the join/remove
	// forwarding path (R4.12) resolve through it.
	resolver := peers.New(cfg.PeerRPCPort, cfg.MgmtPort)

	// Start management server. The forwarder lets /join and /remove be sent to
	// any node: a follower relays them to the leader rather than failing the
	// leader-only Raft call where it landed.
	// The reader doubles as the readiness probe (R5.5) and as the linearizable
	// read path (R4.5) — same call, same connection, so /ready fails exactly when
	// a real read would.
	storeReader := backend.NewStoreReader(backendClient.StateMachineClient)

	// Raft gauges are registered here rather than in the metrics package because
	// they need the live node, which does not exist until now.
	prometheus.MustRegister(metrics.NewRaftCollector(node))

	mgmtServer := management.NewServer(node, cfg.MgmtPort, resolver,
		management.NewHTTPForwarder(mgmtForwardTimeout)).
		WithBackendProbe(storeReader).
		WithMetricsHandler(promhttp.Handler())
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

	// Start gRPC server. The forwarder is what lets a write land on any node
	// (R4.1): a follower relays the proposal to the leader instead of refusing it.
	forwarder := rpc.NewForwarder(resolver)
	defer forwarder.Close()

	// WithLocalReader is what makes linearizable reads (R4.5) available: after
	// the Barrier and the quorum check, the leader answers from its own C++
	// store through this.
	grpcServer := rpc.NewServer(node, forwarder).WithLocalReader(storeReader)

	// Graceful shutdown (R5.8). The ORDER matters and each step earns its place:
	//
	//  1. Hand leadership away first, while we can still serve. Skipping this
	//     costs the cluster a full election timeout on every planned restart, and
	//     every client write fails for that window — downtime nobody needed.
	//  2. Stop accepting new work (gRPC GracefulStop, management Shutdown) but
	//     let in-flight requests finish. Dropping them would fail writes that were
	//     already accepted.
	//  3. Shut raft down last. It is what the FSM and the RPC handlers talk to, so
	//     stopping it first would make the requests drained in step 2 fail anyway.
	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
		sig := <-sigCh
		mainLog.Info(fmt.Sprintf("Received %s, shutting down gracefully", sig))

		if node.IsLeader() {
			mainLog.Info("This node is the leader; transferring leadership before exit")
			// Bounded, and a failure is logged rather than fatal: there may be no
			// other voter to hand off to (a single-node cluster, or peers already
			// gone), and refusing to shut down over that would be worse than
			// taking the election.
			done := make(chan error, 1)
			go func() { done <- node.LeadershipTransfer() }()
			select {
			case err := <-done:
				if err != nil {
					mainLog.Error(fmt.Sprintf("Leadership transfer failed (continuing shutdown): %v", err))
				} else {
					mainLog.Info("Leadership transferred")
				}
			case <-time.After(leadershipTransferTimeout):
				mainLog.Info(fmt.Sprintf("Leadership transfer did not complete within %s; "+
					"continuing shutdown", leadershipTransferTimeout))
			}
		}

		ctx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
		defer cancel()
		if err := mgmtServer.Stop(ctx); err != nil {
			mainLog.Info(fmt.Sprintf("Management server shutdown: %v", err))
		}

		// GracefulStop, not Stop: in-flight proposals have already been accepted
		// and dropping them would fail writes a client believes are in progress.
		grpcServer.Stop()

		if err := node.Shutdown(); err != nil {
			mainLog.Info(fmt.Sprintf("Raft shutdown: %v", err))
		}
		mainLog.Info("Shutdown complete")
	}()

	// Log startup info
	mainLog.Info(fmt.Sprintf("Go Sidecar %s running (Bind: %s, Adv: %s). Mgmt: %s",
		cfg.NodeID,
		cfg.BindAddr(),
		cfg.AdvertiseAddr(),
		cfg.MgmtPort,
	))

	// Start serving (blocks until shutdown)
	if err := grpcServer.Start(cfg.SidecarPort); err != nil {
		log.Fatalf("gRPC server failed: %v", err)
	}
}
