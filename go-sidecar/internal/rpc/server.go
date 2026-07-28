// Package rpc provides the gRPC server for the Raft sidecar.
package rpc

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"strings"
	"time"

	"github.com/hashicorp/raft"
	"google.golang.org/grpc"

	"my-raft-sidecar/internal/metrics"
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

// UnavailablePrefix tags a failure that is TRANSIENT and worth retrying: this
// node knows who the leader is but could not reach it, which is the normal
// state of the world for a second or two during a leader failover.
//
// Distinct from NotLeaderPrefix because the two call for different client
// behavior — not_leader carries an address to retry AGAINST, this one just means
// "try again shortly" — and distinct from an untagged error because those are
// the ones a client cannot do anything about. The C++ layer maps this onto 503
// rather than 502: a routine failover must not look fatal to a client that
// retries on 503 and gives up on 502.
const UnavailablePrefix = "unavailable:"

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

// readTimeout bounds the Barrier in a linearizable read. Unlike proposeTimeout
// this one does what its name says: raft.Barrier's timeout covers the whole
// wait, so a leader that cannot make progress fails the read instead of hanging
// the client.
const readTimeout = 4 * time.Second

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

	// IsLeader reports this node's OWN belief about whether it leads. Cheap and
	// local, and therefore only good enough to decide whether to forward —
	// VerifyLeader is the authoritative check.
	IsLeader() bool

	// Barrier blocks until every entry committed before the call has been
	// applied to this node's FSM. On the leader that is what turns a local read
	// into a linearizable one (R4.5).
	Barrier(timeout time.Duration) error

	// VerifyLeader confirms with a quorum that this node is still the leader.
	// Barrier alone is not enough: a partitioned old leader can satisfy its own
	// barrier while a new leader elsewhere has already moved on.
	VerifyLeader() error
}

// ReadForwarder relays a linearizable read to the leader on a follower's behalf.
// *Forwarder satisfies it; kept as an interface so the handler can be tested
// without a real peer.
type ReadForwarder interface {
	ForwardRead(ctx context.Context, raftAddr string, key string) (*pb.ReadResponse, error)
}

// LocalReader reads one key from this node's own C++ state machine.
// The sidecar only calls it after establishing that this node is the leader.
type LocalReader interface {
	Get(ctx context.Context, key string) (found bool, value []byte, err error)
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
	reader     LocalReader
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

// WithLocalReader supplies the C++ state machine this node reads from when it
// serves a linearizable read (R4.5).
//
// Separate from NewServer because Read is optional: without a reader the RPC
// answers with a truthful error rather than a wrong value, and Propose — the
// path every prior phase depends on — keeps working untouched.
func (s *Server) WithLocalReader(reader LocalReader) *Server {
	s.reader = reader
	return s
}

// Read serves a linearizable read (R4.5).
//
// The linearizability argument, in order, because each step covers a failure the
// others do not:
//
//  1. Only the leader may answer. A follower's local store can be arbitrarily
//     stale, so it forwards — exactly once, same guard as Propose.
//  2. Barrier() waits for everything committed before this call to be applied
//     here. Without it the leader could answer from a state that is missing a
//     write it has already acknowledged.
//  3. VerifyLeader() confirms with a quorum that we still lead. Barrier alone is
//     not enough: a partitioned old leader satisfies its own barrier happily
//     while a new leader elsewhere has already accepted newer writes. Doing this
//     AFTER the barrier is deliberate — it is the barrier's result we need to
//     trust, so leadership has to hold as of the later moment.
//  4. Only then read the local store, which is now known to contain every
//     acknowledged write.
//
// Failures are reported in ReadResponse.error with a nil gRPC error, matching
// Propose: the C++ client reads the field.
func (s *Server) Read(ctx context.Context, req *pb.ReadRequest) (*pb.ReadResponse, error) {
	// Cheap local filter: a node that does not even think it leads has nothing
	// to gain from a Barrier or a quorum check, so forward immediately. The
	// authoritative VerifyLeader happens below, after the barrier.
	if !s.node.IsLeader() {
		resp := s.forwardRead(ctx, req)
		if resp.GetError() == "" {
			metrics.ReadTotal.WithLabelValues(metrics.OutcomeForwarded).Inc()
		} else if strings.HasPrefix(resp.GetError(), NotLeaderPrefix) {
			metrics.ReadTotal.WithLabelValues(metrics.OutcomeNotLeader).Inc()
		} else {
			metrics.ReadTotal.WithLabelValues(metrics.OutcomeError).Inc()
		}
		return resp, nil
	}

	if s.reader == nil {
		return &pb.ReadResponse{
			Error: "linearizable reads are not configured on this node",
		}, nil
	}

	if err := s.node.Barrier(readTimeout); err != nil {
		log.Printf("ERROR: read barrier failed: %v", err)
		return &pb.ReadResponse{
			Error: fmt.Sprintf("read barrier failed: %v", err),
		}, nil
	}

	// After the barrier, not before — see step 3 above.
	if err := s.node.VerifyLeader(); err != nil {
		leader := s.node.LeaderAddr()
		log.Printf("Read rejected: leadership not confirmed (%v, leader=%q)", err, leader)
		return &pb.ReadResponse{Error: NotLeaderPrefix + leader}, nil
	}

	found, value, err := s.reader.Get(ctx, req.GetKey())
	if err != nil {
		log.Printf("ERROR: local read failed: %v", err)
		return &pb.ReadResponse{
			Error: fmt.Sprintf("local read failed: %v", err),
		}, nil
	}

	metrics.ReadTotal.WithLabelValues(metrics.OutcomeOK).Inc()
	return &pb.ReadResponse{Found: found, Value: value}, nil
}

// forwardRead relays a read to the leader, or explains why it could not.
func (s *Server) forwardRead(ctx context.Context, req *pb.ReadRequest) *pb.ReadResponse {
	leader := s.node.LeaderAddr()

	// One hop only, same reasoning as Propose: if a peer sent this here
	// believing we lead and we do not, our hint is no better than theirs.
	if req.GetForwarded() {
		log.Printf("Refusing already-forwarded read: this node is not the leader")
		return &pb.ReadResponse{Error: NotLeaderPrefix + leader}
	}
	if s.forwarder == nil {
		return &pb.ReadResponse{Error: NotLeaderPrefix + leader}
	}

	// No leader known at all — during an election, or before this node has heard
	// from one. That is a "not here, not now" condition, identical to what the
	// write path reports, so it gets the same not_leader: prefix and therefore
	// the same 503. Attempting to forward to an empty address would instead
	// surface a resolver error and map onto a 502, telling the client the
	// cluster is broken when it is merely mid-election.
	if leader == "" {
		log.Printf("Read rejected: no leader known")
		return &pb.ReadResponse{Error: NotLeaderPrefix}
	}

	readForwarder, ok := s.forwarder.(ReadForwarder)
	if !ok {
		return &pb.ReadResponse{Error: NotLeaderPrefix + leader}
	}

	log.Printf("Forwarding linearizable read to the leader at %s", leader)
	resp, err := readForwarder.ForwardRead(ctx, leader, req.GetKey())
	if err != nil {
		// Same reasoning as the write path: transient, so retryable.
		log.Printf("Forwarding read to %s failed: %v", leader, err)
		return &pb.ReadResponse{
			Error: fmt.Sprintf("%scould not reach leader %s: %v",
				UnavailablePrefix, leader, err),
		}
	}
	return resp
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
					// Tagged unavailable, not not_leader: the client should not be
					// told "retry at the leader" when reaching the leader is the
					// thing that just failed. But it IS retryable — during an
					// election the address we have is a node that has just died —
					// so it must not surface as a fatal 502 either.
					log.Printf("ERROR: forwarding to leader %s failed: %v", leader, ferr)
					return &pb.ProposeResponse{
						Success: false,
						Error: fmt.Sprintf("%scould not reach leader %s: %v",
							UnavailablePrefix, leader, ferr),
					}, nil
				}
				metrics.ProposeTotal.WithLabelValues(metrics.OutcomeForwarded).Inc()
				return forwarded, nil
			}

			if cmd.GetForwarded() {
				log.Printf("Propose rejected: arrived forwarded but this node is "+
					"not the leader (leader=%q) — refusing to forward again", leader)
			} else {
				log.Printf("Propose rejected: not the leader (leader=%q)", leader)
			}
			metrics.ProposeTotal.WithLabelValues(metrics.OutcomeNotLeader).Inc()
			return &pb.ProposeResponse{
				Success: false,
				Error:   NotLeaderPrefix + leader,
			}, nil
		}
		log.Printf("ERROR: raft apply failed: %v", err)
		metrics.ProposeTotal.WithLabelValues(metrics.OutcomeError).Inc()
		return &pb.ProposeResponse{Success: false, Error: err.Error()}, nil
	}

	if applyErr, ok := resp.(error); ok && applyErr != nil {
		log.Printf("ERROR: entry committed but the state machine rejected it: %v",
			applyErr)
		metrics.ProposeTotal.WithLabelValues(metrics.OutcomeError).Inc()
		return &pb.ProposeResponse{Success: false, Error: applyErr.Error()}, nil
	}

	metrics.ProposeTotal.WithLabelValues(metrics.OutcomeOK).Inc()
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
