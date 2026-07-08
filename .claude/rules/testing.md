# RaftKV Testing Rules

## Current state

There is **no unit test suite yet**. The only verification is `test_client.py`, an end-to-end smoke test that requires a running docker-compose cluster. Until unit tests exist, any nontrivial change must be verified against the real cluster:

```bash
docker build -t raftkv:latest .
docker-compose up -d
python test_client.py        # pip install requests msgpack
docker-compose logs -f       # check [StateMachine] Applied lines on ALL nodes
docker-compose down          # rm -rf vol-node* for a clean slate
```

A change is only verified when the write is applied on every node (each node's log shows the `Apply`) and a follower (`localhost:8081` / `8082`) can serve the read.

## When adding tests

The code was structured for testability — use the seams that already exist:

- **Go** (preferred starting point, standard tooling): table-driven tests with `go test -race ./...`. `fsm.CppFSM` takes the `StateMachineClient` interface — test it with a fake, no gRPC needed. `config`, `cluster.Joiner` (against `httptest.Server`), and `management.Server` handlers are all unit-testable today.
- **C++**: GoogleTest via CMake/CTest. `PersistentKVStore` (against a temp file), `KVCommand::from_msgpack` (valid/malformed/unknown-op), and `KVHttpHandler` (with fake `IRaftClient`/`IKVStore`) are the high-value targets. Wire tests as a separate CMake target so `kvdb_node` stays dependency-free.
- **Cluster behavior** (leader election, follower writes failing, restart replay) belongs in scripted docker-compose scenarios, not unit tests.

## What matters most to cover

1. MsgPack decode edge cases in `KVCommand` — this is the trust boundary for all client input.
2. Store persistence round-trip, including keys/values containing `=` and newlines (the current line-based `kv.db` format is fragile — tests should pin down actual behavior).
3. Propose failure paths: sidecar down, non-leader, timeout (the C++ client has a 5s deadline).
