// Package fsm provides the Finite State Machine implementation for Raft.
package fsm

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log"

	"github.com/hashicorp/raft"

	pb "my-raft-sidecar/pb"
)

// snapshotChunkSize is the payload size of a single SnapshotChunk, in both
// directions.
//
// 64 KiB sits far below gRPC's 4 MiB default message limit while keeping the
// per-message framing overhead negligible, and it is the same size the C++
// GetSnapshot handler emits — see docs/phases/phase-3-snapshots.md (R3.1).
const snapshotChunkSize = 64 * 1024

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

// StateMachineClient is the consumer-side view of the C++ engine that this
// package needs. It wraps the generated gRPC client so the FSM can be tested
// with a fake and stays decoupled from gRPC itself.
//
// The two streaming calls deliberately drop the variadic grpc.CallOption tail
// of the generated signatures: nothing here passes call options, and leaving
// them out keeps the fakes small. The returned stream types are the generated
// aliases, so no gRPC import leaks into this file.
type StateMachineClient interface {
	// Apply hands one committed Raft log entry to the state machine.
	Apply(ctx context.Context, cmd *pb.Command) (*pb.ApplyResponse, error)

	// GetSnapshot opens a server stream over which the C++ engine sends its
	// entire state, chunked. The payload is the Phase 2 base-file encoding.
	GetSnapshot(ctx context.Context, in *pb.SnapshotRequest) (pb.StateMachine_GetSnapshotClient, error)

	// RestoreSnapshot opens a client stream over which a snapshot is pushed
	// into the C++ engine, replacing its state wholesale.
	RestoreSnapshot(ctx context.Context) (pb.StateMachine_RestoreSnapshotClient, error)
}

// grpcStateMachineClient wraps the generated gRPC client to satisfy our interface.
type grpcStateMachineClient struct {
	client pb.StateMachineClient
}

// Apply forwards the command to the C++ backend via gRPC.
func (g *grpcStateMachineClient) Apply(ctx context.Context, cmd *pb.Command) (*pb.ApplyResponse, error) {
	return g.client.Apply(ctx, cmd)
}

// GetSnapshot opens the snapshot-export stream on the C++ backend.
func (g *grpcStateMachineClient) GetSnapshot(ctx context.Context, in *pb.SnapshotRequest) (pb.StateMachine_GetSnapshotClient, error) {
	return g.client.GetSnapshot(ctx, in)
}

// RestoreSnapshot opens the snapshot-import stream on the C++ backend.
func (g *grpcStateMachineClient) RestoreSnapshot(ctx context.Context) (pb.StateMachine_RestoreSnapshotClient, error) {
	return g.client.RestoreSnapshot(ctx)
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

// Snapshot captures the C++ state machine's entire state and returns it as a
// raft.FSMSnapshot ready to be written out.
//
// WHEN the state is read is the whole correctness argument here, so do not
// "optimise" the capture into Persist. Raft calls Snapshot() on the FSM
// goroutine with no Apply in flight, and may call FSMSnapshot.Persist() much
// later, concurrently with subsequent Applies. The snapshot is labelled with
// the log index that had been applied when Snapshot() returned. Reading the
// state inside Persist would therefore file the state as of some later index
// N+k under the label N, and a node restoring it would skip entries N+1..N+k
// that raft believes the snapshot already contains.
//
// For this store's idempotent SET/DELETE that mislabelling happens to
// re-converge on replay, so it would look perfectly healthy in testing while
// being wrong in general. Capturing eagerly removes the class of bug instead of
// relying on that accident.
//
// This deviates from the phase doc's R3.4, which describes the lazy variant.
//
// The two costs are real and worth knowing:
//
//   - The whole store is held in the sidecar's heap for the lifetime of the
//     snapshot. Fine at this scale, but it is a hard scaling limit: the store
//     must fit in memory twice over (once in C++, once here) during a snapshot.
//   - The FSM goroutine — hence apply — is stalled for the length of the
//     capture, so this is not entirely off the write path the way the phase doc
//     assumes. The stall is bounded by serialise-plus-loopback, not by disk or
//     peer network I/O, because the C++ side holds its store mutex only for the
//     map copy (R3.2) and streams afterwards.
//
// A failure returns (nil, error). Raft logs it and retries on its next snapshot
// tick, which is the outcome we want: no new snapshot beats a wrong one.
func (f *CppFSM) Snapshot() (raft.FSMSnapshot, error) {
	// Cancelled on every exit path, which releases the stream if we abandon it
	// part-way through instead of draining it to EOF.
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	stream, err := f.client.GetSnapshot(ctx, &pb.SnapshotRequest{})
	if err != nil {
		return nil, fmt.Errorf("fsm: opening GetSnapshot on the C++ state machine: %w", err)
	}
	if stream == nil {
		return nil, errors.New("fsm: the C++ state machine returned a nil snapshot stream")
	}

	var state bytes.Buffer
	for {
		chunk, recvErr := stream.Recv()
		if recvErr != nil {
			if errors.Is(recvErr, io.EOF) {
				break
			}
			return nil, fmt.Errorf(
				"fsm: reading the snapshot stream from the C++ state machine after %d byte(s): %w",
				state.Len(), recvErr)
		}
		// bytes.Buffer.Write never returns an error; it panics on OOM instead.
		state.Write(chunk.GetData())
	}

	log.Printf("fsm: captured a %d byte snapshot of the C++ state machine", state.Len())
	return &cppSnapshot{state: state.Bytes()}, nil
}

// Restore replaces the C++ state machine's state with the snapshot in rc.
//
// It fails hard. A node that cannot install the snapshot must not go on serving
// reads, because raft has already decided this node's state is whatever the
// snapshot said — returning nil here would leave it claiming state it does not
// have (R3.5).
func (f *CppFSM) Restore(rc io.ReadCloser) error {
	// Raft hands ownership of the reader over, so it is closed exactly once on
	// every exit path. Close's own error is deliberately discarded: by the time
	// it fires the bytes have already been read and accepted by C++, and
	// failing a good restore over a file handle would take a healthy node out
	// of service for nothing.
	defer rc.Close()

	// Cancelled on every exit path. That is what makes the error paths safe:
	// returning without CloseAndRecv aborts the RPC, so the C++ side sees a
	// cancelled restore and keeps its existing state rather than committing
	// whatever prefix of the snapshot it happened to receive.
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	stream, err := f.client.RestoreSnapshot(ctx)
	if err != nil {
		return fmt.Errorf("fsm: opening RestoreSnapshot on the C++ state machine: %w", err)
	}
	if stream == nil {
		return errors.New("fsm: the C++ state machine returned a nil restore stream")
	}

	sent, aborted, err := streamSnapshot(rc, stream)
	if err != nil {
		return err
	}

	resp, err := stream.CloseAndRecv()
	if err != nil {
		return fmt.Errorf("fsm: completing RestoreSnapshot after %d byte(s): %w", sent, err)
	}
	if resp == nil {
		return fmt.Errorf(
			"fsm: the C++ state machine returned a nil RestoreSnapshot response after %d byte(s)",
			sent)
	}
	if !resp.GetSuccess() {
		reason := resp.GetError()
		if reason == "" {
			reason = "state machine reported failure without a reason"
		}
		return fmt.Errorf("fsm: the C++ state machine rejected the snapshot after %d byte(s): %s",
			sent, reason)
	}
	if aborted {
		// The server ended the stream early and then reported success anyway.
		// It cannot have seen the whole snapshot, so believing it would be the
		// exact "claiming state it does not have" failure R3.5 forbids.
		return fmt.Errorf(
			"fsm: the C++ state machine reported a successful restore, but it ended the "+
				"stream after only %d byte(s): refusing to treat a truncated restore as complete",
			sent)
	}

	log.Printf("fsm: restored a %d byte snapshot into the C++ state machine", sent)
	return nil
}

// streamSnapshot copies r into an open RestoreSnapshot stream in
// snapshotChunkSize pieces.
//
// It returns the number of bytes handed to the stream and whether the loop
// stopped early because the server had already ended the stream. The caller
// must NOT call CloseAndRecv when a non-nil error comes back: a local read
// failure means the snapshot is truncated, and closing the send side would
// announce that truncated prefix as the complete snapshot.
func streamSnapshot(r io.Reader, stream pb.StateMachine_RestoreSnapshotClient) (int64, bool, error) {
	var sent int64
	for {
		// A fresh buffer per chunk, deliberately. grpc-go documents that a
		// message must not be modified after SendMsg returns — stats handlers
		// and tracing interceptors may read it lazily — so recycling one 64 KiB
		// buffer across iterations would rewrite bytes that are still in flight.
		chunk := make([]byte, snapshotChunkSize)
		n, readErr := r.Read(chunk)

		if n > 0 {
			if sendErr := stream.Send(&pb.SnapshotChunk{Data: chunk[:n]}); sendErr != nil {
				if !errors.Is(sendErr, io.EOF) {
					return sent, false, fmt.Errorf(
						"fsm: sending snapshot chunk at offset %d: %w", sent, sendErr)
				}
				// gRPC reports io.EOF from Send once the server has ended the
				// stream; the real status is only available from CloseAndRecv.
				// Stop sending and let the caller read it rather than surfacing
				// a bare EOF that says nothing about what went wrong.
				return sent, true, nil
			}
			sent += int64(n)
		}

		if readErr != nil {
			if errors.Is(readErr, io.EOF) {
				return sent, false, nil
			}
			return sent, false, fmt.Errorf(
				"fsm: reading the snapshot to restore at offset %d: %w", sent, readErr)
		}
	}
}

// cppSnapshot is a raft.FSMSnapshot holding a fully materialised copy of the
// C++ state machine's state, captured at the instant CppFSM.Snapshot() ran.
//
// The bytes are the Phase 2 base-file encoding ("KVB1" plus length-prefixed
// records) — byte-for-byte what kv.db holds. Nothing on the Go side parses
// them; like Command.data they travel opaquely.
type cppSnapshot struct {
	state []byte
}

// Persist writes the captured state to the sink.
//
// Any failure cancels the sink rather than closing it, and that distinction is
// the point. A sink that is Close()d is finalised and offered back to raft as a
// usable snapshot, so closing a half-written one leaves a corrupt snapshot on
// disk that some later Restore will load in earnest.
func (s *cppSnapshot) Persist(sink raft.SnapshotSink) error {
	// A short write is reported as an error by any io.Writer honouring the
	// contract ("Write must return a non-nil error if it returns n < len(p)"),
	// so checking err covers the partial-write case too.
	if _, err := sink.Write(s.state); err != nil {
		sink.Cancel()
		return fmt.Errorf("fsm: writing %d byte(s) to snapshot %s: %w", len(s.state), sink.ID(), err)
	}

	if err := sink.Close(); err != nil {
		// Close failed, so the snapshot was never finalised. Cancel is a no-op
		// on raft's own FileSnapshotSink — it has already marked itself closed
		// and removed its temp directory — but it costs nothing and tells any
		// other sink implementation to discard the partial write.
		sink.Cancel()
		return fmt.Errorf("fsm: closing snapshot %s: %w", sink.ID(), err)
	}

	return nil
}

// Release is a no-op. The snapshot owns nothing but a byte slice, which the
// garbage collector reclaims once raft drops its reference. Raft calls Release
// after Persist on both the success and failure paths, so it must stay safe to
// call repeatedly.
func (s *cppSnapshot) Release() {}

// Ensure CppFSM implements raft.FSM at compile time.
var _ raft.FSM = (*CppFSM)(nil)

// Ensure cppSnapshot implements raft.FSMSnapshot at compile time.
var _ raft.FSMSnapshot = (*cppSnapshot)(nil)
