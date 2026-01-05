// Package fsm provides the Finite State Machine implementation for Raft.
package fsm

import (
	"context"
	"io"
	"log"

	"github.com/hashicorp/raft"

	pb "my-raft-sidecar/pb"
)

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
func (f *CppFSM) Apply(l *raft.Log) interface{} {
	_, err := f.client.Apply(context.Background(), &pb.Command{Data: l.Data})
	if err != nil {
		log.Printf("ERROR: Failed to apply to C++ DB: %v", err)
		return err
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
