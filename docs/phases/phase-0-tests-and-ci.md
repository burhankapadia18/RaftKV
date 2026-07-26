# Phase 0 — Safety Net: Tests and CI

## Spec

### Goals
Create the verification infrastructure every later phase depends on: an asserting
e2e suite, unit tests on both sides of the sidecar boundary, and CI that gates it all.

### Non-goals
- No behavior changes to the product. Where current behavior is wrong (stringly
  errors, FSM ignoring apply failures), tests **pin** the current behavior and are
  updated in Phase 1 — they don't fix it.
- No coverage of snapshots/failover yet (nothing to test).

### Requirements

**E2E suite** (`tests/e2e/`, pytest + requests + msgpack, replaces `test_client.py` as the smoke test):
- R0.1 SET via leader (`:8080`) is readable on **all three** nodes (`:8080/:8081/:8082`),
  polling with a 5s deadline instead of a fixed sleep.
- R0.2 DELETE round-trip: SET, DELETE, then GET returns `Key Not Found` on all nodes.
- R0.3 GET of a never-written key returns `Key Not Found` (pins current 200-body contract).
- R0.4 SET sent to a follower returns body `error` (pins current no-forwarding contract;
  rewritten in Phase 4).
- R0.5 Suite exits non-zero on any failure; runnable as `pytest tests/e2e` against an
  already-running cluster (compose lifecycle handled by the CI job / smoke-test skill,
  not by the tests).

**Go unit tests** (`go test -race ./...`, target ≥80% per touched package):
- R0.6 `internal/fsm`: table-driven tests with a fake `StateMachineClient` — gRPC success,
  gRPC error, and `ApplyResponse{Success:false}` (pin: currently ignored; Phase 1 flips this).
- R0.7 `internal/config`: flag parsing, defaults, `BindAddr`/`AdvertiseAddr`.
- R0.8 `internal/cluster`: `Joiner` against `httptest.Server` — first-try success,
  success after N failures, exhaustion after 20 attempts (shrink retry/delay via `JoinConfig`).
- R0.9 `internal/management`: handler tests for `/join`, `/status`, `/health`. Requires
  extracting a small consumer-side interface for the node (`IsLeader`, `LeaderAddr`,
  `AddVoter`) so a fake can be injected — the only production refactor in this phase.

**C++ unit tests** (GoogleTest via CTest, separate target so `kvdb_node` stays dependency-free):
- R0.10 `KVCommand::from_msgpack`: valid SET/DELETE, malformed bytes (throws), unknown op,
  missing fields, empty key; `is_valid()` truth table.
- R0.11 `PersistentKVStore` round-trip against a temp file, including keys/values containing
  `=` and `\n` — pin the current line format's actual behavior before Phase 2 replaces it.
  **Correction (as built):** the damage is worse than "values are lossy", and the tests pin
  the real thing. `load()` splits at the *first* `=`, so a `=` inside a **value** is safe,
  but a **key** containing `=` reloads as a *different key* (`set("a=b","v")` writes `a=b=v`
  and reloads as key `a` with value `b=v` — the written key is gone). Newlines are worse
  still: a value containing `\n` is silently truncated at the newline, and a **key**
  containing `\n` makes reload *invent* a key that was never written (`set("a\nb","v")`
  writes two lines; `a` is dropped for having no `=`, and `b=v` becomes a brand-new key `b`)
  while losing the one that was written. NUL bytes in values survive intact. Phase 2 must fix
  the first four and not regress the last.
- R0.12 `HttpRequestParser`: routing fields, query extraction, msgpack content-type detection,
  missing/garbage `Content-Length` (document the current throw), body shorter than declared.

**CI** (`.github/workflows/ci.yml`):
- R0.13 Job `go`: `gofmt -l` (fail if non-empty), `go vet ./...`, `go test -race ./...`.
- R0.14 Job `cpp`: cmake build with the existing warning flags, `ctest --output-on-failure`;
  a second configuration with `-fsanitize=address,undefined`.
- R0.15 Job `e2e`: `docker build`, `docker compose up -d`, poll for readiness, `pytest tests/e2e`,
  `docker compose down -v`. (Use the `docker compose` CLI plugin everywhere — the standalone
  `docker-compose` binary is not guaranteed to be installed locally or on the runner.)
- R0.16 Formatting configs checked in: `.clang-format` (matching current style), gofmt enforced.

### Acceptance criteria
- CI green on `main`; every job actually runs tests (no empty matrices).
- E2E proves replication on all three nodes — the check `test_client.py` never made.
- `go test -race ./...` and `ctest` pass locally per the commands in CLAUDE.md.

## Plan

1. **E2E**: create `tests/e2e/conftest.py` (base URLs, msgpack helpers, `wait_until` poller)
   and `tests/e2e/test_cluster.py` (R0.1–R0.4). Keep `test_client.py` as a thin manual
   demo or delete it and update README + the `cluster-smoke-test` skill to call pytest.
2. **Go — management refactor first** (only code change): in
   `go-sidecar/internal/management/server.go`, define `type RaftControl interface
   { IsLeader() bool; LeaderAddr() string; AddVoter(id, addr string) error }`,
   accept it in the constructor (`*raftnode.Node` already satisfies it), and add
   `var _ RaftControl = (*raftnode.Node)(nil)`.
   **Correction (as built):** that assertion must **not** live in `raftnode`, as
   originally written here. Putting it there would make package `raftnode` import
   package `management` — inverting the exact dependency `RaftControl` exists to
   break (`management` depends on an abstraction, `raftnode` knows nothing about
   its consumers). It lives in `go-sidecar/internal/management/server_test.go`
   instead: same compile-time guarantee that the real node still satisfies the
   interface, but the import stays pointing the right way, and it is a test-only
   dependency so production `management` code does not pull in `raftnode` either.
3. **Go tests**: `internal/fsm/fsm_test.go`, `internal/config/config_test.go`,
   `internal/cluster/joiner_test.go`, `internal/management/server_test.go` (R0.6–R0.9).
4. **C++ test scaffolding**: in `cpp-app/CMakeLists.txt` add option `KVDB_BUILD_TESTS`
   (default OFF), FetchContent GoogleTest, `enable_testing()`, target `kvdb_tests` from
   `cpp-app/tests/*.cpp`.

   **Correction (as built):** two deviations from this step.
   (a) GoogleTest is *preferred from the system* — `find_package(GTest QUIET)` first,
   FetchContent (`v1.15.2`, `GIT_SHALLOW`) only as a fallback — so the `cpp` CI job runs
   offline against Debian's `libgtest-dev`. The `cpp-sanitizers` job deliberately omits
   that package so the fallback builds GoogleTest from source *under the same sanitizer
   flags*; linking a non-instrumented `libgtest.a` into ASan-instrumented objects is a
   known `container-overflow` false-positive source.
   (b) Sources are **listed explicitly**, not globbed: adding a fourth
   `cpp-app/tests/*.cpp` file requires adding it to the `add_executable(kvdb_tests ...)`
   list by hand. That is intentional — a new test file should be a visible diff.
5. **C++ tests**: `cpp-app/tests/kv_command_test.cpp`, `kv_store_test.cpp`,
   `http_request_test.cpp` (R0.10–R0.12).
6. **CI**: `.github/workflows/ci.yml` with the three jobs (R0.13–R0.15); add `.clang-format`.
7. **Docs**: update CLAUDE.md ("no unit test suite" section) and `.claude/rules/testing.md`
   to describe the new commands.

### Verification
- Locally: `cd go-sidecar && go test -race ./...`; `cmake -DKVDB_BUILD_TESTS=ON .. && make && ctest`;
  full cluster up + `pytest tests/e2e`.
- Push a branch, confirm all CI jobs run and pass; break one test deliberately to
  confirm the gate actually fails.

## Outcome

Phase 0 is **complete**. Verified results:

**Go** — `go vet ./...` clean; `go test -race ./...` passes and was stable across three
consecutive runs. Coverage per package:

| Package | Coverage |
|---|---|
| `internal/fsm` | 100% |
| `internal/management` | 97.1% |
| `internal/cluster` | 96.2% |
| `internal/config` | 87.5% |

`internal/backend`, `internal/raftnode`, `internal/rpc`, `cmd/sidecar` and `pb` have no
tests yet — deliberately out of Phase 0 scope, and the obvious next targets.

**C++** — `cmake -S cpp-app -B build -DKVDB_BUILD_TESTS=ON && cmake --build build && ctest`
gives **56 tests, 100% pass**. The same 56 pass under `-fsanitize=address,undefined`. With
`KVDB_BUILD_TESTS` unset or OFF the `kvdb_node` build is unaffected (verified), so neither
the default build nor the Docker image gained a GoogleTest dependency. The tests use the
system GoogleTest (Debian `libgtest-dev`) when it is present and fall back to `FetchContent`
otherwise.

**E2E** — 4 tests (R0.1–R0.4) pass against a live 3-node `docker compose` cluster. The
suite was checked for the two ways a test suite lies: it genuinely fails with a non-zero
exit when a node is stopped, and it exits 1 with a clear "cluster does not look ready"
message (naming what each node answered) when no cluster is running, rather than erroring
out obscurely.

**CI** — `.github/workflows/ci.yml` has four independent jobs (no `needs:`, so a C++
failure still reports the Go result): `go`, `cpp`, `cpp-sanitizers`, `e2e`. The `cpp` job
pins `debian:bookworm-slim`, which pins clang-format to 14; `.clang-format` parses under
that version and every file under `cpp-app/src` is already conformant, which is what makes
the clang-format check **blocking** rather than advisory.

**Known warnings not addressed.** Phase 0 neither introduced nor fixed these pre-existing
compiler warnings; they are left for a later phase to avoid mixing a behavior change into
a test-only phase:
- 4× "designated initializers only available with C++20" in `cpp-app/src/config/config.hpp`
- 1× "extra ';'" in `cpp-app/src/commands/kv_command.hpp`

**Corrections to this document** are recorded inline above: the `RaftControl` compile-time
assertion moved from `raftnode` to `management`'s test file (plan step 2), and R0.11's
description of the store's line-format damage was too mild.
