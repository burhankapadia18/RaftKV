package rpc

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"

	"google.golang.org/grpc"

	pb "my-raft-sidecar/pb"
)

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// fakeResolver maps any raft address to itself with a marker suffix, so tests
// can assert that the resolver was consulted rather than the raft address used
// directly.
type fakeResolver struct {
	err error
}

func (r *fakeResolver) RPCAddr(raftAddr string) (string, error) {
	if r.err != nil {
		return "", r.err
	}
	return raftAddr + "|rpc", nil
}

// fakeLeader records what a forwarded call carried and returns canned answers.
type fakeLeader struct {
	mu sync.Mutex

	proposals []*pb.Command
	reads     []*pb.ReadRequest

	proposeResp *pb.ProposeResponse
	proposeErr  error
	readResp    *pb.ReadResponse
	readErr     error
}

var _ leaderClient = (*fakeLeader)(nil)

func (l *fakeLeader) Propose(ctx context.Context, in *pb.Command, opts ...grpc.CallOption) (*pb.ProposeResponse, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.proposals = append(l.proposals, in)
	return l.proposeResp, l.proposeErr
}

func (l *fakeLeader) Read(ctx context.Context, in *pb.ReadRequest, opts ...grpc.CallOption) (*pb.ReadResponse, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.reads = append(l.reads, in)
	return l.readResp, l.readErr
}

func (l *fakeLeader) lastProposal() *pb.Command {
	l.mu.Lock()
	defer l.mu.Unlock()
	if len(l.proposals) == 0 {
		return nil
	}
	return l.proposals[len(l.proposals)-1]
}

// dialRecorder builds a dialFunc that hands out a fixed client and counts dials
// per address, which is how the connection-caching tests observe reuse.
type dialRecorder struct {
	mu      sync.Mutex
	client  leaderClient
	err     error
	addrs   []string
	closes  int
	closeCh chan struct{}
}

func (d *dialRecorder) dial(addr string) (leaderClient, func() error, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.addrs = append(d.addrs, addr)
	if d.err != nil {
		return nil, nil, d.err
	}
	return d.client, func() error {
		d.mu.Lock()
		defer d.mu.Unlock()
		d.closes++
		if d.closeCh != nil {
			select {
			case d.closeCh <- struct{}{}:
			default:
			}
		}
		return nil
	}, nil
}

func (d *dialRecorder) dialCount() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return len(d.addrs)
}

func (d *dialRecorder) closeCount() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.closes
}

func (d *dialRecorder) dialedAddrs() []string {
	d.mu.Lock()
	defer d.mu.Unlock()
	return append([]string(nil), d.addrs...)
}

func newTestForwarder(resolver PeerResolver, d *dialRecorder) *Forwarder {
	return &Forwarder{resolver: resolver, dial: d.dial}
}

// ---------------------------------------------------------------------------
// ForwardPropose
// ---------------------------------------------------------------------------

// TestForwardProposeMarksTheCommandForwarded pins the loop guard's outgoing
// half. If this stops being set, two nodes with stale leader hints can bounce a
// proposal between each other indefinitely.
func TestForwardProposeMarksTheCommandForwarded(t *testing.T) {
	leader := &fakeLeader{proposeResp: &pb.ProposeResponse{Success: true}}
	d := &dialRecorder{client: leader}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	original := &pb.Command{Data: []byte("payload"), Forwarded: false}

	resp, err := f.ForwardPropose(context.Background(), "node1:8088", original)
	if err != nil {
		t.Fatalf("ForwardPropose: %v", err)
	}
	if !resp.GetSuccess() {
		t.Errorf("response = %+v, want success", resp)
	}

	sent := leader.lastProposal()
	if sent == nil {
		t.Fatal("the leader received no proposal")
	}
	if !sent.GetForwarded() {
		t.Error("forwarded proposal must carry Forwarded=true (R4.1 loop guard)")
	}
	if string(sent.GetData()) != "payload" {
		t.Errorf("Data = %q, want %q", sent.GetData(), "payload")
	}

	// The caller's command must not be mutated: it belongs to the gRPC layer
	// that handed it to Propose, and mutating a request in place is the kind of
	// aliasing bug that only shows up under concurrency.
	if original.GetForwarded() {
		t.Error("ForwardPropose mutated the caller's Command")
	}
}

func TestForwardProposeUsesTheResolvedRPCAddress(t *testing.T) {
	d := &dialRecorder{client: &fakeLeader{proposeResp: &pb.ProposeResponse{Success: true}}}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	if _, err := f.ForwardPropose(context.Background(), "node1:8088", &pb.Command{}); err != nil {
		t.Fatalf("ForwardPropose: %v", err)
	}

	got := d.dialedAddrs()
	// The raft address must go through the resolver, not be dialled directly:
	// 8088 is the raft transport, which does not speak gRPC.
	if len(got) != 1 || got[0] != "node1:8088|rpc" {
		t.Errorf("dialled %v, want the resolved address [node1:8088|rpc]", got)
	}
}

func TestForwardProposeReportsAResolverFailure(t *testing.T) {
	d := &dialRecorder{client: &fakeLeader{}}
	f := newTestForwarder(&fakeResolver{err: errors.New("no leader address known")}, d)
	t.Cleanup(f.Close)

	_, err := f.ForwardPropose(context.Background(), "", &pb.Command{})
	if err == nil || !strings.Contains(err.Error(), "no leader address known") {
		t.Errorf("error = %v, want the resolver's failure", err)
	}
	if d.dialCount() != 0 {
		t.Error("a resolver failure must not dial anything")
	}
}

func TestForwardProposeReportsADialFailure(t *testing.T) {
	d := &dialRecorder{err: errors.New("connection refused")}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	if _, err := f.ForwardPropose(context.Background(), "node1:8088", &pb.Command{}); err == nil ||
		!strings.Contains(err.Error(), "connection refused") {
		t.Errorf("error = %v, want the dial failure", err)
	}
}

// TestForwardProposeRelaysTheLeadersRefusal proves a refusal is passed through
// unchanged rather than rewritten. The leader's not_leader answer is the honest
// one when leadership moved again mid-flight, and the client needs to see it.
func TestForwardProposeRelaysTheLeadersRefusal(t *testing.T) {
	leader := &fakeLeader{proposeResp: &pb.ProposeResponse{
		Success: false,
		Error:   NotLeaderPrefix + "node2:8088",
	}}
	f := newTestForwarder(&fakeResolver{}, &dialRecorder{client: leader})
	t.Cleanup(f.Close)

	resp, err := f.ForwardPropose(context.Background(), "node1:8088", &pb.Command{})
	if err != nil {
		t.Fatalf("ForwardPropose: %v", err)
	}
	if resp.GetSuccess() {
		t.Error("a refusal must not be reported as success")
	}
	if resp.GetError() != NotLeaderPrefix+"node2:8088" {
		t.Errorf("error = %q, want it relayed verbatim", resp.GetError())
	}
}

// ---------------------------------------------------------------------------
// Connection caching
// ---------------------------------------------------------------------------

func TestConnectionIsReusedForTheSameLeader(t *testing.T) {
	d := &dialRecorder{client: &fakeLeader{proposeResp: &pb.ProposeResponse{Success: true}}}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	for i := 0; i < 5; i++ {
		if _, err := f.ForwardPropose(context.Background(), "node1:8088", &pb.Command{}); err != nil {
			t.Fatalf("ForwardPropose #%d: %v", i, err)
		}
	}

	if got := d.dialCount(); got != 1 {
		t.Errorf("dialled %d times for one leader, want 1 — forwarding is on the "+
			"write path and must not redial per request", got)
	}
}

func TestConnectionIsReplacedWhenLeadershipMoves(t *testing.T) {
	d := &dialRecorder{client: &fakeLeader{proposeResp: &pb.ProposeResponse{Success: true}}}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	for _, leader := range []string{"node1:8088", "node2:8088", "node1:8088"} {
		if _, err := f.ForwardPropose(context.Background(), leader, &pb.Command{}); err != nil {
			t.Fatalf("ForwardPropose to %s: %v", leader, err)
		}
	}

	if got := d.dialCount(); got != 3 {
		t.Errorf("dialled %d times across two leadership changes, want 3", got)
	}
	// Every superseded connection must be closed, or a cluster that re-elects
	// repeatedly leaks a ClientConn per election.
	if got := d.closeCount(); got != 2 {
		t.Errorf("closed %d superseded connections, want 2", got)
	}
}

// TestCallFailureDropsTheCachedConnection: the cached channel may point at a
// node that has gone away, so a failed call must invalidate it rather than
// leaving every subsequent forward to reuse a dead connection.
func TestCallFailureDropsTheCachedConnection(t *testing.T) {
	leader := &fakeLeader{proposeErr: errors.New("unavailable")}
	d := &dialRecorder{client: leader}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	for i := 0; i < 3; i++ {
		if _, err := f.ForwardPropose(context.Background(), "node1:8088", &pb.Command{}); err == nil {
			t.Fatalf("ForwardPropose #%d succeeded, want the transport error", i)
		}
	}

	if got := d.dialCount(); got != 3 {
		t.Errorf("dialled %d times, want 3 — a failed call must invalidate the "+
			"cached connection", got)
	}
}

func TestCloseIsIdempotent(t *testing.T) {
	d := &dialRecorder{client: &fakeLeader{proposeResp: &pb.ProposeResponse{Success: true}}}
	f := newTestForwarder(&fakeResolver{}, d)

	if _, err := f.ForwardPropose(context.Background(), "node1:8088", &pb.Command{}); err != nil {
		t.Fatalf("ForwardPropose: %v", err)
	}

	f.Close()
	f.Close()

	if got := d.closeCount(); got != 1 {
		t.Errorf("closed %d times across two Close calls, want 1", got)
	}
}

// TestConcurrentForwardsShareOneConnection is the -race guard: Propose runs on
// a gRPC handler goroutine, so many forwards happen at once.
func TestConcurrentForwardsShareOneConnection(t *testing.T) {
	d := &dialRecorder{client: &fakeLeader{proposeResp: &pb.ProposeResponse{Success: true}}}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	var wg sync.WaitGroup
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			cmd := &pb.Command{Data: []byte(fmt.Sprintf("payload-%d", i))}
			if _, err := f.ForwardPropose(context.Background(), "node1:8088", cmd); err != nil {
				t.Errorf("ForwardPropose: %v", err)
			}
		}(i)
	}
	wg.Wait()

	if got := d.dialCount(); got != 1 {
		t.Errorf("dialled %d times under concurrency, want 1", got)
	}
}

// ---------------------------------------------------------------------------
// ForwardRead
// ---------------------------------------------------------------------------

func TestForwardReadMarksTheRequestForwarded(t *testing.T) {
	leader := &fakeLeader{readResp: &pb.ReadResponse{Found: true, Value: []byte("v")}}
	f := newTestForwarder(&fakeResolver{}, &dialRecorder{client: leader})
	t.Cleanup(f.Close)

	resp, err := f.ForwardRead(context.Background(), "node1:8088", "k")
	if err != nil {
		t.Fatalf("ForwardRead: %v", err)
	}
	if !resp.GetFound() || string(resp.GetValue()) != "v" {
		t.Errorf("response = %+v, want found with value %q", resp, "v")
	}

	leader.mu.Lock()
	defer leader.mu.Unlock()
	if len(leader.reads) != 1 {
		t.Fatalf("leader saw %d reads, want 1", len(leader.reads))
	}
	if !leader.reads[0].GetForwarded() {
		t.Error("forwarded read must carry Forwarded=true (R4.5 loop guard)")
	}
	if leader.reads[0].GetKey() != "k" {
		t.Errorf("key = %q, want %q", leader.reads[0].GetKey(), "k")
	}
}

func TestForwardReadReportsATransportFailure(t *testing.T) {
	d := &dialRecorder{client: &fakeLeader{readErr: errors.New("unavailable")}}
	f := newTestForwarder(&fakeResolver{}, d)
	t.Cleanup(f.Close)

	if _, err := f.ForwardRead(context.Background(), "node1:8088", "k"); err == nil ||
		!strings.Contains(err.Error(), "unavailable") {
		t.Errorf("error = %v, want the transport failure", err)
	}
	if d.dialCount() != 1 {
		t.Errorf("dialled %d times, want 1", d.dialCount())
	}
}
