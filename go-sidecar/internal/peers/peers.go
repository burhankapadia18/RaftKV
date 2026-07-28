// Package peers maps a peer's Raft address onto the other ports that peer
// serves, so a follower can forward a request to the leader.
//
// Raft only ever tells us one thing about a peer: its Raft transport address,
// as returned by Raft.LeaderWithID() — for example "node1:8088", or
// "172.18.0.2:8088" once the advertised name has been resolved. Every other
// service that peer runs (the RaftNode gRPC on 50052, the management API on
// 6000) lives on the same host at a different port.
//
// That mapping is a CONVENTION, not something Raft knows, so it is stated in
// exactly one place — here — and the ports are configurable rather than
// hardcoded, so the convention stays honest in a deployment that does not use
// the defaults. See R4.2.
package peers

import (
	"fmt"
	"net"
	"strings"
)

// Resolver turns a peer's Raft address into that peer's other service
// addresses. The zero value is not useful; construct one with New.
type Resolver struct {
	rpcPort  string
	mgmtPort string
}

// New returns a Resolver that maps Raft addresses onto the given ports.
func New(rpcPort, mgmtPort string) *Resolver {
	return &Resolver{rpcPort: rpcPort, mgmtPort: mgmtPort}
}

// RPCAddr returns the peer's RaftNode gRPC address (the Propose/Read surface).
func (r *Resolver) RPCAddr(raftAddr string) (string, error) {
	return r.rebase(raftAddr, r.rpcPort)
}

// MgmtAddr returns the peer's management HTTP address (/join, /status).
func (r *Resolver) MgmtAddr(raftAddr string) (string, error) {
	return r.rebase(raftAddr, r.mgmtPort)
}

// rebase swaps the port of raftAddr for port, keeping the host untouched.
func (r *Resolver) rebase(raftAddr, port string) (string, error) {
	if strings.TrimSpace(raftAddr) == "" {
		// The common case, not an exotic one: LeaderWithID returns an empty
		// address whenever no leader is known — during an election, or before
		// this node has heard from one. Callers must be able to tell that apart
		// from a malformed address, so say which it is.
		return "", fmt.Errorf("peers: no leader address known")
	}
	if port == "" {
		return "", fmt.Errorf("peers: no target port configured for %q", raftAddr)
	}

	host, _, err := net.SplitHostPort(raftAddr)
	if err != nil {
		return "", fmt.Errorf("peers: %q is not a host:port raft address: %w",
			raftAddr, err)
	}
	if host == "" {
		return "", fmt.Errorf("peers: %q has no host part", raftAddr)
	}

	// net.JoinHostPort, not host+":"+port: an IPv6 host has to come back
	// bracketed, and SplitHostPort strips those brackets.
	return net.JoinHostPort(host, port), nil
}
