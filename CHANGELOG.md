# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [1.0.0] — 2026-07-28

First release with guarantees worth stating. The pre-1.0 code replicated writes
and served reads; what it did not do was survive a crash, report a failure, bound
its log, accept a write on a follower, or refuse an unauthenticated peer. Every
section below is one of those gaps closed, in the order they were closed, and each
one is described in `docs/phases/`.

The honest summary of what changed: the store went from "works when nothing goes
wrong" to "states what it guarantees, and is tested against the cases where it
does not".

### Added

**Durability**
- Write-ahead log (`kv.wal`): every command is appended and **`fsync`ed before**
  the in-memory map changes, so no acknowledged write can be missing from disk.
- Binary base file (`kv.db`, `KVB1` format) written only through a temp file +
  `fsync` + `rename` + directory `fsync`, so a reader never sees a partial file.
- Prefix-consistent recovery: a torn WAL tail truncates at the damage and stops
  rather than skipping past a record it could not read.
- WAL compaction with configurable thresholds; new base file written *before* the
  WAL is truncated.

**Snapshots**
- Real `FileSnapshotStore` replacing the discard store, with streaming
  `GetSnapshot` / `RestoreSnapshot` RPCs between the sidecar and the engine.
- Log truncation via `TrailingLogs`, plus `SnapshotInterval` and
  `SnapshotThreshold` as flags.
- A wiped follower now rejoins by snapshot transfer instead of replaying all
  history.

**Client usability**
- **Write forwarding**: a write to a follower is relayed to the leader instead of
  refused, with a one-hop guard (`Command.forwarded`) so two nodes cannot bounce it
  between themselves.
- **Linearizable reads**: `?consistency=linearizable` forwards to the leader and
  answers after a `Barrier` and a quorum `VerifyLeader`.
- REST routes: `PUT`/`GET`/`DELETE /kv/{key}` alongside the original
  `/insert-val` + `/get-val`. `/kv/{key}` percent-decodes; the legacy routes
  deliberately do not.
- `/join` and `/remove` accepted by any node and relayed to the leader.
- Multi-threaded HTTP server (worker pool), request size caps (413/431), and
  header parsing that reads until the terminator instead of assuming one 4 KiB read.

**Operability**
- Prometheus `/metrics` on both processes, including raft gauges evaluated at
  scrape time.
- Structured JSON logs from both processes with a shared schema (`ts`, `level`,
  `node_id`, `component`, `msg`), so one `jq` expression filters the whole cluster.
- `/health` (liveness) and `/ready` (readiness: raft state, leader known, backend
  reachable) as distinct endpoints, with a missing probe reported as a failed check
  rather than silently passing.
- Graceful shutdown: signals forwarded to both children, leadership transferred
  before exit so a planned restart does not cost an election, in-flight requests
  drained.

**Security**
- Bearer-token auth on `/join` and `/remove`. An unset token **disables** them
  (403) rather than leaving them open.
- Mutual TLS between raft peers over a TLS `raft.StreamLayer`, verified against one
  cluster CA.
- Optional TLS on the management listener; the joiner and the join/remove relay
  follow the scheme and refuse to fall back to plaintext.
- TLS for clients via a terminating reverse proxy (`docker-compose.secure.yml` +
  Caddy), with the plaintext proxy-to-node hop documented rather than glossed.
- `scripts/gen-certs.sh` for development certificates, labelled as not a CA.
- The C++ `StateMachine` gRPC now binds `127.0.0.1` instead of `0.0.0.0`.
- libFuzzer targets for both input boundaries, `gosec` in CI, and a ThreadSanitizer
  job.

**Proof**
- Chaos harness (`tests/chaos/`): journaling load generator plus fault injection
  (kill the leader, kill a follower, freeze a node, wipe a follower's disk) with an
  invariant checker that verifies every acknowledged write survived and all replicas
  converged — and that proves it can fail before it is trusted. Nightly in CI.
- Benchmark client (`bench/cmd/kvbench`) and `docs/benchmarks.md`, explicitly
  labelled as local laptop measurements with their run-to-run variance stated.
- Three test layers gated by CI: Go unit tests, C++ GoogleTest (also under
  ASan/UBSan and TSan), and an end-to-end pytest suite; plus opt-in crash, snapshot
  and TLS-profile suites.
- `docs/architecture.md`, including a section on what the design is bad at.

### Changed

- **HTTP status codes are now truthful.** The server previously answered `200` with
  an `{"error": ...}` body for every failure, including a write to a follower. It
  now returns 404 on a miss, 503 (naming the leader) when it cannot serve a write,
  502 on a genuine failure, and 400/415 on a malformed request. The error string
  carries `not_leader:` / `unavailable:` prefixes that are wire contracts.
- **Keys and values are byte-transparent.** The old line-based `kv.db` lost data:
  a key containing `=` reloaded as a *different key*, and a key containing a newline
  made reload invent a key that was never written while losing the one that was.
- A failed apply is reported to the client instead of being swallowed.
- `raft-boltdb` → `raft-boltdb/v2` (bbolt). v1 pulls the unmaintained
  `boltdb/bolt`, which trips Go's `checkptr` and therefore dies under `-race`.
- Malformed MsgPack now returns gRPC `OK` with `success=false` rather than a
  non-OK status. A non-OK status discards the response message, so the reason never
  reached the sidecar — and it routed a routine bad payload into the FSM's
  "this replica may be diverged" branch, raising false alarms on all three nodes.

### Fixed

- **A 17-byte remote denial of service, found by fuzzing.** A chain of MsgPack
  map32 markers made the decoder allocate for a declared size before discovering
  the bytes were absent: 17 bytes requested over 512 MiB. Worse than a slow
  request, because decoding happens at *apply* time on an entry raft has already
  committed, and committed entries replay on restart — one write took down all three
  nodes and kept doing so. Fixed with an explicit `msgpack::unpack_limit`.
- **`Content-Length: -1` killed a node.** `std::stoi("-1")` does not throw, so the
  cast to `size_t` made the body loop unsatisfiable and wedged the single-threaded
  accept loop. Reproduced against a live cluster, where the node reported itself
  healthy while serving nothing.
- **Peer TLS verified against the wrong name.** The bootstrap node advertised its
  *resolved container IP*, so a certificate issued for `node1` did not cover
  `172.18.0.2`. The cluster formed, replicated and passed every smoke test — and
  broke permanently after the first election, because the address was in the
  committed raft configuration.
- **`-bootstrap true` silently dropped every later flag.** Go's flag package treats
  a bare `true` as the first positional argument and stops parsing, so the bootstrap
  node ran production snapshot settings while its command line said otherwise.
- **The "secure" compose profile still published the plaintext client port.**
  `ports` concatenates across compose files rather than replacing, so `!override` is
  required. A plaintext door next to the locked one.
- Snapshot capture buffers the state eagerly instead of streaming from the live
  store, which would have produced a snapshot labelled index N containing state
  from index N+k. Idempotent replay would have hidden it.
- `Persist()` cancels the sink on error instead of closing it; a closed
  half-written snapshot is one raft will later try to restore.
- Config validation rejects half-specified TLS material at startup instead of
  reading it as "TLS is off" and serving plaintext on a port the operator believes
  is encrypted.

### Testing notes

Three assertions were found to be **incapable of failing** and were repaired:

- The Phase 3 snapshot scenarios parsed a startup log line that Phase 5 reformatted,
  and the fixture treated an unparseable line as "unsupported environment". They
  skipped silently — in CI too — for two phases. That branch now fails.
- With them running again, the replay assertion turned out to be vacuous: the
  successful-apply log line had moved to DEBUG, so the count was always zero. It now
  reads a Prometheus counter.
- The first chaos run verified **one** key against 79 indeterminate ones and printed
  PASSED, because a single lost response poisoned a key permanently. Poisoned keys
  are now reclaimed once no in-flight write can still commit, raising coverage to
  ~300 keys per run.

[Unreleased]: https://github.com/burhankapadia18/RaftKV/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/burhankapadia18/RaftKV/releases/tag/v1.0.0
