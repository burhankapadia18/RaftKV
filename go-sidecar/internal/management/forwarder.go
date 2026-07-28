package management

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

// ForwardedHeader marks a management request that has already been relayed once.
//
// The loop guard for /join and /remove, and the exact analogue of
// Command.forwarded on the write path: two nodes that each believe the other
// leads would otherwise bounce a join between themselves until something timed
// out. A request carrying this header is answered by whichever node received
// it — with a truthful "not the leader" if that is the case — never relayed
// again.
const ForwardedHeader = "X-RaftKV-Forwarded"

// maxForwardedBody caps how much of a peer's response we read back.
//
// These endpoints answer with a short line or a JSON object; anything larger
// means the peer is not what we think it is, and reading it unbounded would let
// a misconfigured target dictate this node's memory use.
const maxForwardedBody = 64 << 10

// PeerResolver maps a peer's Raft address to its management HTTP address.
// *peers.Resolver satisfies it; declared here so this package does not depend
// on package peers, matching the consumer-side-interface convention already
// used by RaftControl and rpc.PeerResolver.
type PeerResolver interface {
	MgmtAddr(raftAddr string) (string, error)
}

// ForwardResult is a peer's answer, relayed verbatim to the original caller.
type ForwardResult struct {
	Status int
	Body   []byte
}

// Forwarder relays a management request to the leader on a follower's behalf.
// *HTTPForwarder satisfies it; kept as an interface so the handlers can be
// tested without a real peer.
type Forwarder interface {
	Forward(ctx context.Context, mgmtAddr, path, rawQuery string) (*ForwardResult, error)
}

// HTTPForwarder relays over plain HTTP to a peer's management port.
type HTTPForwarder struct {
	client *http.Client
}

// NewHTTPForwarder returns a Forwarder with a bounded per-request timeout.
//
// The timeout matters more than it looks: this call happens while the original
// client is still waiting on us, so an unbounded relay would turn one slow peer
// into a stalled request on every node that forwards to it.
func NewHTTPForwarder(timeout time.Duration) *HTTPForwarder {
	return &HTTPForwarder{client: &http.Client{Timeout: timeout}}
}

// Forward issues the relayed request and returns the peer's status and body.
func (f *HTTPForwarder) Forward(ctx context.Context, mgmtAddr, path, rawQuery string) (*ForwardResult, error) {
	target := url.URL{
		Scheme:   "http",
		Host:     mgmtAddr,
		Path:     path,
		RawQuery: rawQuery,
	}

	// POST, regardless of how the original arrived: /join accepts both verbs and
	// the relay is a state-changing call, so it should not look cacheable.
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, target.String(), nil)
	if err != nil {
		return nil, fmt.Errorf("building forwarded request to %s: %w", mgmtAddr, err)
	}
	req.Header.Set(ForwardedHeader, "1")

	resp, err := f.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("forwarding to %s: %w", mgmtAddr, err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, maxForwardedBody))
	if err != nil {
		return nil, fmt.Errorf("reading response from %s: %w", mgmtAddr, err)
	}

	return &ForwardResult{Status: resp.StatusCode, Body: body}, nil
}

// Ensure HTTPForwarder implements Forwarder at compile time.
var _ Forwarder = (*HTTPForwarder)(nil)
