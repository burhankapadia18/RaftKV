# RaftKV

<p align="center">
  <strong>A lightweight distributed key-value store built on Raft consensus</strong>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#architecture">Architecture</a> •
  <a href="#quick-start">Quick Start</a> •
  <a href="#api-reference">API</a> •
  <a href="#configuration">Configuration</a> •
  <a href="#data-directory">Data directory</a>
</p>

---

## Overview

**RaftKV** is a distributed key-value store that combines a high-performance C++ storage engine with the reliability of the [Raft consensus algorithm](https://raft.github.io/). The system uses a sidecar architecture where cluster coordination and replication are handled by a Go-based component using [HashiCorp Raft](https://github.com/hashicorp/raft), ensuring strong consistency with minimal coupling between components.

## Features

- **High Performance** — C++ storage engine with in-memory operations and persistent storage
- **Crash-Safe** — every applied command is fsynced to a write-ahead log before it is visible, and the base file is replaced atomically (temp + `fsync` + `rename`)
- **Strong Consistency** — Raft consensus ensures all nodes agree on the order of operations
- **Efficient Serialization** — MsgPack binary protocol for minimal overhead
- **Docker Ready** — Multi-stage Docker build with Docker Compose for easy cluster deployment
- **gRPC Communication** — Fast inter-service communication between components
- **Fault Tolerant** — Automatic leader election and cluster recovery
- **HTTP API** — Simple REST-like interface for client applications
- **Secure by configuration** — mutual TLS between Raft peers, HTTPS + bearer-token auth on the management API, and TLS termination for clients ([Security](#security))

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                          RaftKV Node                            │
├─────────────────────────────────┬───────────────────────────────┤
│         C++ Storage Engine      │       Go Raft Sidecar         │
│                                 │                               │
│  ┌─────────────────────────┐    │    ┌───────────────────────┐  │
│  │     HTTP Server         │    │    │   HashiCorp Raft      │  │
│  │     (Port 8080)         │    │    │   (Port 8088)         │  │
│  └──────────┬──────────────┘    │    └───────────┬───────────┘  │
│             │                   │                │              │
│  ┌──────────▼──────────────┐    │    ┌───────────▼───────────┐  │
│  │    State Machine        │◄───┼────│   RaftNode gRPC       │  │
│  │    (gRPC :50051)        │    │    │   (Port 50052)        │  │
│  └──────────┬──────────────┘    │    └───────────────────────┘  │
│             │                   │                               │
│  ┌──────────▼──────────────┐    │    ┌───────────────────────┐  │
│  │   Persistent Storage    │    │    │   Management API      │  │
│  │   (kv.db + kv.wal)      │    │    │   (Port 6000)         │  │
│  └─────────────────────────┘    │    └───────────────────────┘  │
└─────────────────────────────────┴───────────────────────────────┘
```

### Components

| Component | Language | Description |
|-----------|----------|-------------|
| **Storage Engine** | C++ | Handles HTTP requests, manages the key-value store, and persists data to disk |
| **Raft Sidecar** | Go | Manages cluster membership, leader election, and log replication using HashiCorp Raft |
| **Protocol Buffers** | Protobuf | Defines the gRPC service contracts between components |

### Data Flow

1. **Client Request** → HTTP POST to `/insert-val` with MsgPack payload
2. **Proposal** → C++ engine forwards to Go sidecar via gRPC
3. **Consensus** → Leader replicates log entry to followers via Raft
4. **Apply** → Once committed, sidecar calls back to C++ state machine
5. **Persist** → C++ engine appends the command to the write-ahead log and `fsync`s it, *then* updates the in-memory store (see [Data directory](#data-directory))

## Quick Start

### Prerequisites

- Docker and Docker Compose
- (For local development) Go 1.24+, CMake, gRPC/Protobuf libraries

### Running with Docker Compose

```bash
# Build the Docker image
docker build -t raftkv:latest .

# Start a 3-node cluster
docker compose up -d

# View logs
docker compose logs -f
```

> Use `docker compose` (the Docker CLI plugin). The standalone `docker-compose`
> binary is not required and is not what CI uses.

### Testing the Cluster

With the cluster running, the end-to-end suite verifies it:

```bash
# Install Python dependencies
pip install -r tests/e2e/requirements.txt

# Run the asserting end-to-end suite (exits non-zero on any failure)
pytest tests/e2e -v
```

It checks that a write on the leader replicates to **all three** nodes, that
DELETE round-trips, that a missing key returns `404`, that a write sent to a
follower returns `503` naming the leader, and that a malformed `Content-Length`
returns `400` without killing the node. See
[tests/e2e/README.md](tests/e2e/README.md) for configuration, the full contract
table, and the assertions that still deliberately pin known-wrong behavior.

The crash-recovery test is opt-in, because it is the one test that touches
docker — it `SIGKILL`s a node and restarts it:

```bash
pytest tests/e2e -m requires_docker -v
```

`test_client.py` is still around as a one-shot manual demo, but it asserts
nothing and only touches a single node — use the suite above for verification.

Or use curl directly:

```bash
# Write a value (using Python for MsgPack encoding)
python3 -c "
import requests, msgpack
data = msgpack.packb({'op': 'SET', 'key': 'hello', 'value': 'world'})
r = requests.post('http://localhost:8080/insert-val', data=data,
                  headers={'Content-Type': 'application/msgpack'})
print(r.status_code, r.text)          # 200 {\"ok\":true}
"

# Read a value
curl -i "http://localhost:8080/get-val?key=hello"

# Read a key that is not there
curl -i "http://localhost:8080/get-val?key=nope"   # 404 {"error":"key not found"}

# Send the same write to a follower
python3 -c "
import requests, msgpack
data = msgpack.packb({'op': 'SET', 'key': 'hello', 'value': 'world'})
r = requests.post('http://localhost:8081/insert-val', data=data,
                  headers={'Content-Type': 'application/msgpack'})
print(r.status_code, r.text)
# 503 {\"error\":\"not leader\",\"leader\":\"node1:8088\"}
"
```

### Running the tests

Three layers, each runnable on its own:

```bash
# 1. Go sidecar unit tests — no cluster, no gRPC
cd go-sidecar && test -z "$(gofmt -l .)" && go vet ./... && go test -race ./...

# 2. C++ unit tests — GoogleTest via CTest.
#    KVDB_BUILD_TESTS defaults to OFF, so the normal build and the Docker image
#    are unaffected and need no GoogleTest.
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
ctest --test-dir cpp-app/build --output-on-failure

# 3. End-to-end — requires a running cluster (see "Testing the Cluster" above)
pip install -r tests/e2e/requirements.txt
pytest tests/e2e -v

# 4. Crash recovery and snapshots — also need the LOCAL compose cluster: they
#    SIGKILL a node and wipe a volume. Deselected from the run above by
#    pytest.ini, opt in with the marker. -rs prints skip reasons.
pytest tests/e2e -m requires_docker -v -rs

# 5. TLS profile — needs the cluster brought up with docker-compose.secure.yml
#    and a CA in ./certs. Skips (never fails) when that is not the case.
./scripts/gen-certs.sh
export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d
pytest tests/e2e -m requires_secure -v -rs

# 6. Fuzzers — clang only, not part of the default build.
cmake -S cpp-app -B cpp-app/fuzz-build -DKVDB_BUILD_FUZZERS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build cpp-app/fuzz-build -j4
./cpp-app/fuzz-build/fuzz_kv_command cpp-app/fuzz/corpus/kv_command -max_total_time=60
```

**CI** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs on every
push to `main` and every pull request, with independent jobs:

| Job | Gates |
|---|---|
| `go` | `gofmt -l` (blocking), `go vet ./...`, `go test -race ./...` + coverage summary, `gosec` |
| `cpp` | `clang-format` check against `.clang-format` (blocking), cmake build with the project warning flags, `ctest` |
| `cpp-sanitizers` | the same C++ tests under `-fsanitize=address,undefined` |
| `cpp-tsan` | the same C++ tests under `-fsanitize=thread` |
| `fuzz` | both libFuzzer targets, 60s each, crash inputs uploaded as artifacts |
| `e2e` | `docker build`, `docker compose up -d`, bounded readiness poll, `pytest tests/e2e`, then the docker-gated tests, `docker compose down -v` |
| `e2e-secure` | the same cluster under `docker-compose.secure.yml`: TLS profile tests, plus a forced leader failover (the Phase 6 advertise-address bug only appears after an election) |

## API Reference

Every failure carries a real status code, the matching reason phrase on the
status line, and a JSON body naming what went wrong. Error bodies are always
`Content-Type: application/json`; a successful read is the one response that is
not JSON.

### Insert / delete a key

```http
POST /insert-val
Content-Type: application/msgpack
```

**Request body** (MsgPack **map**, all three fields present):

```json
{ "op": "SET", "key": "your_key", "value": "your_value" }
```

`op` is `SET` or `DELETE`; a `DELETE` ignores `value` but must still supply it.

**Responses**

| Outcome | Status | Body |
|---|---|---|
| Committed and applied on this node | `200 OK` | `{"ok":true}` |
| This node is not the leader | `503 Service Unavailable` | `{"error":"not leader","leader":"node1:8088"}` |
| Propose failed for any other reason (sidecar unreachable, deadline exceeded, the state machine rejected the command) | `502 Bad Gateway` | `{"error":"<reason>"}` |
| `Content-Type` is not `application/msgpack` | `415 Unsupported Media Type` | `{"error":"unsupported media type","expected":"application/msgpack"}` |
| Empty body | `400 Bad Request` | `{"error":"empty request body"}` |
| `Content-Length` is not a number | `400 Bad Request` | `{"error":"malformed Content-Length"}` |

Two things worth knowing about the failure paths:

- **`leader` is a Raft address, not a URL.** It comes from HashiCorp Raft's
  `LeaderWithID`, so under `docker-compose.yml` it is `<node-id>:8088` — the
  peer's raft port, not its HTTP port. It is empty (`"leader":""`) while an
  election is in progress. There is no leader forwarding yet: a client has to
  retry against the leader itself. Forwarding lands in Phase 4, and the
  sidecar's machine-readable `not_leader:` prefix exists so it can.
- **A command the state machine rejects is a `502`, and it was still
  replicated.** Validation happens at apply time, after the entry is committed,
  so an unknown `op` or an empty `key` costs a raft log entry and comes back as
  `502 {"error":"fsm: failed to apply raft log entry index=… term=…: empty key
  for operation \"SET\""}`. Rejecting these before proposing is future work.

### Get value by key

```http
GET /get-val?key=<key>
```

Served from the local in-memory store without consensus, so a follower may
answer with a stale value.

| Outcome | Status | Content-Type | Body |
|---|---|---|---|
| Key present | `200 OK` | `text/plain; charset=utf-8` | the stored value, verbatim |
| Key absent (never written, or deleted) | `404 Not Found` | `application/json` | `{"error":"key not found"}` |
| No `key` query parameter | `400 Bad Request` | `application/json` | `{"error":"missing required query parameter: key"}` |

Keys must be URL-safe: the query parser does no percent-decoding.

### Anything else

| Outcome | Status | Body |
|---|---|---|
| Any other method or path | `404 Not Found` | `{"error":"not found"}` |

### Cluster Management (Sidecar)

```http
GET  http://<any-node>:6000/join?peerID=<node_id>&peerAddress=<raft_address>
GET  http://<any-node>:6000/remove?peerID=<node_id>
GET  http://<any-node>:6000/status
GET  http://<any-node>:6000/health
GET  http://<any-node>:6000/ready
GET  http://<any-node>:6000/metrics
```

`/join` and `/remove` change cluster membership and **require the cluster-admin
bearer token**; the rest are read-only and unauthenticated, because container
health probes and Prometheus cannot present a credential.

```bash
curl -H "Authorization: Bearer $RAFTKV_MGMT_TOKEN" \
  "http://localhost:6000/join?peerID=node4&peerAddress=node4:8088"
```

| Situation | Status | Meaning |
|---|---|---|
| No `Authorization` header | 401 | A credential is required; `WWW-Authenticate: Bearer` is returned |
| Wrong token | 403 | The credential was presented and rejected |
| No token configured on the node | 403 | The endpoint is **disabled**, not open — see below |
| Valid token, missing parameters | 400 | Authenticated; the request itself is malformed |

The "no token configured" case is the important one: with `RAFTKV_MGMT_TOKEN`
unset, `/join` and `/remove` are closed rather than open. A cluster that forgets
to set it cannot grow — which is a much better failure than one that anybody can
join. Either node of a join may receive the request: a follower relays it to the
leader, authenticating as itself.

## Configuration

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `NODE_ID` | Unique identifier for this node | `node1` |
| `BOOTSTRAP` | Set to `true` for the initial leader | `false` |
| `JOIN_ADDR` | Leader's management address for joining | - |
| `RAFTKV_MGMT_TOKEN` | Cluster-admin bearer token for `/join` and `/remove`. Unset **disables** those endpoints | - |
| `RAFT_TLS_CERT` / `RAFT_TLS_KEY` / `RAFT_TLS_CA` | Mutual TLS for the Raft peer transport. Unset means plaintext | - |
| `MGMT_TLS_CERT` / `MGMT_TLS_KEY` / `MGMT_TLS_CA` | TLS for the management API. Unset means HTTP | - |
| `SNAPSHOT_INTERVAL` / `SNAPSHOT_THRESHOLD` / `TRAILING_LOGS` | Raft snapshot tunables. Unset uses HashiCorp Raft's defaults | - |

The token is read from the environment rather than a flag on purpose: a
`-mgmt-token <secret>` would put it in the process's command line, where any
local user can read it out of `ps`. TLS **paths** are passed as flags, because
they are not secrets.

### Port Mapping

| Port | Service | Description |
|------|---------|-------------|
| 8080 | HTTP API | Client-facing REST API |
| 8088 | Raft | Raft consensus protocol |
| 6000 | Management | Cluster join/leave operations |
| 50051 | gRPC | C++ StateMachine service — bound to `127.0.0.1`, never published |
| 50052 | gRPC | Go RaftNode service — peer-reachable (write/read forwarding) |
| 8443 | HTTPS | Client API via the TLS proxy, in the secure profile only |

50051 is bound to loopback (R6.6): its only caller is this node's own sidecar,
and it is an unauthenticated interface that can read and overwrite the entire
store. 50052 is deliberately *not* loopback-only — Phase 4 made it the target of
write and read forwarding from peers.

## Security

Security is **opt-in by configuration** and the default `docker compose up` is
deliberately insecure: a demo that needs a CA before it prints anything is a demo
nobody runs. The supported secure deployment is `docker-compose.secure.yml`, and
it is exercised by its own CI job.

```bash
./scripts/gen-certs.sh                                # dev CA + node certs -> ./certs
export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d

curl --cacert certs/ca.pem https://localhost:6000/status
curl --cacert certs/ca.pem -X PUT --data-binary 'v' https://localhost:8443/kv/k
```

`scripts/gen-certs.sh` is a development helper, not a CA — it writes unencrypted
keys and has no revocation or rotation story. In a real deployment the
certificates should come from whatever already issues them there; the sidecar
only takes three file paths.

### What each surface gets

| Surface | Port | Default profile | Secure profile |
|---|---|---|---|
| Client HTTP API | 8080 / 8443 | Plaintext, unauthenticated | HTTPS via a reverse proxy, **still unauthenticated** |
| Management API | 6000 | HTTP; `/join`+`/remove` behind a bearer token | HTTPS + bearer token |
| Raft peer transport | 8088 | Plaintext, **anyone who can reach it can append entries** | Mutual TLS against one cluster CA |
| C++ StateMachine gRPC | 50051 | Plaintext on `127.0.0.1` | Same — see below |
| Sidecar RaftNode gRPC | 50052 | Plaintext, peer-reachable | Same — see below |

### What is deliberately not protected

Stated plainly, because a security section that only lists wins is worse than
none at all:

- **There is no client authentication, in either profile.** Anyone who can reach
  the client API can read and write every key. The bearer token guards cluster
  *membership*, not data. Put the API behind something that authenticates.
- **The intra-node gRPC pair (50051/50052) is plaintext.** 50051 never leaves the
  container's loopback interface, so encrypting it would buy nothing. 50052 is
  reachable by peers, because Phase 4 forwarding made it so — a peer that can
  reach it can propose writes. It is inside the same trust boundary as the Raft
  port, but unlike the Raft port it is not authenticated. This is a real gap.
- **The proxy-to-node hop is plaintext** (see `deploy/caddy/Caddyfile`). It is
  acceptable only because both ends are in the same compose network; a proxy on a
  different host would give you encryption to the proxy and clear text for the
  rest of the way.
- **Certificates do not rotate.** Restart a node to pick up a new one.
- **There is no authorization model** — no per-key ACLs, no roles. One
  cluster-admin credential, and that is all.

### Why TLS is terminated by a proxy (R6.5)

The C++ engine's HTTP server is a hand-rolled accept loop over raw sockets.
Adding a TLS state machine to it would put certificate parsing and session
handling — a large, historically vulnerable surface — inside the process that
owns the data, to reimplement what almost every deployment already runs in front
of it. The proxy keeps the engine dependency-light and the TLS code maintained by
someone else. The trade-off is the plaintext hop noted above.

### Hardening in the code itself

- Every byte read off disk or off a socket is treated as untrusted and
  bounds-checked before it is used to index or allocate. Fuzzing this boundary
  found a 17-byte MsgPack payload that made a node try to allocate over 512 MiB —
  and because decoding happens *after* Raft commits, it took down every replica
  and came back on every restart. `KVCommand::from_msgpack` now decodes under an
  explicit `msgpack::unpack_limit`.
- `cpp-app/fuzz/` holds libFuzzer targets for both input boundaries
  (`KVCommand::from_msgpack`, `HttpRequestParser::parse`); CI runs each for 60s
  per PR with ASan+UBSan.
- Request caps: 1 MiB body (413), 32 KiB headers (431).
- CI runs `gosec` on the Go tree and the C++ tests under ASan/UBSan and TSan.

## Data directory

Each node keeps its whole state in one directory — `/app/data` in the container
(`DATA_DIR` in `entrypoint.sh`), bind-mounted from `./vol-node<N>` by
`docker-compose.yml`. Four files, two owners:

| File | Owner | Purpose |
|---|---|---|
| `kv.db` | C++ | Base file: the whole key-value map, in the binary `KVB1` format |
| `kv.wal` | C++ | Write-ahead log: every applied command, `fsync`ed before it is acknowledged |
| `kv.db.tmp` | C++ | Transient: the in-progress base file, renamed over `kv.db` |
| `logs.dat` | Go | BoltDB raft log and stable store (HashiCorp Raft) |

### `kv.db` — base file (`KVB1`)

```
"KVB1"                                    4 bytes, magic
uint32  entry_count
entry_count × (
    uint32  key_len   | key_len bytes
    uint32  value_len | value_len bytes
)
```

All integers are little-endian. Nothing is escaped or delimited, so keys and
values may contain `=`, newlines and NUL bytes — the format this replaced was
line-based `key=value\n` and silently corrupted all three. A file that does not
start with the magic is read once with the old line parser and rewritten in this
format immediately, so migration is automatic and happens at most once.

The file is never modified in place: the new contents are written to
`kv.db.tmp`, that file is `fsync`ed, renamed over `kv.db`, and the directory
itself is `fsync`ed. A reader therefore sees either the entire old file or the
entire new one. A leftover `kv.db.tmp` after a crash is expected and harmless —
nothing ever reads it, and the next rewrite replaces it.

`kv.db` is written only by compaction, not by every write.

```bash
xxd vol-node1/kv.db | head     # 4b 56 42 31 … = "KVB1"
```

### `kv.wal` — write-ahead log

```
repeated until EOF:
    uint32  payload_len | payload_len bytes | uint32  crc32(payload)
```

`payload` is the MsgPack-encoded command (`{op, key, value}`) — the same shape
the client sends. CRC32 is the standard IEEE/zlib polynomial. Every `SET` and
`DELETE` appends one record and `fsync`s it **before** the in-memory map
changes, which is what makes an acknowledged write survive `kill -9`; it also
makes an apply cost one record instead of a full rewrite of the store.

**Recovery** on startup is "load `kv.db`, then replay `kv.wal` over it in
order". The first damaged record — a short read, a length that overruns the
file, or a CRC mismatch — ends the replay, and the file is truncated back to the
end of the last intact record. That damaged record is the torn tail a crash
part-way through an append leaves behind: everything before it is applied,
everything after it is discarded, because once the framing is lost the following
bytes cannot be trusted to be the records that were meant to follow.

**Compaction** runs when the WAL passes 4 MiB or 10,000 records (defaults in
`DurabilityOptions`, `cpp-app/src/config/config.hpp`; also home to the
`always`/`never` WAL fsync mode). It writes a fresh base file and *then*
truncates the WAL — that order is the correctness argument, since a crash
between the two costs only a replay of records already folded into the base
file, and replaying a `SET`/`DELETE` twice is idempotent.

Since Phase 3 a restart applies the newest **snapshot** and then only the log
entries after it, rather than replaying the entire history. Each of those
applies still appends to the WAL, so the WAL grows a little on every restart
until the next compaction folds it away — but it is now bounded by the snapshot
interval rather than by the age of the cluster.

### `logs.dat`

Owned by the Go sidecar: `raftnode.New` uses one bbolt file (via
`raft-boltdb/v2`) as both the raft log store and the stable store. Deleting it
erases the node's raft identity and log; deleting `kv.db`/`kv.wal` erases its
data. For a clean slate,
`docker compose down -v && rm -rf vol-node1 vol-node2 vol-node3`.

### `snapshots/`

Owned by the Go sidecar: `raft.NewFileSnapshotStore` keeps the two most recent
snapshots here, each a directory holding `meta.json` and `state.bin`. The
payload is byte-for-byte the same `KVB1` encoding as `kv.db` — one format for
disk and for the wire — produced by the C++ engine over the `GetSnapshot`
stream and pushed back over `RestoreSnapshot`.

Snapshotting is what bounds the raft log, and it is tunable from the sidecar's
flags (see `internal/config`), each forwarded by `entrypoint.sh` only when the
matching environment variable is set:

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `-snapshot-interval` | `SNAPSHOT_INTERVAL` | `120s` | How often a node checks whether a snapshot is due |
| `-snapshot-threshold` | `SNAPSHOT_THRESHOLD` | `8192` | Applied entries since the last snapshot before taking a new one |
| `-trailing-logs` | `TRAILING_LOGS` | `10240` | Entries kept *behind* a snapshot |

**`-trailing-logs` is the one that actually shrinks the log.** Raft truncates to
`snapshot_index - TrailingLogs`, so leaving it at the default means the log
never shrinks however often the node snapshots. `docker-compose.test.yml`
lowers all three so `tests/e2e/test_snapshot.py` can observe a real snapshot and
a real truncation inside one test run:

```bash
docker compose -f docker-compose.yml -f docker-compose.test.yml up -d
pytest tests/e2e -m requires_docker -v -rs
```

Compaction is observable on the management API: `/status` reports
`first_log_index`, `last_log_index`, `applied_index`, `commit_index` and
`last_snapshot_index`, which is how the e2e tests prove the log was really
truncated rather than that a snapshot file merely appeared.

## Project Structure

```
RaftKV/
├── cpp-app/                 # C++ Storage Engine (header-only; main.cpp bootstraps)
│   ├── src/
│   │   ├── main.cpp         # Wiring only
│   │   ├── commands/        # KVCommand (MsgPack wire format)
│   │   ├── config/          # CLI args
│   │   ├── network/         # HTTP request parser + server
│   │   ├── raft/            # Propose client + StateMachine gRPC service
│   │   └── storage/         # PersistentKVStore, WAL, atomic file write,
│   │                        #   binary format primitives (see Data directory)
│   ├── tests/               # GoogleTest unit tests (-DKVDB_BUILD_TESTS=ON)
│   ├── fuzz/                # libFuzzer targets + corpora (-DKVDB_BUILD_FUZZERS=ON)
│   ├── CMakeLists.txt       # Build configuration
│   └── pb/                  # Stale generated Protobuf copy (unused by the build)
├── go-sidecar/              # Go Raft Sidecar (module: my-raft-sidecar)
│   ├── cmd/sidecar/main.go  # Wiring only
│   ├── internal/            # backend, cluster, config, fsm, logging, management,
│   │                        #   metrics, peers, raftnode, rpc, testcerts,
│   │                        #   tlsconfig (+ *_test.go)
│   ├── go.mod               # Go module dependencies
│   └── pb/                  # Generated Protobuf files (checked in)
├── proto/
│   └── consensus.proto      # Service definitions
├── tests/e2e/               # pytest end-to-end suite (needs a running cluster;
│                            #   test_crash.py / test_snapshot.py are opt-in with
│                            #   -m requires_docker, test_secure_profile.py with
│                            #   -m requires_secure)
├── scripts/gen-certs.sh     # Development CA + node certificates -> ./certs
├── deploy/caddy/Caddyfile   # TLS termination for the client API (secure profile)
├── docs/phases/             # Roadmap phase specs and plans
├── .github/workflows/ci.yml # CI: go, cpp, cpp-sanitizers, cpp-tsan, fuzz, e2e,
│                            #   e2e-secure
├── .clang-format            # C++ formatting (enforced by the cpp CI job)
├── docker-compose.yml       # Multi-node cluster setup (plaintext demo)
├── docker-compose.secure.yml # TLS everywhere + a terminating proxy
├── docker-compose.test.yml  # Aggressive snapshot tunables for the e2e suite
├── Dockerfile               # Multi-stage build
├── entrypoint.sh            # Container startup script
├── ROADMAP.md               # Path to 1.0
└── test_client.py           # Manual demo client (superseded by tests/e2e)
```

## Building from Source

### C++ Storage Engine

```bash
cd cpp-app
mkdir build && cd build
cmake ..
make -j$(nproc)
```

**Dependencies:**
- CMake 3.20+ (3.15 is enough to build `kvdb_node`; `ctest --test-dir` needs 3.20)
- gRPC and Protocol Buffers
- MsgPack for C++ (`libmsgpack-dev`)

### Go Sidecar

```bash
cd go-sidecar
go build -o sidecar ./cmd/sidecar
```

**Dependencies:**
- Go 1.24+
- HashiCorp Raft
- gRPC for Go

## How It Works

### Raft Consensus

RaftKV uses the Raft algorithm to maintain consistency across nodes:

1. **Leader Election** — One node is elected leader; it handles all write requests
2. **Log Replication** — The leader appends entries to its log and replicates to followers
3. **Commit** — Once a majority acknowledge, the entry is committed
4. **Apply** — Committed entries are applied to each node's state machine

### Sidecar Pattern

The sidecar architecture decouples the storage logic from consensus:

- **C++** handles performance-critical storage operations
- **Go** leverages the mature HashiCorp Raft implementation
- **gRPC** provides efficient communication between them

This design allows each component to be optimized independently while maintaining clear interfaces.

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## License

This project is open source and available under the [MIT License](LICENSE).
