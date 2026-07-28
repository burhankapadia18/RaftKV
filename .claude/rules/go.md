# RaftKV Go Rules

Project-specific rules for `go-sidecar/`. These extend the general Go guidance with what is idiomatic *in this repo*.

## Structure

- Module name is **`my-raft-sidecar`** (not the repo name) — imports look like `my-raft-sidecar/internal/fsm`.
- Standard layout: `cmd/sidecar/main.go` wires everything; all logic lives in `internal/` packages, one concern each:
  - `config` — flag parsing into a `Config` struct
  - `backend` — gRPC client to the C++ app (with connect retries)
  - `fsm` — `raft.FSM` implementation delegating to C++
  - `raftnode` — HashiCorp Raft setup (BoltDB log store, TCP transport)
  - `rpc` — gRPC `RaftNode` server (Propose)
  - `management` — HTTP admin API (`/join`, `/status`, `/health`)
  - `cluster` — join-with-retry client
- `main.go` stays as pure wiring; new behavior goes into an `internal` package with a constructor (`NewXxx(deps...)`).

## Patterns used here — follow them

- **Config structs with defaults**: tunables live in a `XxxConfig` struct with a `DefaultXxxConfig(...)` constructor (see `backend.ConnectionConfig`, `cluster.JoinConfig`, `raftnode.Options`). Don't scatter magic numbers; add fields there.
- **Small consumer-side interfaces**: e.g. `fsm.StateMachineClient` wraps the generated gRPC client so the FSM is testable. Wrap generated clients rather than using them directly in logic.
- **Error wrapping**: always `fmt.Errorf("context: %w", err)`; startup failures use `log.Fatalf` in `main.go` only — library packages return errors.
- **Compile-time interface checks**: `var _ raft.FSM = (*CppFSM)(nil)` — add these for any new interface implementation.
- **Retry loops for cross-process startup**: the sidecar must tolerate the C++ app or the leader not being up yet (see `backend.Connect`, `cluster.Joiner`). Any new cross-service call at startup needs the same treatment.

## Raft specifics

- `raftnode.New` uses the same BoltDB store for log and stable store, and a real `raft.NewFileSnapshotStore` (Phase 3). Snapshot tunables live in `internal/config` — `SnapshotInterval`, `SnapshotThreshold`, `TrailingLogs`.
- **`TrailingLogs` is what actually bounds the log.** Raft truncates to `snapshot_index - TrailingLogs`, so leaving the default (10240) means the log never shrinks no matter how often the node snapshots. A test that only lowers the threshold will snapshot and still observe an un-truncated log.
- **`CppFSM.Snapshot()` captures the state eagerly, and that is load-bearing.** Raft calls `Snapshot()` on the FSM goroutine with no `Apply` in flight, then may call `Persist()` *later, concurrently with subsequent Applies*. Streaming from the live C++ store inside `Persist()` would produce a snapshot labelled index N whose contents are the state at some later index N+k. This store is idempotent, so the cluster would re-converge on replay and the bug would pass every test while being wrong in general — do not "optimize" the buffering away without replacing it with a real point-in-time read. The cost is the whole store in memory during a snapshot; that is a known scaling limit, not an oversight.
- `Persist()` must `sink.Cancel()` on any error, never `sink.Close()`. A half-written snapshot that was closed is a corrupt snapshot raft will happily try to restore later.
- `CppFSM.Restore()` must return an error when the C++ side reports failure. A node that cannot restore must not serve: returning nil would leave it claiming state it does not have.
- **Use `raft-boltdb/v2` (bbolt), not v1.** The v1 module pulls `github.com/boltdb/bolt`, which is unmaintained and trips Go's `checkptr` instrumentation — and `checkptr` is enabled by `-race`, which CI runs. Any test that successfully opens a v1 BoltDB store dies with "converted pointer straddles multiple allocations". bbolt shares the on-disk format, so this is a drop-in.
- `FSM.Apply` runs on every node for every committed entry and must stay deterministic and side-effect-free apart from the C++ call.
- Only the leader can `Apply`; `rpc.Server.Propose` surfaces `ErrNotLeader` as a failed `ProposeResponse` rather than a gRPC error — keep that contract, the C++ client checks `reply.success()`.

## Checks

```bash
cd go-sidecar
gofmt -l . && go vet ./...
go build -o sidecar ./cmd/sidecar
go test -race ./...
```

- Generated code in `go-sidecar/pb/` is checked in — regenerate it when `proto/consensus.proto` changes (see [protobuf.md](protobuf.md)); never hand-edit it.
