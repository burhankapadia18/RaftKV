package rpc

import (
	"context"
	"fmt"
	"sync"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	pb "my-raft-sidecar/pb"
)

// PeerResolver maps a peer's Raft address to its RaftNode gRPC address.
// *peers.Resolver satisfies it; declared here so this package does not depend
// on package peers, matching the consumer-side-interface convention used by
// management.RaftControl and rpc.RaftProposer.
type PeerResolver interface {
	RPCAddr(raftAddr string) (string, error)
}

// leaderClient is the subset of the generated RaftNode client that forwarding
// uses. Keeping it an interface is what lets the forwarding tests run without a
// real gRPC server.
type leaderClient interface {
	Propose(ctx context.Context, in *pb.Command, opts ...grpc.CallOption) (*pb.ProposeResponse, error)
	Read(ctx context.Context, in *pb.ReadRequest, opts ...grpc.CallOption) (*pb.ReadResponse, error)
}

// dialFunc opens a connection to a peer. Swapped out in tests.
type dialFunc func(addr string) (leaderClient, func() error, error)

// Forwarder holds one gRPC connection to the current leader and replaces it
// when leadership moves.
//
// Connections are cached per leader address rather than dialled per request:
// forwarding is on the write path, and a fresh TCP+HTTP/2 handshake for every
// proposal would dominate the cost of the proposal itself. grpc.ClientConn is
// safe for concurrent use, so one is shared across all forwarded calls.
//
// The cache holds exactly one entry. A cluster has one leader at a time, so a
// map would only ever accumulate connections to nodes that used to lead.
type Forwarder struct {
	resolver PeerResolver
	dial     dialFunc

	mu     sync.Mutex
	addr   string       // raft address the cached conn was opened for
	client leaderClient // nil when nothing is cached
	closer func() error
}

// NewForwarder returns a Forwarder that dials real gRPC peers.
func NewForwarder(resolver PeerResolver) *Forwarder {
	return &Forwarder{resolver: resolver, dial: dialGRPC}
}

// dialGRPC opens a real insecure gRPC connection to a peer sidecar.
//
// grpc.NewClient (not the deprecated grpc.Dial) and no blocking wait: the very
// first forwarded call absorbs the connection setup, and a failure there is
// reported to the caller like any other forwarding failure. Blocking here would
// stall Propose behind a dial to a peer that may already be gone.
func dialGRPC(addr string) (leaderClient, func() error, error) {
	conn, err := grpc.NewClient(addr,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, nil, fmt.Errorf("dialling leader at %s: %w", addr, err)
	}
	return pb.NewRaftNodeClient(conn), conn.Close, nil
}

// clientFor returns a client for the peer at raftAddr, reusing the cached
// connection when the leader has not changed.
func (f *Forwarder) clientFor(raftAddr string) (leaderClient, error) {
	rpcAddr, err := f.resolver.RPCAddr(raftAddr)
	if err != nil {
		return nil, err
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	if f.client != nil && f.addr == raftAddr {
		return f.client, nil
	}

	// Leadership moved (or this is the first forward). Drop the old connection
	// before opening a new one, so a cluster that re-elects repeatedly does not
	// leak a ClientConn per election.
	f.closeLocked()

	client, closer, err := f.dial(rpcAddr)
	if err != nil {
		return nil, err
	}
	f.addr, f.client, f.closer = raftAddr, client, closer
	return client, nil
}

// closeLocked drops any cached connection. Caller must hold f.mu.
func (f *Forwarder) closeLocked() {
	if f.closer != nil {
		_ = f.closer()
	}
	f.addr, f.client, f.closer = "", nil, nil
}

// Close releases the cached connection.
func (f *Forwarder) Close() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closeLocked()
}

// ForwardPropose sends cmd to the leader at raftAddr and relays its response.
//
// The command is marked forwarded so the leader will not forward it again: if
// the node we picked is not actually the leader any more, it refuses with
// not_leader rather than bouncing the request onward. That caps the hop count
// at one no matter how stale our leader hint is (R4.1).
func (f *Forwarder) ForwardPropose(ctx context.Context, raftAddr string, cmd *pb.Command) (*pb.ProposeResponse, error) {
	client, err := f.clientFor(raftAddr)
	if err != nil {
		return nil, err
	}

	forwarded := &pb.Command{
		Op:        cmd.GetOp(),
		Key:       cmd.GetKey(),
		Value:     cmd.GetValue(),
		Data:      cmd.GetData(),
		Forwarded: true,
	}

	resp, err := client.Propose(ctx, forwarded)
	if err != nil {
		// The cached connection may be to a node that has gone away. Drop it so
		// the next attempt redials rather than reusing a dead channel.
		f.Close()
		return nil, fmt.Errorf("forwarding propose to %s: %w", raftAddr, err)
	}
	return resp, nil
}

// ForwardRead sends a linearizable read to the leader at raftAddr (R4.5), with
// the same one-hop guard as ForwardPropose.
func (f *Forwarder) ForwardRead(ctx context.Context, raftAddr string, key string) (*pb.ReadResponse, error) {
	client, err := f.clientFor(raftAddr)
	if err != nil {
		return nil, err
	}

	resp, err := client.Read(ctx, &pb.ReadRequest{Key: key, Forwarded: true})
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("forwarding read to %s: %w", raftAddr, err)
	}
	return resp, nil
}
