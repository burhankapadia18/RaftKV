# RaftKV Roadmap

## Where the project is today

RaftKV works as a demo: a 3-node docker-compose cluster where a C++ storage engine
(HTTP API + in-memory KV store + file persistence) pairs with a Go sidecar running
HashiCorp Raft. Writes flow through consensus (HTTP → Propose → raft log → Apply on
every node); reads are served locally. The codebase is small (~1k lines of Go,
header-only C++), cleanly layered, and dependency-injected — it was built to be
extended.

What it is **not** yet: a system you could trust with data or run unattended.
The gaps, confirmed by code review:

- ✅ **Tests and CI exist** (Phase 0, complete; extended by Phase 1). Three
  layers: Go unit tests covering `internal/fsm`, `management`, `cluster`,
  `config`, and — since Phase 1 — `backend` and `rpc` (live coverage figures
  come from the CI summary, and a Phase 0 snapshot is in the Phase 0 doc;
  `internal/raftnode` and `cmd/sidecar` remain untested); C++ GoogleTest cases
  under CTest, also run under ASan/UBSan, covering `KVCommand`,
  `PersistentKVStore`, `HttpRequestParser`, `StateMachineService::Apply` and
  `KVHttpHandler`; and an asserting `tests/e2e` pytest suite that proves a
  leader write replicates to **all three** nodes and pins the whole HTTP
  status-code contract. `.github/workflows/ci.yml` gates gofmt, `go vet`,
  `go test -race`, clang-format, the C++ build and ctest, and the full
  docker-compose e2e run. Everything below is now change-detected: the suites
  **pin** the current wrong behavior, so each fix must flip its own tests.
  (`test_client.py` survives only as a manual demo — it still asserts nothing.)
- ✅ **Errors are truthful** (Phase 1, complete). Every failure now reports what
  actually happened, end to end: `CppFSM.Apply` returns a typed `*fsm.ApplyError`
  instead of silently diverging a node from the raft log; `StateMachine.Apply`
  validates the decoded command and fills `ApplyResponse.error`;
  `rpc.Server.Propose` tags not-the-leader with a stable `not_leader:<addr>`
  prefix; `GrpcRaftClient::propose` returns a `ProposeResult{success, error}`
  rather than a bare bool; and the HTTP layer answers with real status codes,
  correct reason phrases and JSON error bodies (200/400/404/415/502/503).
  `backend.Connect` waits for the channel to reach `Ready`, so its retry loop is
  no longer a no-op, and `entrypoint.sh` probes the C++ gRPC port instead of
  sleeping. A malformed `Content-Length` no longer crashes the process — that
  was a remote DoS, and there is now an e2e regression test for it.
- **No durability.** `kv.db` is rewritten wholesale on every write with no fsync
  and no atomic rename; a crash mid-write corrupts the store.
- **No snapshots.** `DiscardSnapshotStore` + `DummySnapshot` mean the raft log
  grows forever, restarts replay the entire history, and a lagging follower can
  never catch up via snapshot transfer.
- **Unusable from a client's perspective.** Writes to a follower are rejected
  rather than forwarded — the 503 does now name the leader, but it names its
  *Raft* address (`node1:8088`), which is not something a client can dial.
  Follower reads are silently stale with no linearizable option. Command
  validation happens after the entry is committed, so a bad command still costs
  a raft log entry. The HTTP server is a single-threaded blocking loop with a
  4KB first read, no URL decoding and no body-size limit.
- **No operability or security.** Unstructured logs, no metrics, `/health` always
  says OK, no signal propagation, no TLS or auth anywhere
  (anyone who can reach :6000 can join a voter into the cluster).

## End-goal: what "finished" looks like

**RaftKV 1.0 is a small, honest, production-shaped distributed KV store — the
reference implementation of the C++-engine / Go-consensus sidecar pattern.**
Not a Redis competitor; a system that makes real guarantees and proves them.

Acceptance criteria for 1.0:

1. **Correct** — every committed raft entry is applied or the node loudly halts;
   invalid commands are rejected at the boundary; a crash at any instant loses no
   acknowledged write (WAL/atomic persistence + fsync).
2. **Complete Raft lifecycle** — snapshots + log compaction + follower restore
   work; a node can be wiped, rejoin, and catch up from a snapshot.
3. **Usable** — any node accepts any request: writes are forwarded to the leader,
   reads offer `local` (fast, maybe stale) and `linearizable` (leader-verified)
   modes; the HTTP API uses real methods, real status codes, and structured
   error bodies.
4. **Operable** — structured logs, Prometheus metrics from both processes,
   truthful health endpoints, graceful shutdown with leadership transfer,
   docker-compose healthchecks and restart policies.
5. **Secured** — TLS/auth available on every non-localhost surface; the join
   endpoint is authenticated; inputs are bounded and fuzz-tested.
6. **Proven** — unit tests (≥80% on Go internal packages and C++ core logic),
   an asserting e2e suite including kill-the-leader failover, a chaos script,
   benchmarks with published numbers, and CI that gates all of it.

The sidecar split (C++ storage / Go consensus) stays — it is the identity of the
project. `main.cpp` stays bootstrap-only; new C++ logic goes in domain headers;
new Go logic goes in `internal/` packages with constructors.

---

## The plan, step by step

Ordered by dependency: safety net first, then truth-telling, then the two big
subsystems (durability, snapshots), then client-facing behavior, then ops,
security, and release. Each phase ends green: build + unit tests + cluster
smoke test pass before the next phase starts.

Each phase has a full spec (goals, non-goals, numbered requirements, acceptance
criteria) and implementation plan in `docs/phases/`:

| Phase | Status | Spec & plan |
|---|---|---|
| 0 — Tests and CI | ✅ Complete | [docs/phases/phase-0-tests-and-ci.md](docs/phases/phase-0-tests-and-ci.md) |
| 1 — Truthful errors | ✅ Complete | [docs/phases/phase-1-truthful-errors.md](docs/phases/phase-1-truthful-errors.md) |
| 2 — Durability | Not started | [docs/phases/phase-2-durability.md](docs/phases/phase-2-durability.md) |
| 3 — Snapshots & compaction | Not started | [docs/phases/phase-3-snapshots.md](docs/phases/phase-3-snapshots.md) |
| 4 — Client usability | Not started | [docs/phases/phase-4-client-usability.md](docs/phases/phase-4-client-usability.md) |
| 5 — Operability | Not started | [docs/phases/phase-5-operability.md](docs/phases/phase-5-operability.md) |
| 6 — Security | Not started | [docs/phases/phase-6-security.md](docs/phases/phase-6-security.md) |
| 7 — Proof & release | Not started | [docs/phases/phase-7-proof-and-release.md](docs/phases/phase-7-proof-and-release.md) |

The sections below are the condensed version; the phase docs are authoritative.

### Phase 0 — Safety net: tests and CI *(everything else depends on this)*

**Status: ✅ Complete** — see the Outcome section of
[docs/phases/phase-0-tests-and-ci.md](docs/phases/phase-0-tests-and-ci.md) for
verified results.

0.1 **Make the e2e test real.** Rewrite `test_client.py` (or replace with
`tests/e2e/`) to assert with exit codes: SET on leader → read back from **all
three** nodes (poll with timeout, not `sleep 0.3`); DELETE round-trip; missing
key; write-to-follower currently fails (pin the behavior, update the test when
Phase 4 adds forwarding).

0.2 **Go unit tests** — the seams already exist:
- `internal/fsm`: fake `StateMachineClient`; table-driven Apply cases.
- `internal/config`: flag parsing, `AdvertiseAddr`.
- `internal/cluster`: `Joiner` against `httptest.Server` (success, retry, exhaustion).
- `internal/management`: handler tests with a fake node interface.
Run with `go test -race ./...`.

0.3 **C++ unit tests.** Add a GoogleTest CTest target (separate from `kvdb_node`,
per `.claude/rules/testing.md`): `KVCommand::from_msgpack` (valid / malformed /
unknown op / missing fields), `PersistentKVStore` round-trip against a temp file
(including keys/values containing `=` and `\n` — pin the current fragile format
before Phase 2 replaces it), `HttpRequestParser` (query parsing, bad
Content-Length, oversized input).

0.4 **CI (GitHub Actions).** Jobs: Go (gofmt check, `go vet`, `go test -race`),
C++ (cmake build with `-Wall -Wextra -Wpedantic`, ctest, ASan/UBSan job),
e2e (docker build, compose up, run e2e suite, compose down). Add `.clang-format`
and run `gofmt`/`clang-format` checks.

**Exit:** CI green on main; e2e proves replication on all nodes.

### Phase 1 — Truthful errors *(small diffs, large trust gain)*

**Status: ✅ Complete** — see the Outcome section of
[docs/phases/phase-1-truthful-errors.md](docs/phases/phase-1-truthful-errors.md)
for verified results and for the decisions taken where the spec was silent.

1.1 **Fix the FSM contract.** `CppFSM.Apply` must check `ApplyResponse.success`;
on failure return an error object raft can surface — and log loudly. Add an
`error` field to `ApplyResponse` in `proto/consensus.proto` (new tag, backward
compatible) and regenerate Go stubs per `.claude/rules/protobuf.md`.

1.2 **Validate at the boundary.** `StateMachineService::Apply` calls
`KVCommand::is_valid()` before dispatch; invalid → `success=false` + error string.
Keep the existing try/catch for malformed msgpack.

1.3 **Fix HTTP truthfulness.** Correct reason phrases per status code; return
real codes: 404 missing key, 400 bad request/content-type, 502/503 failed
propose with a structured (JSON) error body carrying the propose error string.
`GrpcRaftClient::propose` must surface `reply.error()` instead of collapsing to
bool — change `IRaftClient` to return a small result struct.

1.4 **Fix startup rigor.** Replace deprecated `grpc.Dial` with `grpc.NewClient`
plus an explicit readiness wait (health ping or `WaitForStateChange`) so the
retry loop actually retries. Replace `sleep 2` in `entrypoint.sh` with a probe
loop against the C++ gRPC port.

1.5 Update README API docs + e2e tests for the new status codes.

**Exit:** no failure path returns 200/"ok"; a follower-write returns an error
that names the leader problem; e2e error-path tests pass.

### Phase 2 — Durability *(the store must survive crashes)*

2.1 **Atomic persistence.** Replace truncate-rewrite in `PersistentKVStore` with
write-to-temp + `fsync` + `rename`. Replace the line-based `key=value` format
with a length-prefixed binary format (kills the `=`/newline fragility pinned in
0.3; keep a one-shot migration read path for old files).

2.2 **Append-only WAL.** New `storage/wal.hpp`: append + fsync each applied
command; rebuild in-memory state from snapshot-file + WAL replay on startup;
compact the WAL after a successful full persist. This makes apply O(entry)
instead of O(store) and provides crash consistency. (Raft-side durability is
already BoltDB.)

2.3 **Crash tests.** C++ unit tests for recovery from a torn/partial WAL tail;
an e2e scenario: write, `docker kill` (not stop) a node, restart, verify state.

**Exit:** kill -9 at any point loses no acknowledged write on that node.

### Phase 3 — Snapshots and log compaction *(the biggest missing subsystem)*

This is the one item CLAUDE.md flags as "its own project" — it becomes one here.

3.1 **Proto:** add `StateMachine.Snapshot(stream)` (C++ streams full state) and
`StateMachine.Restore(stream)` RPCs. Regenerate Go stubs; C++ regenerates at build.

3.2 **C++:** implement snapshot export (consistent view under the store mutex —
copy-then-stream to keep the lock short) and restore (swap-in a fresh store,
then persist).

3.3 **Go:** replace `DummySnapshot` with a real `raft.FSMSnapshot` that pulls
the stream from C++ and writes to the sink; implement `FSM.Restore` by streaming
the snapshot back into C++. Replace `NewDiscardSnapshotStore()` with
`NewFileSnapshotStore(DataDir, ...)`; configure `SnapshotInterval`/`SnapshotThreshold`.

3.4 **Tests:** e2e scenario — write N entries past the snapshot threshold, wipe a
follower's volume, rejoin, verify it restores from snapshot and serves reads;
assert the BoltDB log actually compacts.

**Exit:** raft log bounded; wiped follower catches up via snapshot.

### Phase 4 — Client usability *(any node serves any request)*

4.1 **Leader forwarding for writes.** In the Go sidecar: when not leader,
forward the Propose to the leader's RaftNode gRPC (leader address from
`LeaderWithID`; map raft address → gRPC address via a small membership registry
or convention). Fallback: return the leader's client address so the C++ layer
can respond `307 Temporary Redirect`. Forwarding preferred — clients shouldn't
need retry logic.

4.2 **Read consistency modes.** `GET /kv/{key}?consistency=local|linearizable`.
`linearizable` = C++ asks the sidecar to run a raft `Barrier()`/read-index check
before answering (new small RPC on `RaftNode`). Default `local`, documented as
possibly stale.

4.3 **Modern HTTP API.** Move to `PUT/GET/DELETE /kv/{key}` with URL decoding,
body-size limits, and content negotiation (keep msgpack, add JSON). Keep the old
endpoints as deprecated aliases for one release. Update `test_client.py`, README,
and proto docs together (wire-format rule).

4.4 **Concurrent HTTP server.** Replace the single-threaded accept loop with a
fixed thread pool (`network/thread_pool.hpp`), robust request reading (loop on
headers, cap total size), and graceful shutdown via a stop flag. Store and gRPC
propose path are already thread-safe; verify with a concurrent e2e test + TSan job.

4.5 **`/join` correctness.** Management server forwards join requests to the
leader instead of failing `AddVoter` on followers; add a remove-node endpoint.

**Exit:** e2e writes and reads succeed against **any** node, including during a
leader failover (kill leader mid-load, assert no acknowledged write lost).

### Phase 5 — Operability

5.1 Structured JSON logging both sides (Go: `log/slog`; C++: a tiny
`common/log.hpp`), with node id + component fields.

5.2 Prometheus metrics: Go sidecar (`/metrics` on management port — apply
latency, raft state, log index, propose errors); C++ (request counts/latencies,
store size, WAL size).

5.3 Truthful health: `/health` distinguishes liveness from readiness (raft
initialized, backend reachable, applies flowing); `/status` uses `encoding/json`.

5.4 Lifecycle: `entrypoint.sh` traps SIGTERM and forwards to both processes;
Go performs `LeadershipTransfer` on shutdown; compose gets healthchecks,
`depends_on`, and `restart: unless-stopped`.

**Exit:** `docker compose stop` on the leader hands off leadership cleanly; a
Grafana-scrapeable metrics surface exists on every node.

### Phase 6 — Security

6.1 TLS + token auth on the management API (the `/join` hole closes here);
optional mTLS between raft peers (HashiCorp raft TLS transport) and on the
client HTTP API. Localhost-only gRPC pairs may stay plaintext but bind to
127.0.0.1 explicitly.

6.2 Input hardening: enforce the body-size caps everywhere, fuzz
`KVCommand::from_msgpack` and the HTTP parser (libFuzzer targets in CI),
`gosec` in the Go CI job.

**Exit:** security scan jobs green; unauthenticated `/join` rejected.

### Phase 7 — Proof and release

7.1 **Chaos script** (`tests/chaos/`): random leader kills, follower wipes,
network pauses (`docker pause`), under continuous client load; invariant:
no acknowledged write lost, cluster converges.

7.2 **Benchmarks** (`bench/`): throughput/latency for both consistency modes,
1 vs 3 nodes, published in the README with methodology.

7.3 **Docs and release**: architecture doc reflecting the final design, updated
README, CHANGELOG, tagged `v1.0.0` with multi-arch images pushed to a registry.

---

## Ground rules while executing

- **One phase per PR-sized chunk**, each verified with the cluster smoke test
  (`/cluster-smoke-test`) plus that phase's new tests. Never stack unverified phases.
- **Proto changes** always follow `.claude/rules/protobuf.md`: new tags only,
  Go stubs regenerated and committed, old raft log entries must stay decodable.
- **Known limitations in CLAUDE.md are being removed deliberately** — update
  CLAUDE.md's "Known Limitations" section as each one falls, so the docs never
  claim a limitation that's been fixed (or vice versa).
- Keep the architectural seams: `IKVStore`/`IRaftClient` in C++, small
  consumer-side interfaces in Go, header-only C++ domains, `main`s as pure wiring.

## Suggested sequencing at a glance

| Phase | Status | Theme | Rough size | Unblocks |
|---|---|---|---|---|
| 0 | ✅ Complete | Tests + CI | M | everything |
| 1 | ✅ Complete | Truthful errors | S–M | 2, 3, 4 |
| 2 | Not started | Durability (WAL, atomic persist) | M | 3 |
| 3 | Not started | Snapshots + compaction | L | 4 (wiped-node rejoin) |
| 4 | Not started | Leader forwarding, consistency, HTTP rework | L | 5, 7 |
| 5 | Not started | Logging, metrics, lifecycle | M | 7 |
| 6 | Not started | TLS/auth, fuzzing | M | release |
| 7 | Not started | Chaos, bench, v1.0 | M | — |
