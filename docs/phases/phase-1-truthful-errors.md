# Phase 1 — Truthful Errors

## Spec

### Goals
Every failure path reports what actually happened, with the right status code and a
machine-readable reason. Fix the latent correctness bugs found in review: the FSM
ignoring apply failures, dead validation, the hardcoded `OK` reason phrase, and the
no-op backend connect retry.

### Non-goals
- No leader forwarding, no new endpoints, no API shape change (Phase 4).
- No durability changes (Phase 2).

### Requirements

**Proto contract:**
- R1.1 `ApplyResponse` gains `string error = 2` (new tag — backward compatible per
  `.claude/rules/protobuf.md`). Go stubs regenerated and committed; C++ regenerates at build.

**Apply path (C++ `state_machine.hpp`):**
- R1.2 `StateMachineService::Apply` calls `KVCommand::is_valid()` after decode; invalid
  commands return `success=false` with a descriptive `error` (op, key presence). The
  existing try/catch for malformed msgpack stays and also populates `error`.
- R1.3 Unknown op is a validation failure (today it falls through with `success=false`
  but gRPC `OK` and no reason).

**FSM contract (Go `internal/fsm/fsm.go`):**
- R1.4 `CppFSM.Apply` returns a non-nil error object when the gRPC call fails **or**
  `ApplyResponse.Success` is false, including `resp.Error`, and logs it loudly. This
  closes the silent-divergence bug.
- R1.5 `rpc.Server.Propose` inspects the value returned by `raft.Apply` — if the FSM
  returned an error, `ProposeResponse{Success:false, Error:...}`. Distinguish
  `raft.ErrNotLeader` with a stable error string prefix (`not_leader:` + leader address
  from `LeaderWithID`) so Phase 4 forwarding can build on it. The existing contract
  (failure in body, nil gRPC error) is kept — the C++ client checks `reply.success()`.

**Propose result (C++ `raft_client.hpp`):**
- R1.6 `IRaftClient::propose` returns a `ProposeResult { bool success; std::string error; }`
  instead of `bool`, surfacing `reply.error()` and gRPC status details (deadline,
  unavailable) instead of collapsing everything to `false`.

**HTTP truthfulness (C++ `http_request.hpp` / `http_server.hpp`):**
- R1.7 `HttpResponse::to_string()` emits the correct reason phrase per status code
  (table: 200/201/400/404/415/500/502/503).
- R1.8 Status mapping in `KVHttpHandler`:
  - missing key → **404**, body `{"error":"key not found"}`
  - wrong/missing content type → **415**
  - malformed request (bad Content-Length, empty body) → **400** — and `std::stoi` is
    wrapped so garbage no longer crashes the server
  - propose failed, not-leader → **503** with `{"error":"not leader","leader":"..."}`
  - propose failed, other → **502** with the propose error string
  - success → **200** `{"ok":true}`
  Error bodies are JSON with `Content-Type: application/json`.

**Startup rigor:**
- R1.9 `go-sidecar/internal/backend/client.go`: replace deprecated `grpc.Dial` with
  `grpc.NewClient` plus an explicit readiness wait (`conn.Connect()` +
  `WaitForStateChange` loop or a probe RPC) so the 15-retry loop actually retries
  against a dead backend.
- R1.10 `entrypoint.sh`: replace `sleep 2` with a probe loop on the C++ gRPC port
  (bash `/dev/tcp/localhost/50051` with timeout) before starting the sidecar.

**Consistency of docs and tests:**
- R1.11 README API section, `tests/e2e`, and the Phase 0 pinned tests updated to the
  new status codes in the same change (wire-format rule: payload contract, README,
  and client move together).

### Acceptance criteria
- No failure path returns `200`; `curl` to a follower write returns 503 naming the leader.
- Malformed msgpack or an empty key produces `success=false` + error visible in the
  sidecar log, and the raft apply is reported failed — verified by a Go fsm unit test
  and a C++ apply unit test.
- Sidecar started with the C++ app down retries and connects only when the app is up
  (verified by a unit/integration test with a delayed listener).
- Full e2e suite (updated) green; cluster smoke test green.

## Plan

1. **Proto**: add `error = 2` to `ApplyResponse` in `proto/consensus.proto`; regenerate
   Go stubs (`protoc -I ../proto --go_out=. --go-grpc_out=. ../proto/consensus.proto`
   from `go-sidecar/`); commit stubs.
2. **C++ apply**: wire `is_valid()` + error strings into `StateMachineService::Apply`
   (`cpp-app/src/raft/state_machine.hpp`); extend the Phase 0 unit tests to assert
   `success=false` + error text for invalid/malformed/unknown-op.
3. **Go fsm**: honor `resp.Success`/`resp.Error` in `CppFSM.Apply`; update
   `internal/fsm` tests (flip the pinned behavior from Phase 0).
4. **Go rpc**: in `internal/rpc/server.go`, surface FSM apply errors from
   `ApplyFuture.Response()` and tag not-leader errors with the leader address.
5. **C++ propose**: change `IRaftClient`/`GrpcRaftClient` to return `ProposeResult`
   (`cpp-app/src/raft/raft_client.hpp`); update `KVHttpHandler` call site.
6. **HTTP layer**: reason-phrase table + new factories in `http_request.hpp`;
   status mapping + JSON error bodies + `stoi` hardening in `http_server.hpp`;
   update `http_request_test.cpp`.
7. **Startup**: rewrite `backend.Connect` (R1.9) with a unit test against a
   late-binding `net.Listener`; probe loop in `entrypoint.sh` (R1.10).
8. **Sync docs/tests**: README API tables, `tests/e2e` expectations (R0.3/R0.4 pins
   become 404/503 assertions), CLAUDE.md "Errors are stringly reported" limitation removed.

### Verification
- `go test -race ./...`, `ctest`, then `/cluster-smoke-test`.
- Manual: `curl -i` each error path against the live cluster (follower write, missing
  key, wrong content type) and confirm status lines and JSON bodies.
