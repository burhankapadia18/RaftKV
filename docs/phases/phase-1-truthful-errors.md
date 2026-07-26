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

## Outcome

**Status: ✅ Complete.** All eleven requirements landed and were verified against a
real 3-node cluster, not just in unit tests.

### Verified results

| Layer | Result |
|---|---|
| Go | `gofmt` clean, `go vet` clean, `go test -race` stable over 2 consecutive runs |
| Go coverage | `fsm` 100%, `rpc` 100% (new), `management` 97.1%, `cluster` 96.2%, `config` 87.5%, `backend` 87.2% (new) |
| C++ | **97 tests**, 100% pass via CTest — up from 56 in Phase 0 |
| C++ sanitizers | 97/97 under `-fsanitize=address,undefined`, via the FetchContent GoogleTest path CI actually takes |
| clang-format | gate clean across `cpp-app/src` and `cpp-app/tests` |
| e2e | **9/9** against a clean cluster built from the Phase 1 image (was 4) |

Still untested on the Go side: `internal/raftnode` and `cmd/sidecar`.

### The HTTP contract, confirmed by `curl -i` against a live cluster

```
SET on leader        200 application/json          {"ok":true}
SET on follower      503 Service Unavailable       {"error":"not leader","leader":"172.18.0.4:8088"}
GET hit              200 text/plain; charset=utf-8 <raw value>
GET miss             404 Not Found                 {"error":"key not found"}
GET without ?key=    400 Bad Request               {"error":"missing required query parameter: key"}
wrong Content-Type   415 Unsupported Media Type    {"error":"unsupported media type","expected":"application/msgpack"}
empty body           400 Bad Request               {"error":"empty request body"}
unknown route        404 Not Found                 {"error":"not found"}
```

Reason phrases are correct on the status line — R1.7 fixed the hardcoded
`HTTP/1.1 404 OK`. The `leader` field is raft's resolved peer address
(`172.18.0.4:8088`), not the HTTP base URL, because `createTransport` resolves the
advertised name to a TCP address.

### The bug Phase 0 found, now fixed

`Content-Length: abc` used to be a **remote denial of service**: `std::stoi` threw,
the exception unwound out of `handle_connection` and `run()` into `main.cpp`'s
catch-all, and the node exited 1 — verified in Phase 0 by moving a node to
`Exited (1)` with a single unauthenticated request. It now returns
`400 {"error":"malformed Content-Length"}` and all three nodes stay `Up`;
`test_r1_8_malformed_content_length_is_rejected_without_killing_the_node` guards it.

### Invalid commands are now reported instead of silently applied

```
empty key    502 {"error":"fsm: failed to apply raft log entry index=12 term=2: empty key for operation \"SET\""}
unknown op   502 ... unknown operation: "FROB"
bad msgpack  502 ... rpc error: code = Internal desc = parse error
```

`{op:"SET", key:""}` previously ran `store_.set("", value)` and reported success.

### Decisions taken during implementation

- **Propose timeouts were racing.** Both sides used 5s, so the C++ gRPC deadline
  usually fired before the sidecar could answer and a slow commit surfaced as a bare
  `DEADLINE_EXCEEDED` instead of the structured reason this phase exists to produce.
  The Go `proposeTimeout` is now 4s, below the C++ 5s deadline; both sides carry a
  comment pointing at the other.
- **"Divergence" is now used accurately.** A `Success == false` rejection is a pure
  function of the entry's bytes, so every replica rejects it identically and the
  cluster stays consistent — it is logged as a deterministic rejection, not as
  divergence. The transport-failure and nil-response branches, where this node may
  genuinely differ from peers, are the ones that warn about divergence.
- **Only `raft.ErrNotLeader` carries the `not_leader:` prefix**, exactly as R1.5
  specifies. `ErrLeadershipLost` and `ErrLeadershipTransferInProgress` therefore
  report as 502 rather than 503. That is arguably wrong for a client that could
  usefully retry, but those errors also leave the write's outcome *uncertain*, which
  is a different thing from "definitely not applied" — see Known issues.
- **Malformed msgpack still returns gRPC `INTERNAL`**, so its `ApplyResponse.error`
  never reaches the sidecar. No information is lost: the reason travels in the gRPC
  status message instead, and the FSM surfaces it. The field is set anyway so the
  unit tests can assert on it in-process.

### Known issues this phase deliberately did not fix

- **Validation happens after commit.** An invalid command is accepted by `Propose`,
  replicated, committed, and only then rejected by every state machine. The bogus
  entry is permanent in the log, consumes an index, and is re-rejected (and re-logged)
  on every restart replay. Moving validation to the propose boundary would give a 400
  and keep the log clean, but it means the C++ HTTP layer parsing the msgpack body —
  which today it deliberately forwards opaquely (see CLAUDE.md). That is an
  architectural change, not a Phase 1 one.
- **Client-controlled `op` strings are echoed into error bodies and logs** quoted and
  JSON-escaped, but not length-capped. Body/header caps are R4.9.
