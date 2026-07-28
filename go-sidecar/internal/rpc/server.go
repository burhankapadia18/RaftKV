// Package rpc provides the gRPC server for the Raft sidecar.
package rpc

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"time"

	"github.com/hashicorp/raft"
	"google.golang.org/grpc"

	pb "my-raft-sidecar/pb"
)

// NotLeaderPrefix tags a ProposeResponse.error that failed purely because this
// node is not the leader. Everything after the colon is the current leader's
// Raft address, which is empty while no leader is known.
//
// This is a wire contract, not a log message: the C++ HTTP layer keys its 503
// response (and, from Phase 4 on, request forwarding) off this exact prefix.
// Any other failure must NOT carry it.
const NotLeaderPrefix = "not_leader:"

// proposeTimeout bounds how long a proposal may wait to be committed and
// applied before Raft gives up on it.
//
// CAREFUL — this bounds LESS than the name suggests. In hashicorp/raft v1.7.3,
// Raft.Apply's timeout only bounds enqueueing onto applyCh (yielding
// ErrEnqueueTimeout); see ApplyLog in raft/api.go. Once the entry is queued,
// ApplyFuture.Error() blocks until it commits AND the local FSM applies it,
// with no deadline. CppFSM.Apply in turn calls the C++ state machine with
// context.Background(), also with no deadline.
//
// So a wedged C++ state machine still blocks past this value, and the C++
// client's 5s gRPC deadline will fire first and surface a bare
// DEADLINE_EXCEEDED. Keeping this below that 5s only helps the enqueue-backlog
// case, which is the one it actually covers.
//
// Bounding the rest is deliberately NOT done here: timing out an apply locally
// and moving on would skip an entry other replicas applied, which is real
// divergence. If a bound is wanted it belongs on the future.Error() wait in
// raftnode.Node.Apply, and it has to fail the node rather than continue.
const proposeTimeout = 4 * time.Second

// RaftProposer is the consumer-side view of the Raft node that the Propose
// handler needs. Declaring it here (rather than depending on *raftnode.Node)
// keeps this package testable with a fake and free of any dependency on
// package raftnode.
//
// *raftnode.Node satisfies this interface; the production guarantee is the
// call site in cmd/sidecar/main.go. The
// `var _ RaftProposer = (*raftnode.Node)(nil)` assertion deliberately lives in
// this package's test file rather than in package raftnode, since putting it
// there would make raftnode import rpc and invert the dependency direction the
// interface exists to break — same reasoning as management.RaftControl.
type RaftProposer interface {
	// Apply replicates data through Raft and returns the value the FSM
	// produced for the resulting entry alongside any Raft-level error.
	Apply(data []byte, timeout time.Duration) (interface{}, error)
	// LeaderAddr returns the current leader's Raft address, or "" if unknown.
	LeaderAddr() string
}

// ProposeForwarder relays a proposal to the leader on a follower's behalf.
// *Forwarder satisfies it; kept as an interface so the handler can be tested
// without a real peer.
type ProposeForwarder interface {
	ForwardPropose(ctx context.Context, raftAddr string, cmd *pb.Command) (*pb.ProposeResponse, error)
}

// Server represents the gRPC server for Raft operations.
type Server struct {
	pb.UnimplementedRaftNodeServer
	node       RaftProposer
	forwarder  ProposeForwarder
	grpcServer *grpc.Server
	listener   net.Listener
}

// NewServer creates a new gRPC server for the Raft node.
//
// forwarder may be nil, which disables forwarding and restores the Phase 1
// behavior of refusing a write on a follower. That is what the older tests
// expect, and it keeps the failure mode explicit rather than depending on a
// half-configured forwarder.
func NewServer(node RaftProposer, forwarder ProposeForwarder) *Server {
	return &Server{
		node:       node,
		forwarder:  forwarder,
		grpcServer: grpc.NewServer(),
	}
}

// Propose handles client proposals to the Raft cluster.
//
// Failures are reported in the response body with a nil gRPC error: the C++
// client checks reply.success() and reads reply.error(), so turning these into
// gRPC status errors would break that contract.
//
// There are three distinct failure shapes and they must stay distinguishable:
//   - not the leader: nothing was written anywhere, and the caller can retry
//     against the address carried after NotLeaderPrefix;
//   - other Raft error: the entry never committed;
//   - FSM error: the entry DID commit and replicate, but this node's state
//     machine refused to apply it. That is not a "try again elsewhere"
//     situation and must not be dressed up as one.
func (s *Server) Propose(ctx context.Context, cmd *pb.Command) (*pb.ProposeResponse, error) {
	resp, err := s.node.Apply(cmd.GetData(), proposeTimeout)
	if err != nil {
		if errors.Is(err, raft.ErrNotLeader) {
			leader := s.node.LeaderAddr()

			// R4.1: forward once, then stop. `cmd.GetForwarded()` means a peer
			// already sent this here believing we lead — if we do not, our own
			// leader hint is no better than theirs, and forwarding again is how
			// two nodes with stale hints ping-pong a request between them. Refuse
			// instead, and let the original client see the truth.
			if s.forwarder != nil && !cmd.GetForwarded() && leader != "" {
				log.Printf("Propose: not the leader, forwarding to %s", leader)
				forwarded, ferr := s.forwarder.ForwardPropose(ctx, leader, cmd)
				if ferr != nil {
					// Forwarding failed as a transport matter. Report it as
					// itself, NOT as not_leader: the client should not be told
					// "retry at the leader" when the problem is that we could
					// not reach the leader.
					log.Printf("ERROR: forwarding to leader %s failed: %v", leader, ferr)
					return &pb.ProposeResponse{
						Success: false,
						Error:   fmt.Sprintf("could not reach leader %s: %v", leader, ferr),
					}, nil
				}
				return forwarded, nil
			}

			if cmd.GetForwarded() {
				log.Printf("Propose rejected: arrived forwarded but this node is "+
					"not the leader (leader=%q) — refusing to forward again", leader)
			} else {
				log.Printf("Propose rejected: not the leader (leader=%q)", leader)
			}
			return &pb.ProposeResponse{
				Success: false,
				Error:   NotLeaderPrefix + leader,
			}, nil
		}
		log.Printf("ERROR: raft apply failed: %v", err)
		return &pb.ProposeResponse{Success: false, Error: err.Error()}, nil
	}

	if applyErr, ok := resp.(error); ok && applyErr != nil {
		log.Printf("ERROR: entry committed but the state machine rejected it: %v",
			applyErr)
		return &pb.ProposeResponse{Success: false, Error: applyErr.Error()}, nil
	}

	return &pb.ProposeResponse{Success: true}, nil
}

// Start starts the gRPC server on the specified port.
func (s *Server) Start(port string) error {
	addr := ":" + port
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen on %s: %w", addr, err)
	}
	s.listener = lis

	pb.RegisterRaftNodeServer(s.grpcServer, s)

	log.Printf("gRPC server listening on %s", addr)
	return s.grpcServer.Serve(lis)
}

// Stop gracefully stops the gRPC server.
func (s *Server) Stop() {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
}
