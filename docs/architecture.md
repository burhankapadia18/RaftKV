# RaftKV architecture

The design as it stands at v1.0, including the parts that are compromises. If you
are looking for how to run it, start with the [README](../README.md); this
document is for someone who needs to change it.

## Contents

- [The sidecar split](#the-sidecar-split)
- [The two data paths](#the-two-data-paths)
- [Consistency modes](#consistency-modes)
- [Durability and on-disk state](#durability-and-on-disk-state)
- [Snapshot lifecycle](#snapshot-lifecycle)
- [Cluster membership](#cluster-membership)
- [Failure handling and error contract](#failure-handling-and-error-contract)
- [Security model](#security-model)
- [Ports](#ports)
- [What this design is bad at](#what-this-design-is-bad-at)

## The sidecar split

Each node is **two processes**, not one:

```
┌─ node1 ──────────────────────────────────────────────┐
│                                                      │
│   kvdb_node (C++17)              sidecar (Go)         │
│   ┌────────────────────┐         ┌─────────────────┐  │
│   │ HTTP API     :8080 │         │ RaftNode  :50052│  │
│   │ StateMachine :50051│◀────────│ (Propose, Read) │  │
│   │                    │  Apply  │                 │  │
│   │ PersistentKVStore  │────────▶│ hashicorp/raft  │  │
│   │  kv.wal / kv.db    │ Propose │  logs.dat       │  │
│   └────────────────────┘         │ Mgmt HTTP :6000 │  │
│                                  │ Raft TCP  :8088 │◀─┼──▶ peers
│                                  └─────────────────┘  │
└──────────────────────────────────────────────────────┘
```

- **`cpp-app/` — the storage engine.** Header-only C++17: HTTP API, in-memory map
  with a write-ahead log and an atomically-rewritten base file, and a gRPC
  `StateMachine` server. Binary: `kvdb_node`.
- **`go-sidecar/` — consensus.** Wraps `hashicorp/raft`: elections, replication,
  membership, snapshots. Module: `my-raft-sidecar`.
- **`proto/consensus.proto` — the only interface between them.** Both directions
  over localhost gRPC.

**Why split at all.** Raft is a solved problem with a good Go implementation and no
comparably maintained C++ one; the storage engine wanted C++. The cost of the split
is two gRPC hops on the write path and two processes to supervise per node. The
benefit is that neither half reimplements the other's hard part.

The seam is narrow on purpose: the sidecar knows nothing about keys, values or
MsgPack. It moves opaque bytes and calls `Apply`.

## The two data paths

Understanding writes versus reads is most of understanding this codebase.

### Write path — through consensus

1. Client sends `PUT /kv/{key}` (or `POST /insert-val` with a MsgPack body) to any
   node's HTTP server (`cpp-app/src/network/http_server.hpp`).
2. C++ forwards the **raw MsgPack bytes opaquely** in `Command.data` via
   `GrpcRaftClient::propose()` → the sidecar's `RaftNode.Propose` on :50052.
3. If this node is not the leader, the sidecar **forwards the proposal to the
   leader** over the peers' :50052 (Phase 4), carrying `Command.forwarded=true` so
   the hop can happen at most once.
4. The leader calls `raft.Apply()`; the entry is replicated to a quorum.
5. Once committed, **every** node's `CppFSM.Apply` calls its own local C++
   `StateMachine.Apply` on :50051.
6. C++ **only now** deserializes the MsgPack into a `KVCommand`, validates it, and
   applies SET/DELETE to `PersistentKVStore` — which appends to `kv.wal` and
   **fsyncs before** the in-memory map changes.

The payload is parsed exactly once, at apply time, on each replica. The proto's
`op`/`key`/`value` fields exist and are unused in transit.

**Consequence worth internalising:** validation happens *after* commit. A command
with an unknown op or an empty key is replicated first and rejected at apply time,
costing a raft log entry and returning 502 to the client. Moving validation to the
propose boundary would mean the HTTP layer parsing the MsgPack it currently
forwards blind — an architectural change, not a tweak.

### Read path — no consensus by default

`GET /kv/{key}` is served straight from the local in-memory map. No raft
involvement, no network. Reads on a follower can be stale.

## Consistency modes

| Request | Path | Guarantee |
|---|---|---|
| `GET /kv/{key}` | Local map read | May be stale; no bound on how stale |
| `GET /kv/{key}?consistency=linearizable` | Forwarded to leader → `Barrier` → `VerifyLeader` → local read | Reflects every write acknowledged before the request |

The linearizable path is three steps and each one is load-bearing:

1. **Forward to the leader** if this node is not it. A follower cannot answer
   linearizably at all.
2. **`Barrier`** — proposes a no-op and waits for it to apply, which guarantees
   every previously-committed entry has been applied to *this* state machine.
3. **`VerifyLeader`** — confirms with a quorum that this node still leads. Without
   it, a partitioned old leader would happily serve a stale read from a state it
   believes is current. `IsLeader()` reflects only this node's own belief and is not
   sufficient.

Measured cost of that guarantee: about 7× versus a local read
([benchmarks](benchmarks.md)).

## Durability and on-disk state

`PersistentKVStore` keeps two files, and the split *is* the durability argument.

### `kv.wal` — write-ahead log

Records of `uint32 len | msgpack KVCommand | uint32 crc32`. `set()`/`remove()`
append one record and **`fsync` it before mutating the map**. There is therefore no
instant at which a client has been told a write succeeded while the bytes are not
on disk. It also makes an apply O(entry) rather than O(store).

### `kv.db` — base file (`KVB1`)

A whole-map snapshot: `"KVB1" | uint32 count | count × (uint32 klen | key | uint32
vlen | value)`, little-endian, nothing escaped — so keys and values are
byte-transparent, including `=`, newlines and NUL. Written **only** by compaction,
and only through `atomic_write_file()`: temp file → `fsync` → `rename` → `fsync`
the directory. A reader never sees a half-written base file.

### Recovery

Load `kv.db`, then replay `kv.wal` over it. Recovery is **prefix-consistent**: a
torn tail — a short record, a length that overruns, a bad CRC, or a record that
frames correctly but does not decode to a valid command — truncates the WAL at that
point and stops. Everything before it is applied; nothing after.

That "and stops" is deliberate and not a convenience. Skipping a bad record and
continuing would reconstruct a state the replica never had, because the record you
could not read may have been a DELETE.

### Compaction

When the WAL passes `wal_max_bytes` / `wal_max_records`, compaction runs
synchronously inside the store lock: **new base file first, then truncate the WAL**.
A crash between the two costs a re-replay of records already in the base file, and
SET/DELETE replay is idempotent. The reverse order loses data.

### Trust boundaries

Every byte read off disk or off a socket is untrusted. A length prefix is
bounds-checked against the bytes that actually remain before it is used to index,
advance or `reserve`; MsgPack is decoded under an explicit `unpack_limit`. Both
paths are fuzzed and run under ASan/UBSan in CI — because the alternative was
demonstrated: a 17-byte payload once made a node try to allocate over 512 MiB, and
since decoding happens after commit, it took down all three replicas and did it
again on every restart.

## Snapshot lifecycle

Without snapshots the raft log grows forever and every restart replays all of
history. Phase 3 wired a real `FileSnapshotStore`.

**Capture.** Raft calls `CppFSM.Snapshot()` on the FSM goroutine with no `Apply` in
flight. It **eagerly streams the whole store out of C++ and buffers it**, then
`Persist()` writes that buffer to the sink — possibly much later, concurrently with
subsequent applies.

The eagerness is load-bearing. Streaming from the live store inside `Persist()`
would produce a snapshot *labelled* index N whose contents are the state at some
later index N+k. Because this store is idempotent the cluster would re-converge on
replay, so the bug would pass every test while being wrong in general. The cost is
holding the store in memory during a snapshot: a known scaling limit, not an
oversight.

`Persist()` calls `sink.Cancel()` on any error, never `Close()` — a half-written
snapshot that was closed is a corrupt snapshot raft will later try to restore.

**Restore.** Raft loads the newest snapshot *before* replaying anything, and
`CppFSM.Restore()` pushes it into C++ via the streaming `RestoreSnapshot` RPC. The
C++ side decodes the whole payload before touching the store, so a corrupt snapshot
leaves the previous state intact — half-restoring is the worst available outcome.
`Restore()` returns an error when C++ reports failure: a node that cannot restore
must not serve, and returning nil would leave it claiming state it does not have.

**Truncation.** `TrailingLogs` is what actually bounds the log. Raft truncates to
`snapshot_index - TrailingLogs`, so leaving the default (10240) means the log never
shrinks however often the node snapshots. All three tunables
(`SnapshotInterval`, `SnapshotThreshold`, `TrailingLogs`) are flags because the e2e
suite has to drive them far below production values to observe a snapshot at all.

## Cluster membership

One node bootstraps (`BOOTSTRAP=true`); the others retry-join through the
management API. `/join` and `/remove` are leader-only raft operations, but may be
sent to **any** node: a follower relays to the leader, marking the request with
`X-RaftKV-Forwarded` so it can be relayed at most once. Two nodes each believing
the other leads would otherwise bounce a join between themselves until something
timed out.

A node's Raft address, its RaftNode gRPC and its management API all live on the same
host at different ports; `internal/peers` is the single place that knows the
mapping, and it is a flag rather than a constant so a non-default deployment does
not forward into a black hole.

## Failure handling and error contract

Every failure on the write path is reported truthfully back down it:
`StateMachine.Apply` fills `ApplyResponse.error` → `CppFSM.Apply` returns an
`*fsm.ApplyError` (never nil) → `rpc.Server.Propose` surfaces it in
`ProposeResponse.error` → `GrpcRaftClient::propose` returns a `ProposeResult` →
`KVHttpHandler` maps it onto a status code and a JSON body.

Two prefixes on that error string are **wire contracts**, not log messages:

| Prefix | HTTP | Meaning |
|---|---|---|
| `not_leader:` | 503 | Retry; the leader is named |
| `unavailable:` | 503 | Retryable — no leader yet, an election in progress |
| *(anything else)* | 502 | A real failure; do not retry blindly |

Phase 4's forwarding is built on `not_leader:`, so any other failure must not carry
it. The full status/body table is in the [README](../README.md#api-reference) and
mirrored as constants in `tests/e2e/contracts.py`; the handler and both of those
change together.

### Writes are at-least-once under failure

Neither 502 nor 503 tells a client whether the write was applied, and 503 is the
surprising one. `rpc.Server.Propose` returns `unavailable:` — which the handler maps
to 503 — when *forwarding* to the leader fails as a transport matter. The leader may
already have received and committed the entry before that connection broke. The
client sees a retryable failure for a write that succeeded.

There are no idempotency tokens and no request deduplication, so this is inherent
rather than a bug: a client that retries gets at-least-once semantics. `PUT` and
`DELETE` are idempotent, so a retry is harmless. Anything built on top that needs
exactly-once, or a read-modify-write, needs a compare-and-set primitive this store
does not provide.

Found by the chaos harness, whose load generator initially encoded the intuitive
reading of the contract ("503 means the key is untouched") and reported the store
holding a value fifteen writes newer than the last one it had been told about.

One asymmetry to know about: `CppFSM.Apply` treats a failed apply as a
*deterministic* rejection. That is accurate for a malformed command — every replica
decodes the same bytes and reaches the same verdict — and **not** accurate for a
local I/O error, where this node really can diverge from its peers. Nothing
currently depends on that branch; do not build on it assuming determinism.

## Security model

Opt-in by configuration. The default compose profile is plaintext — a demo needing
a CA before it prints anything is a demo nobody runs — and
`docker-compose.secure.yml` is the documented deployment.

| Surface | Default | Secure profile |
|---|---|---|
| Client HTTP API | Plaintext, unauthenticated | HTTPS via a reverse proxy, **still unauthenticated** |
| Management API | HTTP; `/join`+`/remove` behind a bearer token | HTTPS + bearer token |
| Raft peer transport | Plaintext — anyone who can reach it can append entries | Mutual TLS against one cluster CA |
| C++ StateMachine gRPC | Plaintext on `127.0.0.1` | Same |
| Sidecar RaftNode gRPC | Plaintext, peer-reachable | Same |

Design points worth keeping:

- **`internal/tlsconfig` is the only place a `*tls.Config` is built.** A server
  with a cert and key but no `ClientCAs` completes a handshake with anybody, looks
  like mutual TLS, and authenticates nobody. Centralised, that mistake can only be
  made once.
- **The raft transport is mutually authenticated, not configurably so.** One-way
  TLS there would encrypt the traffic and still let anyone append entries.
- **The management listener does *not* require a client certificate.** Health probes
  and Prometheus hold none; they are authenticated by the bearer token instead.
- **A misconfigured TLS surface fails startup rather than falling back.** A join or
  a relay that quietly degraded to HTTP would put the cluster-admin token on the
  wire in clear.
- **An empty token disables `/join` and `/remove`** (403) rather than opening them. A
  cluster that forgets to set one cannot grow, which is a much better failure than
  one anybody can join.
- **The TLS raft transport advertises a hostname; the plaintext one advertises a
  resolved IP.** Forced, not sloppy: `raft.NewTCPTransport` demands a `*net.TCPAddr`
  with a concrete IP, but a peer verifies the certificate against the address it
  dialled, and a certificate for `node1` does not cover `172.18.0.2`. See
  `hostPortAddr`.

What is **not** protected is in [the README's Security section](../README.md#security)
and in CLAUDE.md's limitations, at the same level of detail as what is.

## Ports

| Port | Process | Purpose | Exposure |
|---|---|---|---|
| 8080 | C++ | Client HTTP API | Published (default profile) |
| 8443 | proxy | Client HTTPS | Published (secure profile) |
| 6000 | Go | Management API: `/join`, `/remove`, `/status`, `/health`, `/ready`, `/metrics` | Published |
| 8088 | Go | Raft peer transport | Cluster-internal |
| 50051 | C++ | `StateMachine` gRPC (Apply, snapshots, Get) | **`127.0.0.1` only** |
| 50052 | Go | `RaftNode` gRPC (Propose, Read) | Cluster-internal, peer-reachable |

Startup order matters: the C++ app must be listening before the sidecar connects.
`entrypoint.sh` probes 50051 in a bounded loop, and `backend.Connect` additionally
waits for the gRPC channel to reach `Ready`.

## What this design is bad at

Stated here rather than left to be discovered:

- **One raft group.** Every write goes through one leader and one log. There is no
  sharding, so write throughput has a ceiling that adding nodes makes *worse*, not
  better — more followers to wait for. This is the fundamental scalability limit.
- **The whole dataset lives in memory,** and a snapshot holds a second copy while
  it is being written. Dataset size is bounded by RAM.
- **No client authentication or authorization.** Anyone who can reach the API can
  read and write everything.
- **No client SDK.** Clients speak HTTP and MsgPack directly, and must implement
  their own retry policy around the 502/503 distinction.
- **Reads are stale unless asked otherwise,** and there is no bounded-staleness
  option between "local" and "full barrier".
- **No keep-alive on the HTTP server.** Every request pays a TCP handshake; a
  client must not pool connections.
- **TLS for clients is proxy-terminated,** so the proxy-to-node hop is plaintext
  and the proxy belongs on the same host as the node.
- **`50052` is peer-reachable and unauthenticated.** A peer that can reach it can
  propose writes. It is inside the same trust boundary as the raft port but,
  unlike the raft port, has no credential.
- **Writes are at-least-once under failure, with no way to detect a duplicate.**
  See above. There is no compare-and-set, so a client cannot build exactly-once
  semantics on top of this.
