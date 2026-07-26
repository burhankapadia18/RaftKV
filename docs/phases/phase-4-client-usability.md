# Phase 4 — Client Usability

## Spec

### Goals
Any node serves any request: writes are transparently forwarded to the leader, reads
offer explicit consistency modes, the HTTP API becomes a real REST surface, and the
C++ server handles concurrent clients. This is the largest phase; it ships in four
independently verifiable slices (write forwarding → reads → HTTP API → concurrency).

### Non-goals
- No client-side cluster discovery/SDK (clients may still talk to one node).
- No multi-hop forwarding chains — exactly one hop, then a truthful error.
- No auth/TLS (Phase 6).

### Requirements

**Slice A — write forwarding (Go):**
- R4.1 `Command` gains `bool forwarded = 5` (new tag). `rpc.Server.Propose` on a
  non-leader forwards the request once to the leader's RaftNode gRPC and relays the
  response. A request already marked `forwarded` that lands on a non-leader returns
  the Phase 1 `not_leader:` error (loop guard).
- R4.2 Leader address mapping: raft advertises `host:8088`; the RaftNode gRPC lives at
  `host:50052`. Mapping is by convention — same host, sidecar gRPC port from config —
  with the port exposed as a flag (`-peer-rpc-port`, default 50052) so it stays honest
  in non-default deployments.
- R4.3 Forwarding uses a short-lived-per-leader cached connection with the existing
  retry/backoff idiom (`backend.ConnectionConfig` pattern) and a 5s deadline matching
  the C++ client's.

**Slice B — read consistency:**
- R4.4 `GET /kv/{key}?consistency=local|linearizable`; default `local` (served from
  the local store, documented as possibly stale — current behavior, now explicit).
- R4.5 Linearizable reads are forwarded to the leader and verified there:
  new RPCs — `RaftNode.Read(ReadRequest{key}) → ReadResponse{found, value, error}`
  and `StateMachine.Get(GetRequest{key}) → GetResponse{found, value}`.
  Flow: C++ handler → local sidecar `Read` → (forward to leader sidecar if follower) →
  leader runs `raft.Barrier(timeout)` + `VerifyLeader`, then reads from **its local C++
  store** via `StateMachine.Get`. Barrier-then-local-read on the verified leader is
  linearizable; same one-hop loop guard as writes.

**Slice C — HTTP API rework (C++):**
- R4.6 New routes: `PUT /kv/{key}` (body = value), `GET /kv/{key}`, `DELETE /kv/{key}`.
  Path keys are URL-decoded (new `url_decode` in `http_request.hpp`). Content
  negotiation: request `Content-Type` msgpack or JSON or raw bytes for PUT values;
  responses JSON by default, msgpack when `Accept: application/msgpack`.
- R4.7 The wire format through raft is **unchanged**: the handler builds the same
  msgpack `{op,key,value}` map that `/insert-val` receives today, so old log entries
  and new ones are identical on the apply path.
- R4.8 Legacy `/insert-val` and `/get-val` remain as deprecated aliases for one
  release (README marks them deprecated); `test_client.py`/e2e move to the new API.
- R4.9 Request hardening: max body size (default 1 MB, in `Config`), max header size,
  read loop that doesn't assume headers arrive in the first 4 KB packet.

**Slice D — concurrent HTTP server (C++):**
- R4.10 `network/thread_pool.hpp`: fixed pool (default `std::thread::hardware_concurrency()`,
  configurable), accept loop dispatches connections to workers. Graceful stop: a stop
  flag + closing the listen socket unblocks `accept`; workers drain and join.
- R4.11 Everything reachable from workers is thread-safe: `PersistentKVStore` already
  locks; `GrpcRaftClient` gets one gRPC channel shared across threads (channels are
  thread-safe) with per-call contexts. Verified under TSan in CI.

**Slice A′ — join forwarding (Go, small):**
- R4.12 `management` `/join` on a non-leader forwards the request to the leader's
  management endpoint (address derived like R4.2, management port flag) instead of
  failing `AddVoter`. Add `POST /remove?peerID=` → `RemoveServer`, same forwarding.

### Acceptance criteria
- E2E: SET via **each** of the three nodes succeeds and replicates; linearizable GET
  via a follower returns the just-written value with no sleep; local GET documented-stale
  behavior pinned.
- Failover e2e: continuous writes against a fixed follower while the leader is killed —
  after re-election, writes succeed again with no client-side redirect logic; no
  acknowledged write lost.
- Concurrency e2e: 50 parallel clients mixing PUT/GET against one node — no errors,
  no TSan findings.
- `curl -X PUT http://localhost:8081/kv/hello%20world -d 'v'` works from a follower.
- Smoke test and all prior-phase tests green (with R0.4's follower-write-fails pin
  replaced by follower-write-succeeds).

## Plan

1. **Proto**: add `forwarded` to `Command`, `Read`/`Get` RPCs + messages; regenerate
   Go stubs; commit. (One proto change for the whole phase.)
2. **Slice A**: leader-forwarding in `internal/rpc/server.go` + peer-address resolver
   (new `internal/peers` package, unit-tested); flags in `internal/config`; fsm/rpc
   tests for the loop guard. E2E: write-via-follower now asserts success.
3. **Slice A′**: join/remove forwarding in `internal/management` (reuses `internal/peers`);
   handler tests with fakes.
4. **Slice B**: `Read` handler in `internal/rpc` (Barrier + VerifyLeader + local
   `StateMachine.Get`); `Get` implementation in C++ `state_machine.hpp` (trivial store
   lookup); C++ handler plumbs `consistency=` to a new `IRaftClient::read` method.
   E2E: follower linearizable read with zero sleep.
5. **Slice C**: `url_decode` + route matching for `/kv/{key}` in `http_request.hpp`;
   handler methods + content negotiation in `http_server.hpp`; body/header caps (R4.9);
   port `tests/e2e` and `test_client.py` to the new API; README API section rewritten.
6. **Slice D**: `thread_pool.hpp` + accept-dispatch + graceful stop in `http_server.hpp`;
   shared-channel refactor in `raft_client.hpp`; TSan job added to CI; concurrency e2e test.
7. **Docs**: CLAUDE.md known-limitations — remove "writes to a follower fail" and
   "single-threaded HTTP server"; update the architecture data-path description.

### Verification
- Per slice: unit tests + targeted e2e before starting the next slice.
- Phase exit: full e2e (including `test_failover.py` with kill-leader-under-load),
  `/cluster-smoke-test`, TSan CI job green.
