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

- `raftnode.New` uses the same BoltDB store for log and stable store, and `NewDiscardSnapshotStore()` — snapshots are intentionally not implemented (`DummySnapshot`). Don't add snapshot logic as a drive-by; it requires a C++-side state export and is its own project.
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
