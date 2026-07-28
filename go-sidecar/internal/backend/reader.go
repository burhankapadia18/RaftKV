package backend

import (
	"context"
	"fmt"

	pb "my-raft-sidecar/pb"
)

// StoreReader reads a single key from the local C++ state machine.
//
// This is the second half of a linearizable read (R4.5): the sidecar calls it
// only after it has run a Barrier and confirmed with a quorum that it still
// leads. On its own it is just a local lookup, with exactly the staleness
// GET ?consistency=local has.
type StoreReader struct {
	client pb.StateMachineClient
}

// NewStoreReader wraps a generated StateMachine client.
func NewStoreReader(client pb.StateMachineClient) *StoreReader {
	return &StoreReader{client: client}
}

// Get looks up one key.
//
// A miss is (false, nil, nil) — not an error. Conflating "the key is absent"
// with "the lookup failed" is how a read failure gets reported to a client as a
// definitive "not found".
func (r *StoreReader) Get(ctx context.Context, key string) (bool, []byte, error) {
	resp, err := r.client.Get(ctx, &pb.GetRequest{Key: key})
	if err != nil {
		return false, nil, fmt.Errorf("reading %q from the state machine: %w", key, err)
	}
	return resp.GetFound(), resp.GetValue(), nil
}
