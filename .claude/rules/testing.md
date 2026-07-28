# RaftKV Testing Rules

## Current state

Three test layers exist (Phase 0). All of them are gated by CI
(`.github/workflows/ci.yml`), and all of them must stay green.

**1. Go unit tests** — `go-sidecar/internal/*/*_test.go`, no gRPC and no cluster
needed:

```bash
cd go-sidecar
test -z "$(gofmt -l .)" && go vet ./... && go test -race ./...
```

Covered today: `internal/fsm`, `internal/management`, `internal/cluster`,
`internal/config` — all comfortably above the 80% bar. Live per-package numbers
are published by the `go` CI job's coverage summary; a point-in-time snapshot is
recorded in
[docs/phases/phase-0-tests-and-ci.md](../../docs/phases/phase-0-tests-and-ci.md).
Deliberately not repeated here, so this file cannot drift. Not yet covered:
`internal/backend`, `internal/raftnode`, `internal/rpc`, `cmd/sidecar`, `pb`.

**2. C++ unit tests** — GoogleTest via CTest, in `cpp-app/tests/`:

```bash
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
ctest --test-dir cpp-app/build --output-on-failure
```

`KVDB_BUILD_TESTS` defaults to **OFF**, so the normal `kvdb_node` build and the
Docker image are unaffected and need no GoogleTest. When the option is on, the
build uses the system GoogleTest (Debian `libgtest-dev`) if it is installed and
falls back to `FetchContent` otherwise. CI runs the same tests a second time
under `-fsanitize=address,undefined`.

**3. End-to-end** — `tests/e2e/` (pytest), against an **already-running**
cluster. The tests never start, stop or build anything with docker; the compose
lifecycle belongs to the CI `e2e` job and to the
[`cluster-smoke-test`](../skills/cluster-smoke-test/SKILL.md) skill.

```bash
docker build -t raftkv:latest .
docker compose up -d
pip install -r tests/e2e/requirements.txt
pytest tests/e2e -v
pytest tests/e2e -m requires_docker -v   # Phase 2 crash test, opt-in
docker compose down -v       # rm -rf vol-node* for a clean slate
```

Use `docker compose`, not the standalone `docker-compose` binary.

`tests/e2e/test_crash.py` is the single, deliberate exception to the
no-docker rule: proving that an acknowledged write survives `kill -9` means
sending a real SIGKILL to a real container. It carries the `requires_docker`
marker, `pytest.ini` deselects that marker by default so the line above stays
true for the normal run, and the test *skips* rather than fails when docker or
the local compose project is unavailable. Any future test that must drive docker
goes in that module, behind that marker — not into `conftest.py`.

The e2e suite makes the check the old script never made: a write on the leader
must be readable on **all three** nodes (polled against a deadline, no fixed
sleep). It also fails loudly — exit 1, "cluster does not look ready" — when no
cluster is running, instead of printing a confusing transport error.

`test_client.py` survives only as a manual one-shot demo. It asserts nothing and
touches a single node; never treat a clean run of it as verification.

## Chaos and benchmarks (Phase 7)

Two things live outside the three layers above, and both have a rule attached.

**`tests/chaos/`** drives docker directly — the deliberate opposite of the e2e
rule, because injecting a fault means killing a real container. It runs nightly,
not per-PR.

- **A check that cannot fail is worse than no check.** The harness proves its own
  checker can fail before every run: it asserts a value that was never written, a
  value that differs from what the cluster holds, a value that matches, and a
  deletion that did not happen — and requires three objections and one silence. Do
  not remove that, and do not add an invariant without a negative control for it.
  Two assertions in this repository were vacuous for two phases while CI stayed
  green; that is the precedent, not a hypothetical.
- **Only acknowledged operations may be asserted.** A write that timed out or
  returned 502 may have committed anyway. Those keys are poisoned, reclaimed only
  after every retry the write path can perform has provably elapsed, and never
  claimed in between. Relaxing that produces false violations on correct behavior.
- Mid-run checks **park the load first**. Replaying a live journal against a live
  cluster reports lost writes that never happened.

**`bench/`** is a separate Go module and is *not* covered by the `go` CI job; it
has its own `bench-build` job. It has no tests and should not grow fake ones — a
test asserting that a throughput number is positive is worse than nothing. Its
output is checked by reading it, and [docs/benchmarks.md](../../docs/benchmarks.md)
states the run-to-run variance so a single number is not mistaken for a
measurement.

## When adding tests

The seams the code was structured around are now actually in use — copy the
existing tests rather than inventing a new style:

- **Go**: table-driven tests with `go test -race ./...`. `fsm.CppFSM` takes the
  `StateMachineClient` interface — `internal/fsm/fsm_test.go` drives it with a
  fake, no gRPC. `internal/cluster/joiner_test.go` runs `Joiner` against an
  `httptest.Server` with the retry counts shrunk via `JoinConfig`.
  `internal/management/server_test.go` injects a `fakeRaftControl` into the
  handlers; note that its `var _ RaftControl = (*raftnode.Node)(nil)`
  compile-time check lives in the **test** file, because putting it in
  `raftnode` would make `raftnode` import `management` and invert the very
  dependency `RaftControl` exists to break.
- **C++**: GoogleTest via CMake/CTest, wired as the separate `kvdb_tests` target
  behind `KVDB_BUILD_TESTS` so `kvdb_node` stays dependency-free. Add new
  `cpp-app/tests/*.cpp` files there. `KVHttpHandler` (with fake
  `IRaftClient`/`IKVStore`) is still an untested high-value target.
- **Cluster behavior** (leader election, failover, restart replay) belongs in
  `tests/e2e/` or scripted docker compose scenarios, not unit tests.

**Pinned-buggy tests.** Phase 0 is a safety net, not a fix: several assertions
deliberately encode current misbehavior, each commented with the phase that will
change it. When you fix one of those behaviors, update its pinning test in the
**same** PR — do not "fix" the test on its own. Two sets have already been
flipped this way and must not be re-pinned: the 200-for-everything HTTP contract
and stringly `error` bodies (Phase 1), and the lossy `kv.db` line format
(Phase 2 — `=`, newlines and NUL bytes now round-trip exactly).

## What matters most to cover

1. ~~MsgPack decode edge cases in `KVCommand`~~ — **covered**
   (`cpp-app/tests/kv_command_test.cpp`): valid SET/DELETE, empty/truncated/
   garbage bytes, non-map and array wire formats, wrong field types, unknown op,
   missing/extra fields, and the `is_valid()` truth table.
2. ~~Store persistence round-trip, including keys/values containing `=` and
   newlines~~ — **covered** (`cpp-app/tests/kv_store_test.cpp`), and the pinned
   behavior is worse than "lossy values": a key containing `=` reloads as a
   *different key*, and a key containing a newline makes reload *invent* a key
   that was never written while losing the one that was. NUL bytes survive.
   Phase 2's replacement format has to fix these and flip those tests.
3. **Still open — propose failure paths**: sidecar down, non-leader, timeout
   (the C++ client has a 5s deadline). The non-leader case is pinned end-to-end
   by R0.4, but `GrpcRaftClient` itself is untested, and so are
   `internal/backend`, `internal/raftnode` and `internal/rpc` on the Go side.
4. **Still open — `KVHttpHandler`** routing and response bodies with fake
   `IRaftClient`/`IKVStore`. `HttpRequestParser` is covered; the handler above it
   is not.
