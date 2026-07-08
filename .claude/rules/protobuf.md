# Protobuf / gRPC Contract Rules

`proto/consensus.proto` defines the only interface between the C++ engine and the Go sidecar:

- `RaftNode.Propose(Command) → ProposeResponse` — C++ → Go (port 50052)
- `StateMachine.Apply(Command) → ApplyResponse` — Go → C++ (port 50051)

## The `Command.data` convention

The KV payload travels as **opaque MsgPack bytes in `Command.data`**, end to end: HTTP body → Propose → raft log → Apply. The `op`/`key`/`value` proto fields exist but are unused in transit. Do not start populating them without migrating both sides and considering old raft log entries — committed log entries are replayed through `StateMachine.Apply` on restart, so the apply path must remain able to decode every format ever written.

## When you change the proto

1. Edit `proto/consensus.proto`.
2. **C++**: nothing to regenerate manually — CMake runs `protoc` at build time into `cpp-app/build/proto/`. The checked-in `cpp-app/pb/` is a stale copy that the build does not reference; leave it alone (or delete it in a dedicated cleanup, updating nothing else).
3. **Go**: stubs are checked in and must be regenerated:

   ```bash
   # requires protoc, protoc-gen-go, protoc-gen-go-grpc on PATH
   cd go-sidecar
   protoc -I ../proto --go_out=. --go-grpc_out=. ../proto/consensus.proto
   ```

   The `option go_package = "./pb";` in the proto places output in `go-sidecar/pb/`.
4. Rebuild both sides and run the cluster smoke test (`docker build`, `docker-compose up -d`, `python test_client.py`).

## Compatibility rules

- Never renumber or reuse field tags; only add new fields with fresh tags.
- Additions must be backward compatible: a node with the new binary must still apply raft log entries written by the old binary.
- Keep `proto/consensus.proto`, the README API section, and `test_client.py` consistent with any payload change.
