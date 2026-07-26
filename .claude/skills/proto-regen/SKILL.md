---
name: proto-regen
description: Regenerate gRPC/protobuf stubs after editing proto/consensus.proto — Go stubs must be regenerated manually and committed; C++ stubs regenerate automatically at build time. Use whenever the proto contract between cpp-app and go-sidecar changes.
---

# Regenerate Protobuf Stubs

`proto/consensus.proto` is the contract between the C++ engine and the Go sidecar. The two sides handle generated code differently.

## Go (manual — stubs are checked in)

```bash
# Prerequisites on PATH: protoc, protoc-gen-go, protoc-gen-go-grpc
# If missing:
#   go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
#   go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
#   (ensure "$(go env GOPATH)/bin" is on PATH)

cd go-sidecar
protoc -I ../proto --go_out=. --go-grpc_out=. ../proto/consensus.proto
```

Output lands in `go-sidecar/pb/` (driven by `option go_package = "./pb";`). Commit the regenerated files. Then verify:

```bash
test -z "$(gofmt -l .)" && go vet ./... && go build -o /dev/null ./cmd/sidecar
go test -race ./...
```

## C++ (automatic — do nothing)

CMake runs `protoc` at build time into `cpp-app/build/proto/` (see the `add_custom_command` in `cpp-app/CMakeLists.txt`). A rebuild picks up the change:

```bash
cd cpp-app/build && cmake .. && make -j4
```

**Do not touch `cpp-app/pb/`** — it is a stale, unreferenced copy of generated code; the build never reads it.

## After regenerating

1. Update both sides' code for the new fields/RPCs (`cpp-app/src/raft/`, `go-sidecar/internal/rpc/`, `go-sidecar/internal/fsm/`).
2. Mind compatibility: committed raft log entries are replayed through `StateMachine.Apply` on restart, so the apply path must still decode old payloads. Never renumber existing field tags.
3. Keep the client-visible contract in step: if the change alters what a client sees, update the README API tables and `tests/e2e/contracts.py` in the same commit (`contracts.py` is the suite's single source of truth for status codes and bodies).
4. Run the full verification: invoke the `cluster-smoke-test` skill — `docker build`, `docker compose up -d`, then the asserting end-to-end suite:

   ```bash
   pip install -r tests/e2e/requirements.txt   # once
   pytest tests/e2e -v
   ```

   Not `test_client.py`: it asserts nothing and only touches one node.
