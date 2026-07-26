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

# 4. Crash recovery — also needs the LOCAL compose cluster: it SIGKILLs a node.
#    Deselected from the run above by pytest.ini, opt in with the marker.
pytest tests/e2e -m requires_docker -v
```

**CI** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs on every
push to `main` and every pull request, with four independent jobs:

| Job | Gates |
|---|---|
| `go` | `gofmt -l` (blocking), `go vet ./...`, `go test -race ./...` + coverage summary |
| `cpp` | `clang-format` check against `.clang-format` (blocking), cmake build with the project warning flags, `ctest` |
| `cpp-sanitizers` | the same C++ tests under `-fsanitize=address,undefined` |
| `e2e` | `docker build`, `docker compose up -d`, bounded readiness poll, `pytest tests/e2e`, then the crash test (`-m requires_docker`), `docker compose down -v` |

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
GET http://<leader>:6000/join?peerID=<node_id>&peerAddress=<raft_address>
```

Adds a new node to the Raft cluster.

## Configuration

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `NODE_ID` | Unique identifier for this node | `node1` |
| `BOOTSTRAP` | Set to `true` for the initial leader | `false` |
| `JOIN_ADDR` | Leader's management address for joining | - |

### Port Mapping

| Port | Service | Description |
|------|---------|-------------|
| 8080 | HTTP API | Client-facing REST API |
| 8088 | Raft | Raft consensus protocol |
| 6000 | Management | Cluster join/leave operations |
| 50051 | gRPC | C++ StateMachine service |
| 50052 | gRPC | Go RaftNode service |

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

Note that a restart replays the *entire* raft log through the state machine
(there are no snapshots yet), and each of those applies appends to the WAL
again — so the WAL grows on every restart until the next compaction folds it
away.

### `logs.dat`

Owned by the Go sidecar: `raftnode.New` uses one BoltDB file as both the raft
log store and the stable store. Deleting it erases the node's raft identity and
log; deleting `kv.db`/`kv.wal` erases its data. For a clean slate,
`docker compose down -v && rm -rf vol-node1 vol-node2 vol-node3`.

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
│   ├── CMakeLists.txt       # Build configuration
│   └── pb/                  # Stale generated Protobuf copy (unused by the build)
├── go-sidecar/              # Go Raft Sidecar (module: my-raft-sidecar)
│   ├── cmd/sidecar/main.go  # Wiring only
│   ├── internal/            # backend, cluster, config, fsm, management,
│   │                        #   raftnode, rpc (+ *_test.go)
│   ├── go.mod               # Go module dependencies
│   └── pb/                  # Generated Protobuf files (checked in)
├── proto/
│   └── consensus.proto      # Service definitions
├── tests/e2e/               # pytest end-to-end suite (needs a running cluster;
│                            #   test_crash.py is opt-in: -m requires_docker)
├── docs/phases/             # Roadmap phase specs and plans
├── .github/workflows/ci.yml # CI: go, cpp, cpp-sanitizers, e2e
├── .clang-format            # C++ formatting (enforced by the cpp CI job)
├── docker-compose.yml       # Multi-node cluster setup
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
