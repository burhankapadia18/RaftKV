# Phase 3 — Snapshots and Log Compaction

## Spec

### Goals
Implement the full raft snapshot lifecycle so the log is bounded, restarts don't
replay all history, and a wiped or lagging follower catches up via snapshot transfer.
This removes the limitation CLAUDE.md flags as "its own project".

### Non-goals
- No incremental/streaming-diff snapshots — full-state snapshots are correct and
  sufficient at this scale.
- No change to the write path or the `Command.data` msgpack convention.

### Requirements

**Proto contract (`proto/consensus.proto`):**
- R3.1 New RPCs on `StateMachine` (C++ side serves both):
  ```proto
  rpc GetSnapshot(SnapshotRequest) returns (stream SnapshotChunk);
  rpc RestoreSnapshot(stream SnapshotChunk) returns (RestoreResponse);
  message SnapshotRequest {}
  message SnapshotChunk { bytes data = 1; }
  message RestoreResponse { bool success = 1; string error = 2; }
  ```
  Chunk payload is the Phase 2 base-file format (`KVB1` + records) — one format for
  disk and wire, chunked at 64 KB. New messages/tags only; existing RPCs untouched.

**C++ (`cpp-app/src/raft/state_machine.hpp`, `storage/kv_store.hpp`):**
- R3.2 `IKVStore` gains `snapshot_state()` (consistent copy of the map) and
  `restore_state(map)` (swap in, persist atomically, reset WAL). Copy is taken under
  the store mutex, streaming happens after release — the lock is held O(copy), not O(network).
- R3.3 `GetSnapshot` streams the serialized copy; `RestoreSnapshot` accumulates chunks,
  validates the magic, and calls `restore_state`. Errors reported via `RestoreResponse.error`.

**Go (`internal/fsm/fsm.go`, `internal/raftnode/node.go`):**
- R3.4 `CppFSM.Snapshot()` returns a real `raft.FSMSnapshot` whose `Persist(sink)`
  opens `GetSnapshot` on the local C++ backend and copies the stream into the sink
  (cancel sink on error); `Release()` is a no-op. `DummySnapshot` is deleted.
- R3.5 `CppFSM.Restore(rc io.ReadCloser)` streams the reader into `RestoreSnapshot`
  in 64 KB chunks and fails hard (returns error) if C++ reports failure — a node that
  cannot restore must not serve.
- R3.6 `raftnode.New` uses `raft.NewFileSnapshotStore(cfg.DataDir, 2, os.Stderr)`
  instead of `NewDiscardSnapshotStore()`. `SnapshotInterval` and `SnapshotThreshold`
  become flags in `internal/config` with a `DefaultXxx` pattern (defaults: 120s / 8192
  entries; low values used in tests).

**Semantics:**
- R3.7 After restore, the node's local `kv.db`/`kv.wal` reflect exactly the snapshot
  (Phase 2 atomic persist + WAL reset), so a crash right after restore recovers to the
  snapshot state, and raft replays only post-snapshot entries.
- R3.8 Old raft log entries written before this phase must still apply (no change to
  entry payload format — compatibility rule holds).

### Acceptance criteria
- Unit: Go fsm snapshot/restore tests against a fake `StateMachineClient` (stream
  success, mid-stream error → sink canceled, restore failure → error propagated);
  C++ tests for `snapshot_state`/`restore_state` round-trip and lock behavior.
- E2E (`tests/e2e/test_snapshot.py`, cluster configured with low threshold):
  1. Write past the snapshot threshold; assert a snapshot file appears in the data dir
     and BoltDB's `FirstIndex` advances (log actually compacted — exposed via `/status`).
  2. Stop a follower, delete its volume, rejoin; assert it serves all keys without
     replaying the full log (log index evidence + timing).
  3. Restart a node normally; assert startup applies snapshot + tail, not full history.
- Smoke test green; write path latency unchanged (snapshotting happens off the apply path —
  HashiCorp raft snapshots via `Snapshot()` on its own schedule).

## Plan

1. **Proto**: add R3.1 to `proto/consensus.proto`; regenerate Go stubs per
   `.claude/rules/protobuf.md`; commit.
2. **C++ store**: add `snapshot_state`/`restore_state` to `IKVStore` +
   `PersistentKVStore` (reusing Phase 2 `format.hpp` and atomic persist); unit tests.
3. **C++ RPCs**: implement `GetSnapshot`/`RestoreSnapshot` in `StateMachineService`;
   register in the existing `StateMachineServer`.
4. **Go fsm**: real `FSMSnapshot` (Persist/Release) and `Restore` per R3.4–R3.5;
   extend the `StateMachineClient` interface accordingly; table-driven tests with a
   fake streaming client.
5. **Go raftnode/config**: file snapshot store + tunables (R3.6); expose
   `first_log_index`/`last_snapshot_index` in the management `/status` JSON so tests
   and operators can observe compaction.
6. **E2E**: `tests/e2e/test_snapshot.py` (three scenarios above); a compose override
   file (`docker-compose.test.yml`) setting aggressive snapshot flags for CI.
7. **Docs**: remove the "No snapshots" known-limitation from CLAUDE.md and the
   corresponding note in `.claude/rules/go.md`; document snapshot tunables in README.

### Verification
- `go test -race ./...`, `ctest`.
- Full e2e including `test_snapshot.py` via CI; manual run of the wipe-and-rejoin
  scenario watching `docker compose logs` for restore lines on the rejoined node.

## Outcome

**Status: ✅ Complete.** All eight requirements landed. This is the phase CLAUDE.md
called "its own project"; it is now the one with the strongest end-to-end evidence.

### Verified results

| Layer | Result |
|---|---|
| C++ | **224 tests**, 100% pass via CTest — up from 191 in Phase 2 |
| C++ sanitizers | 224/224 under `-fsanitize=address,undefined` |
| clang-format | gate clean across `cpp-app/src` and `cpp-app/tests` |
| Go | `gofmt` clean, `go vet` clean, `go test -race` green. `internal/raftnode` has tests for the first time (71%); `fsm` and `rpc` at 100%, `management` 91.8%, `config` 89.5%, `backend` 87.2% |
| e2e (default) | 13 passed, 4 deselected |
| e2e (`-m requires_docker`) | **4/4** — the Phase 2 crash test plus all three Phase 3 snapshot scenarios |

### The three snapshot scenarios, and the evidence they are real

Run against a cluster started with `docker-compose.test.yml`
(`SnapshotInterval=2s SnapshotThreshold=20 TrailingLogs=10`):

1. **The log actually compacts.** `/status` after the run:
   `first_log_index=83, last_log_index=96, last_snapshot_index=92` on the leader —
   the first 82 entries are gone. A snapshot directory (`2-92-…`) exists on disk.
   Asserting on `first_log_index` rather than on the file's existence is the point:
   a snapshot that is written but never truncates anything is the default
   behavior, not a working one.
2. **A wiped follower catches up by snapshot transfer.** Its volume is deleted
   entirely, then it rejoins. The logs show the mechanism rather than a lucky
   replay:
   `raft: Installed remote snapshot`, `snapshot restore progress: … 100.00%`,
   `fsm: restored a 4937 byte snapshot into the C++ state machine`, and on the C++
   side `[StateMachine] Restored snapshot: 87 entries (4937 bytes)`.
3. **A normal restart applies snapshot + tail**, not the whole history.

### Decisions taken during implementation

- **`Snapshot()` captures eagerly; `Persist()` only writes.** R3.4 describes
  opening the stream inside `Persist()`. That is wrong under raft's contract:
  raft calls `Snapshot()` on the FSM goroutine with no `Apply` in flight, then may
  call `Persist()` later, *concurrently with subsequent Applies*. Streaming from
  the live store at Persist time would produce a snapshot labelled index N whose
  contents are the state at some later index N+k. Because this store is
  idempotent the cluster would re-converge on replay, so the bug would pass every
  test while being wrong in general. The cost of doing it correctly is the whole
  store in memory during a snapshot — a real scaling limit, documented in the code
  rather than left implicit.
- **`restore_state()` resets the WAL *before* writing the base file** — the
  opposite order from compaction, deliberately. Compaction's WAL records are
  already subsumed by the new base file, so a crash between the two costs an
  idempotent re-replay. A restore's WAL holds *pre-restore* commands that the
  snapshot neither contains nor was built from, so "persist snapshot, crash,
  replay stale WAL on top" would recover to a state no replica ever had.
- **One codec, not two.** The KVB1 encoder/decoder moved out of
  `PersistentKVStore` into free `serialize_state`/`deserialize_state` functions
  that both the base file and the snapshot stream go through, so the disk format
  and the wire format cannot drift.
- **The RPC bodies are thin adapters** over transport-free helpers
  (`snapshot::for_each_chunk`, `snapshot::restore_from_payload`), because
  `grpc::ServerWriter`/`ServerReader` cannot be constructed outside a running
  server. The chunking and the decode-fully-before-touching-the-store rule are
  unit-tested directly; the RPC wiring itself is covered only by the e2e suite,
  which is stated plainly rather than papered over with a test that pretends.

### Two real bugs found while verifying

- **`entrypoint.sh` passed `-bootstrap true`.** Go's `flag` package treats the
  bare `true` as the first positional argument and *stops parsing flags there*, so
  every flag after it was silently discarded. Harmless while nothing followed it;
  Phase 3 appends the snapshot tunables, so the bootstrap node was running
  production snapshot settings while its command line said otherwise. Fixed to
  `-bootstrap=true`. This was caught by `test_snapshot.py` refusing to run against
  a cluster whose *reported* settings did not match the override — a guard written
  to avoid a vacuous pass found a live misconfiguration instead.
- **`raft-boltdb` v1 cannot be tested under `-race`.** It pulls
  `github.com/boltdb/bolt`, which is unmaintained and trips Go's `checkptr`
  instrumentation — and `-race` enables `checkptr`. `internal/raftnode` had no
  tests before this phase, so nothing had ever opened a BoltDB store under the
  race detector; the first test that did died with "converted pointer straddles
  multiple allocations". Migrated to `raft-boltdb/v2` (bbolt), which shares the
  on-disk format and is a drop-in.
