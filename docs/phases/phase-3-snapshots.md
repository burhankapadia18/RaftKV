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
