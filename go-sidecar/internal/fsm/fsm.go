// Package fsm provides the Finite State Machine implementation for Raft.
package fsm

import (
	"context"
	"fmt"
	"io"
	"log"

	"github.com/hashicorp/raft"

	pb "my-raft-sidecar/pb"
)

// ApplyError reports a Raft log entry that this node could not apply to its
// local C++ state machine.
//
// The raft.FSM interface has no error return, so this is handed back as
// CppFSM.Apply's value. Raft stores it on ApplyFuture.Response(), which is how
// rpc.Server.Propose recovers the real reason and reports it to the client
// instead of pretending the write succeeded.
//
// Index and Term identify exactly which entry failed, which is what an operator
// needs in order to go and look at it.
//
// Two very different failures land here, and it matters which one you are
// looking at:
//
//   - The state machine ANSWERED and rejected the entry (Success == false).
//     That verdict is deterministic — it is a function of the entry's bytes, so
//     every replica reaches it independently and identically. Replicas stay
//     consistent with each other; the entry is simply a permanent no-op in the
//     log. Err is nil in this case.
//   - The state machine could NOT be reached, or answered nothing (transport
//     failure, nil response). That outcome depends on this node's local
//     conditions, so other replicas may well have applied the entry while this
//     one did not. THIS is the case that can actually diverge a replica. Err is
//     non-nil.
type ApplyError struct {
	// Index is the Raft log index of the entry that failed to apply.
	Index uint64
	// Term is the Raft term of the entry that failed to apply.
	Term uint64
	// Reason is the human-readable cause. For a state-machine rejection it is
	// ApplyResponse.error; for a transport failure it is the gRPC error text.
	Reason string
	// Err is the underlying cause when the gRPC call itself failed. It is nil
	// when the state machine answered but reported failure.
	Err error
}

// Error implements the error interface.
func (e *ApplyError) Error() string {
	return fmt.Sprintf("fsm: failed to apply raft log entry index=%d term=%d: %s",
		e.Index, e.Term, e.Reason)
}

// Unwrap exposes the underlying transport error to errors.Is and errors.As.
func (e *ApplyError) Unwrap() error { return e.Err }

// Ensure ApplyError implements error at compile time.
var _ error = (*ApplyError)(nil)

// StateMachineClient defines the interface for applying commands to the state machine.
// This abstraction allows for easier testing and decoupling from gRPC.
type StateMachineClient interface {
	Apply(ctx context.Context, cmd *pb.Command) (*pb.ApplyResponse, error)
}

// grpcStateMachineClient wraps the generated gRPC client to satisfy our interface.
type grpcStateMachineClient struct {
	client pb.StateMachineClient
}

// Apply forwards the command to the C++ backend via gRPC.
func (g *grpcStateMachineClient) Apply(ctx context.Context, cmd *pb.Command) (*pb.ApplyResponse, error) {
	return g.client.Apply(ctx, cmd)
}

// NewStateMachineClient creates a StateMachineClient from a gRPC client.
func NewStateMachineClient(client pb.StateMachineClient) StateMachineClient {
	return &grpcStateMachineClient{client: client}
}

// CppFSM implements the raft.FSM interface, forwarding Apply calls to the C++ backend.
type CppFSM struct {
	client StateMachineClient
}

// NewCppFSM creates a new FSM that delegates to the given state machine client.
func NewCppFSM(client StateMachineClient) *CppFSM {
	return &CppFSM{client: client}
}

// Apply applies a Raft log entry to the C++ backend.
//
// It returns nil only when the state machine confirms the entry was applied.
// Every other outcome (transport failure, missing response, or an explicit
// ApplyResponse.success == false) yields a non-nil *ApplyError, which Raft
// surfaces on ApplyFuture.Response(). Reporting nil here would tell Raft the
// entry is applied on this node when it is not, silently diverging this
// replica's state from the log.
func (f *CppFSM) Apply(l *raft.Log) interface{} {
	resp, err := f.client.Apply(context.Background(), &pb.Command{Data: l.Data})
	if err != nil {
		applyErr := &ApplyError{
			Index:  l.Index,
			Term:   l.Term,
			Reason: err.Error(),
			Err:    err,
		}
		// Local, non-deterministic failure: peers that could reach their own
		// state machine have applied this entry and this node has not. This is
		// the branch that genuinely risks divergence.
		log.Printf("ERROR: %v (could not reach the C++ state machine; peers may "+
			"have applied this entry, so THIS REPLICA MAY NOW BE DIVERGED)",
			applyErr)
		return applyErr
	}

	if resp == nil {
		applyErr := &ApplyError{
			Index:  l.Index,
			Term:   l.Term,
			Reason: "state machine returned a nil response",
		}
		log.Printf("ERROR: %v (outcome unknown, so THIS REPLICA MAY NOW BE DIVERGED)",
			applyErr)
		return applyErr
	}

	if !resp.GetSuccess() {
		reason := resp.GetError()
		if reason == "" {
			reason = "state machine reported failure without a reason"
		}
		applyErr := &ApplyError{Index: l.Index, Term: l.Term, Reason: reason}
		// Deliberately NOT described as divergence. The rejection is a pure
		// function of the entry's bytes, so every replica rejects it identically
		// and the cluster stays consistent — the entry is just a permanent no-op.
		// Saying "diverged" here would send an operator hunting a problem that
		// does not exist, and this path is reached by any client sending a bad
		// command, so it would cry wolf constantly.
		log.Printf("ERROR: %v (state machine rejected the entry; this verdict is "+
			"deterministic, so all replicas reject it identically and stay "+
			"consistent — the entry is a permanent no-op in the log)", applyErr)
		return applyErr
	}

	return nil
}

// Snapshot returns a snapshot of the FSM state.
// Currently returns a dummy snapshot as snapshot support is not fully implemented.
func (f *CppFSM) Snapshot() (raft.FSMSnapshot, error) {
	return &DummySnapshot{}, nil
}

// Restore restores the FSM from a snapshot.
// Currently a no-op as snapshot support is not fully implemented.
func (f *CppFSM) Restore(rc io.ReadCloser) error {
	defer rc.Close()
	return nil
}

// DummySnapshot is a placeholder snapshot implementation.
type DummySnapshot struct{}

// Persist writes the snapshot to the given sink.
func (d *DummySnapshot) Persist(sink raft.SnapshotSink) error {
	defer sink.Close()
	return nil
}

// Release releases any resources held by the snapshot.
func (d *DummySnapshot) Release() {}

// Ensure CppFSM implements raft.FSM at compile time.
var _ raft.FSM = (*CppFSM)(nil)

// Ensure DummySnapshot implements raft.FSMSnapshot at compile time.
var _ raft.FSMSnapshot = (*DummySnapshot)(nil)
