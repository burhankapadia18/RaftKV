# RaftKV

<p align="center">
  <strong>A lightweight distributed key-value store built on Raft consensus</strong>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#architecture">Architecture</a> •
  <a href="#quick-start">Quick Start</a> •
  <a href="#api-reference">API</a> •
  <a href="#configuration">Configuration</a>
</p>

---

## Overview

**RaftKV** is a distributed key-value store that combines a high-performance C++ storage engine with the reliability of the [Raft consensus algorithm](https://raft.github.io/). The system uses a sidecar architecture where cluster coordination and replication are handled by a Go-based component using [HashiCorp Raft](https://github.com/hashicorp/raft), ensuring strong consistency with minimal coupling between components.

## Features

- **High Performance** — C++ storage engine with in-memory operations and persistent storage
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
│  │      (kv.db)            │    │    │   (Port 6000)         │  │
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
5. **Persist** → C++ engine updates in-memory store and writes to disk

## Quick Start

### Prerequisites

- Docker and Docker Compose
- (For local development) Go 1.24+, CMake, gRPC/Protobuf libraries

### Running with Docker Compose

```bash
# Build the Docker image
docker build -t raftkv:latest .

# Start a 3-node cluster
docker-compose up -d

# View logs
docker-compose logs -f
```

### Testing the Cluster

```bash
# Install Python dependencies
pip install requests msgpack

# Run the test client
python test_client.py
```

Or use curl directly:

```bash
# Write a value (using Python for MsgPack encoding)
python3 -c "
import requests, msgpack
data = msgpack.packb({'op': 'SET', 'key': 'hello', 'value': 'world'})
print(requests.post('http://localhost:8080/insert-val', data=data, 
      headers={'Content-Type': 'application/msgpack'}).text)
"

# Read a value
curl "http://localhost:8080/get-val?key=hello"
```

## API Reference

### Insert Key-Value Pair

```http
POST /insert-val
Content-Type: application/msgpack
```

**Request Body** (MsgPack encoded):
```json
{
  "op": "SET",
  "key": "your_key",
  "value": "your_value"
}
```

**Response**: `ok` on success, `error` on failure

### Get Value by Key

```http
GET /get-val?key=<key>
```

**Response**: The value associated with the key, or `Key Not Found`

### Delete Key (via SET operation)

```http
POST /insert-val
Content-Type: application/msgpack
```

**Request Body** (MsgPack encoded):
```json
{
  "op": "DELETE",
  "key": "your_key",
  "value": ""
}
```

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

## Project Structure

```
RaftKV/
├── cpp-app/                 # C++ Storage Engine
│   ├── main.cpp             # HTTP server, gRPC service, KV store
│   ├── CMakeLists.txt       # Build configuration
│   └── pb/                  # Generated Protobuf files
├── go-sidecar/              # Go Raft Sidecar
│   ├── main.go              # Raft setup, gRPC service
│   ├── go.mod               # Go module dependencies
│   └── pb/                  # Generated Protobuf files
├── proto/
│   └── consensus.proto      # Service definitions
├── docker-compose.yml       # Multi-node cluster setup
├── Dockerfile               # Multi-stage build
├── entrypoint.sh            # Container startup script
└── test_client.py           # Python test client
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
- CMake 3.10+
- gRPC and Protocol Buffers
- MsgPack for C++ (`libmsgpack-dev`)

### Go Sidecar

```bash
cd go-sidecar
go build -o sidecar .
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
