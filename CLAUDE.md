# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

RaftKV is a distributed key-value store using a **sidecar pattern**: each node runs two processes that talk to each other over localhost gRPC in both directions.

- `cpp-app/` — C++17 storage engine: HTTP API, in-memory KV store with crash-safe persistence (write-ahead log + atomically rewritten binary base file), and a gRPC `StateMachine` server (binary: `kvdb_node`)
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

# The one exception: the Phase 2 crash test, which SIGKILLs and restarts a
# container. Deselected by default (pytest.ini `addopts`), opt in explicitly.
pytest tests/e2e -m requires_docker -v
```

Use `docker compose` (the CLI plugin), not the standalone `docker-compose` binary — CI and the local toolchain only guarantee the former.

There are three test layers, all of which must stay green:

1. **Go unit tests** (`go-sidecar/internal/*/*_test.go`) — `internal/fsm`, `internal/config`, `internal/cluster`, `internal/management`, `internal/backend`, `internal/rpc`, run with `go test -race ./...`. `internal/raftnode` and `cmd/sidecar` have no tests yet.
2. **C++ unit tests** (`cpp-app/tests/*.cpp`, GoogleTest via CTest) — `KVCommand::from_msgpack`, `PersistentKVStore`, `HttpRequestParser`, `StateMachineService::Apply`, `KVHttpHandler`. Built only when `-DKVDB_BUILD_TESTS=ON`; uses the system GoogleTest when present, otherwise fetches it. Also run under `-fsanitize=address,undefined` in CI. Test sources are **listed explicitly** in `CMakeLists.txt`, not globbed — a new test file is a visible diff.
3. **End-to-end** (`tests/e2e/`, pytest) — asserts that a write on the leader is readable on **all three** nodes, DELETE round-trips, and the full HTTP status-code contract (404 on a miss, 503 naming the leader on a follower write, 415/400 on bad requests, and a regression test proving a malformed `Content-Length` no longer kills a node). Requires a live cluster; exits 1 with a "cluster does not look ready" message if there isn't one. These tests never start, stop or build anything with docker — the compose lifecycle belongs to CI and to the `cluster-smoke-test` skill.
   - `tests/e2e/test_crash.py` is the deliberate exception (Phase 2): it SIGKILLs a follower, restarts it, and asserts the acknowledged writes were already on that node's disk *while it was dead* — the only way to tell durability from raft log replay, since restarts replay the whole log. It carries the `requires_docker` marker and is deselected by `addopts` so the default run stays docker-free; it *skips* (never fails) when docker, the compose project or a local cluster is unavailable.

`test_client.py` is retained only as a manual one-shot demo — it asserts nothing and checks a single node. Use `pytest tests/e2e` for verification. If you add tests, follow [.claude/rules/testing.md](.claude/rules/testing.md).

**CI**: `.github/workflows/ci.yml` runs on every push to `main` and every PR, with four independent jobs: `go` (blocking `gofmt -l`, `go vet`, `go test -race` + coverage summary), `cpp` (blocking `clang-format` check against `.clang-format`, cmake build with the project warning flags, `ctest`), `cpp-sanitizers` (the same tests under ASan+UBSan), and `e2e` (docker build, `docker compose up -d`, bounded readiness poll, `pytest tests/e2e`, then `pytest tests/e2e -m requires_docker` for the crash test, `docker compose down -v`).

## Architecture — The Two Data Paths

Understanding writes vs. reads is the key to this codebase:

**Write path** (goes through consensus):
1. Client sends `POST /insert-val` with a MsgPack body `{op, key, value}` to the C++ HTTP server (`cpp-app/src/network/http_server.hpp`)
2. C++ forwards the **raw MsgPack bytes opaquely** in `Command.data` via `GrpcRaftClient.propose()` → Go sidecar's `RaftNode.Propose` (port 50052) (`cpp-app/src/raft/raft_client.hpp`, `go-sidecar/internal/rpc/server.go`)
3. Go calls `raft.Apply()`; the leader replicates the log entry to followers
4. Once committed, every node's `CppFSM.Apply` (`go-sidecar/internal/fsm/fsm.go`) calls back into its local C++ `StateMachine.Apply` (port 50051)
5. C++ **only now** deserializes the MsgPack into a `KVCommand` (`cpp-app/src/commands/kv_command.hpp`), validates it (`validation_error()` — unknown op, empty key), and applies SET/DELETE to `PersistentKVStore`, which appends the command to the write-ahead log `kv.wal` and **fsyncs it before** the in-memory map changes. The base file `kv.db` is not touched on a normal write — it is rewritten only when the WAL crosses a compaction threshold (see "Storage on disk" below)
6. Every failure on that path is reported truthfully back down it: `StateMachine.Apply` fills `ApplyResponse.error`, `CppFSM.Apply` turns a failed apply into an `*fsm.ApplyError` (never `nil`), `rpc.Server.Propose` surfaces it in `ProposeResponse.error`, `GrpcRaftClient::propose` returns a `ProposeResult{success, error}`, and `KVHttpHandler` maps that onto a status code and a JSON body

**Read path** (no consensus): `GET /get-val?key=...` is served directly from the local in-memory store. Reads on followers can be stale.

Important consequences:
- The proto `Command.op/key/value` fields are **unused in transit** — the payload rides in `Command.data` as opaque MsgPack. Only the C++ state machine parses it, at apply time. Changing the wire format means touching `kv_command.hpp`, `cpp-app/tests/kv_command_test.cpp`, `tests/e2e/contracts.py`, `test_client.py`, and the README API docs together.
- Writes sent to a follower are **rejected, not forwarded**. `raft.Apply` returns `ErrNotLeader`; `rpc.Server.Propose` reports `ProposeResponse{success:false, error:"not_leader:<raft addr>"}` using `rpc.NotLeaderPrefix` and the address from `LeaderWithID`; the C++ handler maps that prefix onto **503** with `{"error":"not leader","leader":"node1:8088"}`. That prefix is a wire contract, not a log message — Phase 4 leader forwarding is built on it, so any other failure must not carry it (those become **502**).
- The exact HTTP status/body for every path is tabulated in [README.md](README.md#api-reference) and mirrored as constants in `tests/e2e/contracts.py`. Change the handler and both of those in the same commit.
- Startup order matters: the C++ app must be up before the sidecar connects. `entrypoint.sh` probes `/dev/tcp/127.0.0.1/50051` in a bounded loop (`APP_WAIT_TIMEOUT`, default 30s) before launching the sidecar, and `backend.Connect` additionally waits for the gRPC channel to reach `Ready` with its own retry budget. See `entrypoint.sh` for launch order and the CLI args of both binaries.

### Storage on disk (Phase 2)

`PersistentKVStore` keeps two files in `DATA_DIR`, and the split is the durability argument:

- **`kv.wal`** — append-only log of `uint32 len | msgpack KVCommand | uint32 crc32`. `set()`/`remove()` append one record and `fsync` it *before* mutating the map, so there is no instant at which a client has been told a write succeeded but the bytes are not on disk. This is also what makes an apply O(entry) instead of O(store).
- **`kv.db`** — a whole-map snapshot in the binary `KVB1` format (`"KVB1" | uint32 count | count × (uint32 klen | key | uint32 vlen | value)`, little-endian, nothing escaped). Written only by compaction, and only through `atomic_write_file()` — temp file, `fsync`, `rename`, `fsync` the directory — so a reader never sees a half-written base file. A transient `kv.db.tmp` during that sequence is normal, and surviving one after a crash is harmless.

Recovery is "load `kv.db`, then replay `kv.wal` over it". A torn tail (short record, length overrun, bad CRC) truncates the WAL at that point and stops: everything before it is applied, nothing after. Compaction (base file first, *then* truncate the WAL) runs synchronously inside the store lock when the WAL passes `DurabilityOptions::wal_max_bytes` / `wal_max_records`. A pre-Phase-2 line-based `kv.db` is read once with the legacy parser and immediately rewritten as `KVB1`.

Consequences worth knowing before touching this code:

- **Keys and values are now byte-transparent.** `=`, newlines and NUL bytes round-trip. The Phase 0 tests that pinned the old truncation were flipped, not deleted.
- **`set()`/`remove()` can throw** (WAL append, `fsync`, or a compaction rewrite failing). `StateMachineService::Apply` catches it and answers `success=false`, which `CppFSM.Apply` treats as a *deterministic* rejection — accurate for a malformed command, **not** accurate for a local I/O error, where this node really can diverge from its peers. See the phase-2 doc's Outcome section; nothing depends on it yet, but do not build on that branch assuming determinism.
- **The WAL grows on every restart** while there are no snapshots: raft replays its whole BoltDB log through `Apply`, and each of those applies is a fresh WAL record, until compaction folds them away.
- The store's locking discipline (`_unlocked` helpers, never re-locking) is a correctness requirement, not a naming convention: see [.claude/rules/cpp.md](.claude/rules/cpp.md).

## Component Map

| Concern | C++ (`cpp-app/src/`) | Go (`go-sidecar/internal/`) |
|---|---|---|
| Config / CLI args | `config/config.hpp` (positional args) | `config/config.go` (flags) |
| Client-facing API | `network/http_server.hpp` (hand-rolled HTTP over sockets) | `management/server.go` (`/join`, `/status`, `/health` on :6000) |
| Consensus glue | `raft/raft_client.hpp` (propose), `raft/state_machine.hpp` (apply) | `rpc/server.go` (propose), `fsm/fsm.go` (apply) |
| Storage | `storage/kv_store.hpp` (map + recovery), `storage/wal.hpp` (append/replay/heal), `storage/atomic_file.hpp` (fsync + rename), `storage/format.hpp` (length-prefix + CRC32) | `raftnode/node.go` (BoltDB raft log in `DATA_DIR/logs.dat`) |
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

Phase 0 removed none of these — it **pinned** them with tests that assert the current (wrong) behavior, each commented with the phase that will change it. Fixing a limitation therefore means updating its pinning tests in the same PR: see the pinned-behavior tables in `tests/e2e/README.md` and the `CURRENT LOSSY BEHAVIOR` / `PINNED` comments in `cpp-app/tests/` and `go-sidecar/internal/fsm/fsm_test.go`. This list shrinks as phases land — Phase 1 removed the stringly-error entry and Phase 2 removed the durability entry (WAL + atomic base file + binary format), so do not reintroduce either.

- **No snapshots**: `CppFSM` returns a `DummySnapshot` and the raft node uses `NewDiscardSnapshotStore()` — the raft log grows unbounded and restarts replay the full log.
- **HTTP server**: single-threaded accept loop, one 4KB read per request, no keep-alive, no body-size cap, no URL decoding. (Status lines and error bodies are truthful as of Phase 1; a negative `Content-Length` is still unhandled.)
- **No leader forwarding**: a write to a follower is answered with 503 and the leader's *Raft* address, which is not something a client can dial. Phase 4.
- **Stale reads**: `GET /get-val` is served from the local store with no read-index check, and there is no way to request a linearizable read. Phase 4.
- **Validation happens after commit**: a command with an unknown op or an empty key is replicated first and rejected at apply time, costing a raft log entry and returning 502.
- **No TLS/auth anywhere**: all gRPC channels and HTTP endpoints are insecure; the management `/join` endpoint is unauthenticated.
