# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

RaftKV is a distributed key-value store using a **sidecar pattern**: each node runs two processes that talk to each other over localhost gRPC in both directions.

- `cpp-app/` — C++17 storage engine: HTTP API, in-memory KV store with crash-safe persistence (write-ahead log + atomically rewritten binary base file), and a gRPC `StateMachine` server (binary: `kvdb_node`)
- `go-sidecar/` — Go consensus sidecar wrapping HashiCorp Raft: leader election, log replication, cluster membership (Go module name: `my-raft-sidecar`)
- `proto/consensus.proto` — the single gRPC contract between them (`RaftNode.Propose`, `StateMachine.Apply`, and the streaming `StateMachine.GetSnapshot` / `StateMachine.RestoreSnapshot`)

## Commands

```bash
# Full cluster (the primary way to run and verify anything)
docker build -t raftkv:latest .
docker compose up -d                  # 3 nodes: HTTP on host ports 8080/8081/8082
# `image:` is ${RAFTKV_IMAGE:-raftkv:latest}, so the default is the LOCAL build and
# a published release needs no compose edit:
#   RAFTKV_IMAGE=ghcr.io/burhankapadia18/raftkv:1.0.0 docker compose up -d
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

# The exceptions, both deselected by default (pytest.ini `addopts`) and opted
# into explicitly. -rs prints skip reasons; a silent skip is an unverified
# requirement.
pytest tests/e2e -m requires_docker -v -rs   # crash + snapshot: drives containers
pytest tests/e2e -m requires_secure -v -rs   # TLS profile: needs the secure cluster

# Secure profile (Phase 6). Certs first, then layer the override on.
./scripts/gen-certs.sh                        # dev CA + node certs -> ./certs (gitignored)
export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d
curl --cacert certs/ca.pem https://localhost:6000/status
curl --cacert certs/ca.pem -X PUT --data-binary v https://localhost:8443/kv/k

# Chaos harness — needs a cluster up; drives docker itself (Phase 7)
python tests/chaos/chaos.py --duration 600

# Benchmarks — separate Go module, no third-party deps
cd bench && go run ./cmd/kvbench -workload all -clients 32 -duration 20s

# Fuzzers (clang only; not built by default)
cmake -S cpp-app -B cpp-app/fuzz-build -DKVDB_BUILD_FUZZERS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build cpp-app/fuzz-build -j4
# Two corpus dirs: libFuzzer WRITES to the first and reads the rest as
# seeds. Passing the checked-in seed dir first would fill it with
# generated inputs.
mkdir -p /tmp/corpus-kv
./cpp-app/fuzz-build/fuzz_kv_command /tmp/corpus-kv \
  cpp-app/fuzz/corpus/kv_command -max_total_time=60
```

Use `docker compose` (the CLI plugin), not the standalone `docker-compose` binary — CI and the local toolchain only guarantee the former.

There are three test layers, all of which must stay green:

1. **Go unit tests** (`go-sidecar/internal/*/*_test.go`) — `internal/fsm`, `internal/config`, `internal/cluster`, `internal/management`, `internal/backend`, `internal/rpc`, `internal/peers`, `internal/raftnode`, `internal/tlsconfig`, run with `go test -race ./...`. `cmd/sidecar`, `internal/logging` and `internal/metrics` have no tests. `internal/testcerts` is a **non-test package imported only by tests** — Go has no other way to share a cert generator across packages; it lives under `internal/` and nothing outside a `_test.go` imports it. It deliberately does not import `internal/tlsconfig`, because `tlsconfig`'s own in-package tests would then be an import cycle.
2. **C++ unit tests** (`cpp-app/tests/*.cpp`, GoogleTest via CTest) — `KVCommand::from_msgpack`, `PersistentKVStore`, `HttpRequestParser`, `StateMachineService::Apply`, `KVHttpHandler`. Built only when `-DKVDB_BUILD_TESTS=ON`; uses the system GoogleTest when present, otherwise fetches it. Also run under `-fsanitize=address,undefined` in CI. Test sources are **listed explicitly** in `CMakeLists.txt`, not globbed — a new test file is a visible diff.
3. **End-to-end** (`tests/e2e/`, pytest) — asserts that a write on the leader is readable on **all three** nodes, DELETE round-trips, and the full HTTP status-code contract (404 on a miss, 503 naming the leader on a follower write, 415/400 on bad requests, and a regression test proving a malformed `Content-Length` no longer kills a node). Requires a live cluster; exits 1 with a "cluster does not look ready" message if there isn't one. These tests never start, stop or build anything with docker — the compose lifecycle belongs to CI and to the `cluster-smoke-test` skill.
   - `tests/e2e/test_security.py` (Phase 6) runs in the default suite: the request caps (413/431, plus a burst that proves the rejection path does not leak), and cluster-membership auth. Its load-bearing assertion is not a status code — it is that `last_log_index` does **not** move after a refused `/join`, because middleware that returned 403 *after* calling `AddVoter` would pass a status-code-only test and leave the cluster compromised.
   - `tests/e2e/test_secure_profile.py` (Phase 6) carries the `requires_secure` marker: it needs the cluster brought up with `docker-compose.secure.yml` and a CA in `./certs`, and skips otherwise. It checks the properties only a deployment has — plaintext refused on the raft port, a client certificate actually required there, HTTPS management, and that the plaintext client ports are **not** published (compose concatenates `ports` across files rather than replacing, so `!override` is required and was missed the first time).
   - `tests/e2e/test_crash.py` is the deliberate exception (Phase 2): it SIGKILLs a follower, restarts it, and asserts the acknowledged writes were already on that node's disk *while it was dead* — the only way to tell durability from raft log replay, since restarts replay the whole log. It carries the `requires_docker` marker and is deselected by `addopts` so the default run stays docker-free; it *skips* (never fails) when docker, the compose project or a local cluster is unavailable.

`test_client.py` is retained only as a manual one-shot demo — it asserts nothing and checks a single node. Use `pytest tests/e2e` for verification. If you add tests, follow [.claude/rules/testing.md](.claude/rules/testing.md).

**Chaos and benchmarks** (Phase 7, both outside the three layers above):

- `tests/chaos/` — fault injection under load with an invariant checker (acknowledged writes survive; replicas converge). Drives docker directly, which is the deliberate opposite of the e2e rule. Runs nightly, not per-PR. Its checker proves it can fail before every run, because a broken checker and a healthy cluster produce identical output. `pytest tests/chaos/test_journal.py` covers the journal fold with no cluster. See [tests/chaos/README.md](tests/chaos/README.md).
- `bench/` — a **separate Go module** (`raftkv-bench`) with no third-party dependencies. Gated by its own `bench-build` CI job, because the `go` job is scoped to `go-sidecar` and would never compile it. Results in [docs/benchmarks.md](docs/benchmarks.md), labelled as laptop measurements with their ±25% run-to-run variance stated.

**CI**: `.github/workflows/ci.yml` runs on every push to `main` and every PR, with independent jobs: `go` (blocking `gofmt -l`, `go vet`, `go test -race` + coverage summary, `gosec` pinned to v2.21.4 with `-exclude-dir=pb`), `cpp` (blocking `clang-format` check against `.clang-format`, cmake build with the project warning flags, `ctest`), `cpp-sanitizers` (ASan+UBSan), `cpp-tsan` (ThreadSanitizer — needs `--security-opt seccomp=unconfined`, or the build dies in `gtest_discover_tests` on a blocked `personality()` call), `fuzz` (both libFuzzer targets, 60s each, crash inputs uploaded), `e2e` (docker build, `docker compose -f docker-compose.yml -f docker-compose.test.yml up -d`, bounded readiness poll, `pytest tests/e2e`, then `-m requires_docker`), `e2e-secure` (the same cluster under `docker-compose.secure.yml`, `-m requires_secure`, **plus a forced leader failover** — see below), and `bench-build`. Two more workflows exist: `chaos.yml` (nightly, not per-PR) and `release.yml` (on a `v*` tag: full suite → multi-arch buildx push to GHCR → smoke-test the *published* image → GitHub release, in that order, because publishing before verifying leaves a broken artifact people can pull).

The failover step in `e2e-secure` is not padding. Phase 6 found a bug where the bootstrap node advertised its *resolved container IP*, so a peer dialling it verified the certificate against `172.18.0.2` while the certificate was issued for `node1`. The cluster forms, replicates and passes every smoke test; it breaks only after the first election, permanently, because the address is in the committed raft configuration. Only an election catches it.

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
| TLS / auth (Phase 6) | — | `tlsconfig/` (X.509 material for every surface), `raftnode/tls_transport.go` (mutual-TLS `raft.StreamLayer`), `management/auth.go` (bearer token) |

Ports: 8080 HTTP · 50051 C++ StateMachine gRPC (**127.0.0.1 only**) · 50052 Go RaftNode gRPC (peer-reachable) · 8088 Raft TCP · 6000 management HTTP(S) · 8443 client HTTPS via the proxy in the secure profile.

### Security model (Phase 6)

Security is opt-in by configuration; the default compose profile is deliberately plaintext (a demo needing a CA is a demo nobody runs), and `docker-compose.secure.yml` is the documented deployment. Four rules worth knowing before touching any of it:

- **`internal/tlsconfig` is the only place a `*tls.Config` is built.** The dangerous parts of a TLS config are the ones easy to omit: a server with a cert and key but no `ClientCAs` completes a handshake with anybody, and looks like mutual TLS while authenticating nobody. Centralizing it means that mistake can only be made once, under test. Do not hand-roll a `tls.Config` at a call site.
- **`ServerConfig`'s `requireClientCert` is a separate argument, not inferred from `CAFile`.** The raft transport must refuse anyone who cannot prove cluster membership; the management listener must *not* demand client certs, because health probes and Prometheus have none (they authenticate with the bearer token instead). Inferring would silently turn "I supplied a CA" into "I demand client certs" and break every probe.
- **The TLS raft transport advertises the configured hostname, the plaintext one advertises a resolved IP — and that asymmetry is forced.** `raft.NewTCPTransport` rejects an advertise address that is not a `*net.TCPAddr` with a concrete IP, so the plaintext path must resolve. Under TLS the resolved IP is fatal: a peer verifies the certificate against the address it dialled, and a certificate for `node1` does not cover `172.18.0.2`. `NewNetworkTransport` makes no such demand, so the TLS path passes `hostPortAddr(advertiseAddr)`. See the long comment on that type; do not "simplify" the two paths back together.
- **A misconfigured TLS surface must fail startup, never fall back.** `Config.Validate()` runs before anything binds, and `NewJoiner`/`WithTLS` return errors rather than degrading to HTTP. A relay or a join that quietly fell back to plaintext would put the cluster-admin token on the wire in clear, which is worse than a node that refuses to boot.

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

Phase 0 removed none of these — it **pinned** them with tests that assert the current (wrong) behavior, each commented with the phase that will change it. Fixing a limitation therefore means updating its pinning tests in the same PR: see the pinned-behavior tables in `tests/e2e/README.md` and the `CURRENT LOSSY BEHAVIOR` / `PINNED` comments in `cpp-app/tests/` and `go-sidecar/internal/fsm/fsm_test.go`. This list shrinks as phases land — Phase 1 removed the stringly-error entry, Phase 2 the durability entry (WAL + atomic base file + binary format), Phase 3 the no-snapshots entry, Phase 4 the leader-forwarding, stale-read and single-threaded-server entries, and Phase 6 the blanket "no TLS/auth anywhere" — replaced below by three narrower statements that are actually true. Do not reintroduce any of them.

- **HTTP server**: no keep-alive — the server closes the socket after every response, so a client must not pool connections. (Phase 4 added the worker pool, the read-until-terminator header loop, body/header caps and URL decoding; Phase 1 made status lines and error bodies truthful and rejects a negative `Content-Length`. None of those clauses should be reinstated.)
- **Legacy routes do not URL-decode**: `/kv/{key}` percent-decodes its path, `/get-val?key=` and `/insert-val` do not, so the same logical key is addressed differently through the two surfaces. Deliberate — decoding the deprecated routes would silently move which key an existing client reaches. They go away a release after Phase 4.
- **Response content negotiation is not implemented**: R4.6 asks for msgpack responses under `Accept: application/msgpack`; requests are flexible but responses are always JSON envelopes plus raw values for reads. `HttpRequest::headers` is never populated, so honoring `Accept` needs real header capture first.
- **Validation happens after commit**: a command with an unknown op or an empty key is replicated first and rejected at apply time, costing a raft log entry and returning 502. Moving it to the propose boundary means the HTTP layer parsing the msgpack it currently forwards opaquely — an architectural change, not a small one.
- **No client authentication or authorization**: anyone who can reach the client HTTP API can read and write every key, in both compose profiles. The Phase 6 bearer token guards cluster *membership* (`/join`, `/remove`), not data. No per-key ACLs, no roles. Phase 6 scoped this out for 1.0 deliberately — the README's Security section says so plainly rather than implying the secure profile is fully locked down.
- **The intra-node gRPC pair is plaintext**: 50051 binds `127.0.0.1` and never leaves the container, so that one is fine. **50052 is not** — Phase 4 made it peer-reachable for write and read forwarding, so a peer that can reach it can propose writes with no credential. It sits inside the same trust boundary as the Raft port but, unlike the Raft port, is unauthenticated. A real gap, knowingly left.
- **Certificates do not rotate**: TLS material is read once at startup. Changing a certificate means restarting the node.
- **One raft group, and this is the ceiling**: every write goes through one leader and one log. There is no sharding, so adding nodes makes write throughput *worse* (more followers to wait for), not better. Measured at ~1.9k writes/s on a laptop, saturating at 32 concurrent writers — see [docs/benchmarks.md](docs/benchmarks.md). Anything that needs more than one group's worth of writes needs a different design, not tuning.
- **The whole dataset lives in memory**, and a snapshot holds a second copy while it is being written (see the `CppFSM.Snapshot()` note in [.claude/rules/go.md](.claude/rules/go.md) for why the buffering is load-bearing). Dataset size is bounded by RAM.
- **Writes are at-least-once under failure**: a 503 does not mean the write did not happen. `rpc.Server.Propose` returns `unavailable:` (→ 503) when *forwarding* to the leader fails as a transport matter, and the leader may already have committed the entry. There are no idempotency tokens and no compare-and-set, so a retrying client gets at-least-once. Safe for the idempotent PUT/DELETE surface; not safe as a base for read-modify-write. Found by the chaos harness assuming the opposite — see the `UNKNOWN_OUTCOME_STATUSES` comment in `tests/chaos/load.py`. Do not "tighten" the 503 documentation back to "nothing was lost".
- **No client SDK**: clients speak HTTP and MsgPack directly and must implement their own retry policy around the 502/503 distinction. The distinction is documented and stable; the retrying is not done for them.
- **No bounded-staleness read**: the only choices are a local read (unbounded staleness) and a full barrier + quorum check. There is nothing in between, such as "no older than 100ms".
- **Client TLS is proxy-terminated**, so the proxy-to-node hop is plaintext and the proxy belongs on the same host as the node. Native TLS in the C++ server was considered and rejected for 1.0; see [docs/architecture.md](docs/architecture.md#security-model).
