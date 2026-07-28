package rpc

import (
	"context"
	"errors"
	"strings"
	"sync"
	"testing"

	"github.com/hashicorp/raft"

	pb "my-raft-sidecar/pb"
)

// recordingForwarder is a ProposeForwarder that records what it was asked to
// relay, so the handler's forward-or-refuse decision can be observed directly.
type recordingForwarder struct {
	mu    sync.Mutex
	calls []forwardCall

	resp *pb.ProposeResponse
	err  error
}

type forwardCall struct {
	raftAddr string
	cmd      *pb.Command
}

var _ ProposeForwarder = (*recordingForwarder)(nil)

func (f *recordingForwarder) ForwardPropose(ctx context.Context, raftAddr string, cmd *pb.Command) (*pb.ProposeResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, forwardCall{raftAddr: raftAddr, cmd: cmd})
	return f.resp, f.err
}

func (f *recordingForwarder) callsSnapshot() []forwardCall {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]forwardCall(nil), f.calls...)
}

// TestProposeForwardsWhenNotLeader is the whole point of Slice A: a write sent
// to a follower now succeeds instead of being refused.
func TestProposeForwardsWhenNotLeader(t *testing.T) {
	node := &fakeProposer{err: raft.ErrNotLeader, leaderAddr: "node1:8088"}
	fwd := &recordingForwarder{resp: &pb.ProposeResponse{Success: true}}
	server := NewServer(node, fwd)

	resp, err := server.Propose(context.Background(), &pb.Command{Data: []byte("payload")})
	if err != nil {
		t.Fatalf("Propose returned a gRPC error: %v", err)
	}
	if !resp.GetSuccess() {
		t.Errorf("response = %+v, want the leader's success relayed", resp)
	}

	calls := fwd.callsSnapshot()
	if len(calls) != 1 {
		t.Fatalf("forwarder saw %d calls, want 1", len(calls))
	}
	if calls[0].raftAddr != "node1:8088" {
		t.Errorf("forwarded to %q, want the leader address from LeaderAddr()",
			calls[0].raftAddr)
	}
	if string(calls[0].cmd.GetData()) != "payload" {
		t.Errorf("forwarded payload = %q, want it unchanged", calls[0].cmd.GetData())
	}
}

// TestProposeDoesNotForwardAnAlreadyForwardedCommand is the loop guard's
// receiving half, and the reason a stale leader hint cannot become an infinite
// forwarding cycle between two nodes that each think the other leads.
func TestProposeDoesNotForwardAnAlreadyForwardedCommand(t *testing.T) {
	node := &fakeProposer{err: raft.ErrNotLeader, leaderAddr: "node2:8088"}
	fwd := &recordingForwarder{resp: &pb.ProposeResponse{Success: true}}
	server := NewServer(node, fwd)

	resp, err := server.Propose(context.Background(),
		&pb.Command{Data: []byte("payload"), Forwarded: true})
	if err != nil {
		t.Fatalf("Propose returned a gRPC error: %v", err)
	}

	if len(fwd.callsSnapshot()) != 0 {
		t.Error("an already-forwarded command must NOT be forwarded again — " +
			"that is how two stale leader hints produce a forwarding loop")
	}
	if resp.GetSuccess() {
		t.Error("response = success, want a refusal")
	}
	if got := resp.GetError(); got != NotLeaderPrefix+"node2:8088" {
		t.Errorf("error = %q, want the Phase 1 %q contract", got,
			NotLeaderPrefix+"node2:8088")
	}
}

// TestProposeRefusesWhenNoLeaderIsKnown: during an election LeaderAddr() is
// empty, and there is nowhere to forward to. Refuse rather than dial "".
func TestProposeRefusesWhenNoLeaderIsKnown(t *testing.T) {
	node := &fakeProposer{err: raft.ErrNotLeader, leaderAddr: ""}
	fwd := &recordingForwarder{resp: &pb.ProposeResponse{Success: true}}
	server := NewServer(node, fwd)

	resp, err := server.Propose(context.Background(), &pb.Command{})
	if err != nil {
		t.Fatalf("Propose returned a gRPC error: %v", err)
	}
	if len(fwd.callsSnapshot()) != 0 {
		t.Error("must not attempt to forward when no leader is known")
	}
	if resp.GetError() != NotLeaderPrefix {
		t.Errorf("error = %q, want a bare %q", resp.GetError(), NotLeaderPrefix)
	}
}

// TestForwardingFailureIsNotReportedAsNotLeader guards a subtle honesty
// property. If forwarding fails as a transport matter, the client must not be
// told "not the leader, retry over there" — the leader hint is fine, it is the
// connection that is broken, and mislabelling it sends the client into a retry
// loop against a node it also cannot reach.
func TestForwardingFailureIsNotReportedAsNotLeader(t *testing.T) {
	node := &fakeProposer{err: raft.ErrNotLeader, leaderAddr: "node1:8088"}
	fwd := &recordingForwarder{err: errors.New("connection refused")}
	server := NewServer(node, fwd)

	resp, err := server.Propose(context.Background(), &pb.Command{})
	if err != nil {
		t.Fatalf("Propose returned a gRPC error: %v", err)
	}
	if resp.GetSuccess() {
		t.Error("a failed forward must not be reported as success")
	}
	if strings.HasPrefix(resp.GetError(), NotLeaderPrefix) {
		t.Errorf("error = %q, must NOT carry the not_leader prefix: the problem "+
			"is reaching the leader, not identifying it", resp.GetError())
	}
	if !strings.Contains(resp.GetError(), "connection refused") {
		t.Errorf("error = %q, want it to name the underlying failure", resp.GetError())
	}
	if !strings.Contains(resp.GetError(), "node1:8088") {
		t.Errorf("error = %q, want it to name the leader it could not reach",
			resp.GetError())
	}
}

// TestNilForwarderRestoresPhase1Behaviour: NewServer(node, nil) must refuse a
// follower write rather than panic, so a partially-wired binary fails loudly
// and predictably.
func TestNilForwarderRestoresPhase1Behaviour(t *testing.T) {
	node := &fakeProposer{err: raft.ErrNotLeader, leaderAddr: "node1:8088"}
	server := NewServer(node, nil)

	resp, err := server.Propose(context.Background(), &pb.Command{})
	if err != nil {
		t.Fatalf("Propose returned a gRPC error: %v", err)
	}
	if resp.GetError() != NotLeaderPrefix+"node1:8088" {
		t.Errorf("error = %q, want the Phase 1 refusal", resp.GetError())
	}
}

// TestLeaderDoesNotForward: the happy path must never consult the forwarder.
func TestLeaderDoesNotForward(t *testing.T) {
	node := &fakeProposer{resp: nil, err: nil}
	fwd := &recordingForwarder{}
	server := NewServer(node, fwd)

	resp, err := server.Propose(context.Background(), &pb.Command{Data: []byte("x")})
	if err != nil {
		t.Fatalf("Propose: %v", err)
	}
	if !resp.GetSuccess() {
		t.Errorf("response = %+v, want success on the leader", resp)
	}
	if len(fwd.callsSnapshot()) != 0 {
		t.Error("the leader must apply locally, never forward")
	}
}

// TestNonLeaderErrorsAreNotForwarded: only ErrNotLeader means "someone else can
// do this". A commit failure or an FSM rejection must be reported as itself.
func TestNonLeaderErrorsAreNotForwarded(t *testing.T) {
	tests := []struct {
		name string
		node *fakeProposer
	}{
		{
			name: "raft error that is not ErrNotLeader",
			node: &fakeProposer{err: errors.New("enqueue timeout"), leaderAddr: "node1:8088"},
		},
		{
			name: "entry committed but the state machine refused it",
			node: &fakeProposer{resp: errors.New("empty key"), leaderAddr: "node1:8088"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			fwd := &recordingForwarder{resp: &pb.ProposeResponse{Success: true}}
			server := NewServer(tc.node, fwd)

			resp, err := server.Propose(context.Background(), &pb.Command{})
			if err != nil {
				t.Fatalf("Propose: %v", err)
			}
			if len(fwd.callsSnapshot()) != 0 {
				t.Error("must not forward: this failure is not about leadership")
			}
			if resp.GetSuccess() {
				t.Error("response = success, want the failure reported")
			}
			if strings.HasPrefix(resp.GetError(), NotLeaderPrefix) {
				t.Errorf("error = %q, must not be dressed up as not_leader",
					resp.GetError())
			}
		})
	}
}
