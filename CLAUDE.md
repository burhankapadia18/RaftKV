# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

RaftKV is a distributed key-value store using a **sidecar pattern**: each node runs two processes that talk to each other over localhost gRPC in both directions.

- `cpp-app/` — C++17 storage engine: HTTP API, in-memory KV store with file persistence, and a gRPC `StateMachine` server (binary: `kvdb_node`)
- `go-sidecar/` — Go consensus sidecar wrapping HashiCorp Raft: leader election, log replication, cluster membership (Go module name: `my-raft-sidecar`)
- `proto/consensus.proto` — the single gRPC contract between them (`RaftNode.Propose` and `StateMachine.Apply`)

## Commands

```bash
# Full cluster (the primary way to run and verify anything)
docker build -t raftkv:latest .
docker compose up -d                  # 3 nodes: HTTP on host ports 8080/8081/8082
docker compose logs -f
docker compose down                   # add -v and rm -rf vol-node* for a clean slate

# Go sidecar
cd go-sidecar && go build -o sidecar ./cmd/sidecar
cd go-sidecar && test -z "$(gofmt -l .)" && go vet ./... && go test -race ./...

# C++ engine (local build; needs cmake, gRPC/protobuf dev libs, libmsgpack-dev)
cd cpp-app && mkdir -p build && cd build && cmake .. && make -j4

# C++ unit tests (GoogleTest via CTest; KVDB_BUILD_TESTS defaults to OFF, so the
# command above and the Docker image build exactly as before — no gtest needed)
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
ctest --test-dir cpp-app/build --output-on-failure

# End-to-end suite — needs a cluster already running (it never touches docker)
pip install -r tests/e2e/requirements.txt
pytest tests/e2e -v
```

Use `docker compose` (the CLI plugin), not the standalone `docker-compose` binary — CI and the local toolchain only guarantee the former.

There are three test layers, all of which must stay green:

1. **Go unit tests** (`go-sidecar/internal/*/*_test.go`) — `internal/fsm`, `internal/config`, `internal/cluster`, `internal/management`, run with `go test -race ./...`. `internal/backend`, `internal/raftnode`, `internal/rpc` and `cmd/sidecar` have no tests yet.
2. **C++ unit tests** (`cpp-app/tests/*.cpp`, GoogleTest via CTest) — `KVCommand::from_msgpack`, `PersistentKVStore`, `HttpRequestParser`. Built only when `-DKVDB_BUILD_TESTS=ON`; uses the system GoogleTest when present, otherwise fetches it. Also run under `-fsanitize=address,undefined` in CI.
3. **End-to-end** (`tests/e2e/`, pytest) — asserts that a write on the leader is readable on **all three** nodes, DELETE round-trips, missing keys, and follower-write rejection. Requires a live cluster; exits 1 with a "cluster does not look ready" message if there isn't one.

`test_client.py` is retained only as a manual one-shot demo — it asserts nothing and checks a single node. Use `pytest tests/e2e` for verification. If you add tests, follow [.claude/rules/testing.md](.claude/rules/testing.md).

**CI**: `.github/workflows/ci.yml` runs on every push to `main` and every PR, with four independent jobs: `go` (blocking `gofmt -l`, `go vet`, `go test -race` + coverage summary), `cpp` (blocking `clang-format` check against `.clang-format`, cmake build with the project warning flags, `ctest`), `cpp-sanitizers` (the same tests under ASan+UBSan), and `e2e` (docker build, `docker compose up -d`, bounded readiness poll, `pytest tests/e2e`, `docker compose down -v`).

## Architecture — The Two Data Paths

Understanding writes vs. reads is the key to this codebase:

**Write path** (goes through consensus):
1. Client sends `POST /insert-val` with a MsgPack body `{op, key, value}` to the C++ HTTP server (`cpp-app/src/network/http_server.hpp`)
2. C++ forwards the **raw MsgPack bytes opaquely** in `Command.data` via `GrpcRaftClient.propose()` → Go sidecar's `RaftNode.Propose` (port 50052) (`cpp-app/src/raft/raft_client.hpp`, `go-sidecar/internal/rpc/server.go`)
3. Go calls `raft.Apply()`; the leader replicates the log entry to followers
4. Once committed, every node's `CppFSM.Apply` (`go-sidecar/internal/fsm/fsm.go`) calls back into its local C++ `StateMachine.Apply` (port 50051)
5. C++ **only now** deserializes the MsgPack into a `KVCommand` (`cpp-app/src/commands/kv_command.hpp`) and applies SET/DELETE to `PersistentKVStore`, which rewrites `kv.db`

**Read path** (no consensus): `GET /get-val?key=...` is served directly from the local in-memory store. Reads on followers can be stale.

Important consequences:
- The proto `Command.op/key/value` fields are **unused in transit** — the payload rides in `Command.data` as opaque MsgPack. Only the C++ state machine parses it, at apply time. Changing the wire format means touching `kv_command.hpp`, `cpp-app/tests/kv_command_test.cpp`, `tests/e2e/conftest.py`, `test_client.py`, and the README API docs together.
- Writes sent to a follower **fail** (`raft.Apply` returns `ErrNotLeader`; there is no leader forwarding). The client gets `error`.
- Startup order matters: the C++ app must be up before the sidecar connects (the sidecar retries; see `entrypoint.sh` for launch order and CLI args of both binaries).

## Component Map

| Concern | C++ (`cpp-app/src/`) | Go (`go-sidecar/internal/`) |
|---|---|---|
| Config / CLI args | `config/config.hpp` (positional args) | `config/config.go` (flags) |
| Client-facing API | `network/http_server.hpp` (hand-rolled HTTP over sockets) | `management/server.go` (`/join`, `/status`, `/health` on :6000) |
| Consensus glue | `raft/raft_client.hpp` (propose), `raft/state_machine.hpp` (apply) | `rpc/server.go` (propose), `fsm/fsm.go` (apply) |
| Storage | `storage/kv_store.hpp` | `raftnode/node.go` (BoltDB raft log in `DATA_DIR/logs.dat`) |
| Cluster membership | — | `cluster/joiner.go` (retry-join via leader's mgmt API) |

Ports: 8080 HTTP · 50051 C++ StateMachine gRPC · 50052 Go RaftNode gRPC · 8088 Raft TCP · 6000 management HTTP.

## Codebase Conventions

- The C++ app is **header-only by design**: all logic lives in `.hpp` files under `cpp-app/src/`, organized by domain (`config/`, `storage/`, `raft/`, `network/`, `commands/`), with `main.cpp` as the only translation unit doing pure bootstrap. Keep new logic in domain headers, not in `main.cpp`. Dependencies are injected via the `IKVStore` / `IRaftClient` interfaces — preserve that seam.
- The Go side follows standard `cmd/` + `internal/` layout with constructor injection and small interfaces (`fsm.StateMachineClient`). Follow the existing `Config` struct + `DefaultXxxConfig()` pattern for new tunables.
- Per-language style and review rules: [.claude/rules/cpp.md](.claude/rules/cpp.md), [.claude/rules/go.md](.claude/rules/go.md).

## Protobuf Changes

`proto/consensus.proto` is the source of truth, but the two languages handle generated code differently:

- **C++**: CMake regenerates stubs at build time into `build/proto/`. The checked-in `cpp-app/pb/` directory is a **stale, unreferenced copy** — do not edit it or rely on it.
- **Go**: stubs in `go-sidecar/pb/` are **checked in and must be regenerated manually** when the proto changes.

See [.claude/rules/protobuf.md](.claude/rules/protobuf.md) for the regeneration workflow.

## Known Limitations (deliberate — don't "fix" silently)

These are acknowledged simplifications. If a task touches one, call it out and confirm scope before redesigning.

Phase 0 removed none of these — it **pinned** them with tests that assert the current (wrong) behavior, each commented with the phase that will change it. Fixing a limitation therefore means updating its pinning tests in the same PR: see the pinned-behavior tables in `tests/e2e/README.md` and the `CURRENT LOSSY BEHAVIOR` / `PINNED` comments in `cpp-app/tests/` and `go-sidecar/internal/fsm/fsm_test.go`.

- **No snapshots**: `CppFSM` returns a `DummySnapshot` and the raft node uses `NewDiscardSnapshotStore()` — the raft log grows unbounded and restarts replay the full log.
- **Durability**: `PersistentKVStore::persist()` rewrites the entire `kv.db` on every write, without `fsync` or WAL.
- **HTTP server**: single-threaded accept loop, one 4KB read per request, no keep-alive; responses always say `OK` regardless of status code.
- **No TLS/auth anywhere**: all gRPC channels and HTTP endpoints are insecure; the management `/join` endpoint is unauthenticated.
- **Errors are stringly reported**: write failures return HTTP 200 with body `error`.
