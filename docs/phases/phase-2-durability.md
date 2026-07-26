# Phase 2 — Durability

## Spec

### Goals
An acknowledged write survives `kill -9` at any instant. Replace the
truncate-rewrite-everything persistence with an atomic base file + append-only WAL,
and replace the fragile line-based format with a binary one.

### Non-goals
- No raft snapshot integration (Phase 3 builds on the export format defined here).
- No group-commit/batching tuning beyond a simple fsync-per-append default.
- Raft log durability is already BoltDB's problem — untouched.

### Requirements

**Base file format (replaces line-based `kv.db`):**
- R2.1 Binary format: magic `KVB1` (4 bytes), `uint32` entry count, then per entry
  `uint32 key_len | key | uint32 value_len | value` (little-endian). No escaping needed —
  kills the `=`/newline corruption pinned by the Phase 0 store tests.
- R2.2 Atomic persistence: write to `kv.db.tmp`, `fsync` the file, `rename` over
  `kv.db`, `fsync` the directory. Implemented with POSIX fds (`open`/`write`/`fsync`),
  not `std::ofstream` (which cannot fsync).
- R2.3 Migration: on load, a file not starting with the magic is parsed with the legacy
  line format once, then rewritten in the new format. Legacy support can be dropped a
  release later.

**Write-ahead log:**
- R2.4 New header `cpp-app/src/storage/wal.hpp` (`namespace kvdb`, registered in
  `KVDB_HEADERS`): append-only file `kv.wal` of records
  `uint32 len | payload | uint32 crc32(payload)` where payload is the msgpack-encoded
  `KVCommand`. Each append is `write` + `fsync` before returning.
- R2.5 Recovery: startup loads `kv.db`, then replays `kv.wal` in order. A record with
  bad length/CRC (torn tail) truncates the WAL at that point and stops — everything
  before it is applied, nothing after.
- R2.6 Compaction: when the WAL exceeds a threshold (default 4 MB or 10k records,
  constants in `Config`), write a fresh base file per R2.2, then truncate the WAL.
  Done synchronously inside the store lock (simple; apply throughput is raft-bounded).
- R2.7 `PersistentKVStore::set/remove` append one WAL record instead of rewriting the
  base file. The `IKVStore` interface is unchanged; durability is an implementation
  detail behind it. All store methods still take the single mutex (CLAUDE.md rule).

**Configuration:**
- R2.8 WAL fsync mode configurable (`always` default, `never` for tests/benchmarks)
  via `Config` (`cpp-app/src/config/config.hpp`), not a magic number.

### Acceptance criteria
- C++ unit tests: base-file round-trip with binary-hostile keys/values (`=`, `\n`,
  NUL bytes); legacy-file migration; WAL replay; torn-tail recovery (truncate a valid
  WAL mid-record, reload, assert prefix applied and file healed).
- E2E crash test: write K keys, `docker kill` (SIGKILL) one node, restart it, assert
  all K keys present locally (raft replay is not required for these — they were applied
  and WAL'd before the kill).
- Existing e2e + smoke test green; no interface change visible to Go or HTTP layers.

## Plan

1. **Format primitives**: `cpp-app/src/storage/format.hpp` — encode/decode helpers for
   the length-prefixed records and CRC32 (tiny table-based implementation, no new deps).
   Unit tests first (TDD per repo rules).
2. **Atomic base file**: rewrite `PersistentKVStore::persist()`/`load()` in
   `cpp-app/src/storage/kv_store.hpp` per R2.1–R2.3; update `kv_store_test.cpp` —
   the Phase 0 pins for `=`/newline lossiness flip to exact round-trip assertions.
3. **WAL**: implement `wal.hpp` (append, replay-with-callback, truncate, heal) with
   its own unit tests including the torn-tail case (R2.4–R2.5).
4. **Store integration**: `PersistentKVStore` gains the WAL: constructor takes
   `Config` durability knobs; `set`/`remove` append; compaction per R2.6; startup
   recovery per R2.5.
5. **Crash e2e**: `tests/e2e/test_crash.py` — the docker-kill scenario; mark it to run
   in the CI e2e job.
6. **Docs**: remove the "Durability" known-limitation from CLAUDE.md and README;
   document `kv.db`/`kv.wal` in the README data-directory section.

### Verification
- `ctest` (new storage tests, including torn-tail).
- ASan/UBSan CI job over the new fd-handling code.
- `/cluster-smoke-test` plus `tests/e2e/test_crash.py` against a live cluster.
- Manual sanity: `xxd vol-node1/kv.db | head` shows the magic + binary layout.

## Outcome

**Status: ✅ Complete.** All eight requirements landed.

### Verified results

| Layer | Result |
|---|---|
| C++ | **191 tests**, 100% pass via CTest — up from 97 in Phase 1 |
| C++ sanitizers | 191/191 under `-fsanitize=address,undefined` (FetchContent GoogleTest, the path CI takes). This is the phase that adds raw fd handling and parses untrusted bytes off disk, so this run matters more here than anywhere else so far. |
| clang-format | gate clean across `cpp-app/src` and `cpp-app/tests` |
| Go | untouched by this phase; re-run anyway, still green |
| e2e | 13 passed, 1 deselected by default; the deselected crash test passes on its own with `-m requires_docker` |
| Crash test | **passes, and provably did the work** — node2 came back `Up 17 seconds` against ~1 minute for its peers, and the cluster logged 4 node boots rather than 3 |
| On-disk WAL | `xxd vol-node1/kv.wal` shows `3c 00 00 00` (length 60, little-endian) followed by the msgpack map — the R2.4 layout exactly |

Note on the manual `xxd kv.db` check in the plan: after a short run there is **no
`kv.db` at all**, only `kv.wal`. That is correct and worth understanding — the base
file is written by compaction, so until the WAL crosses `wal_max_bytes` /
`wal_max_records` the WAL *is* the durable state and recovery is
"empty base + replay everything".

### What landed

| Requirement | Where |
|---|---|
| R2.1 binary base file | `src/storage/format.hpp` (primitives), `PersistentKVStore::serialize_unlocked` / `parse_binary_unlocked` |
| R2.2 atomic persistence | `src/storage/atomic_file.hpp` — `atomic_write_file()`: temp file, `fsync`, `rename`, `fsync` the parent directory, POSIX fds throughout |
| R2.3 legacy migration | `PersistentKVStore::load_base_unlocked` / `parse_legacy_unlocked`, rewritten as `KVB1` on the same start |
| R2.4 WAL | `src/storage/wal.hpp` — `uint32 len \| payload \| uint32 crc32`, `fsync` per append |
| R2.5 recovery + torn-tail heal | `Wal::replay` (framing/CRC), `PersistentKVStore::replay_wal_unlocked` (undecodable payloads) |
| R2.6 compaction | `PersistentKVStore::maybe_compact_unlocked` / `compact_unlocked`, base file before WAL truncate |
| R2.7 store wiring | `PersistentKVStore(std::string, DurabilityOptions)`; `IKVStore` unchanged |
| R2.8 configuration | `DurabilityOptions` in `src/config/config.hpp`; `WalSyncMode` in `wal.hpp`; no new CLI args |
| Crash e2e | `tests/e2e/test_crash.py`, `requires_docker` marker registered in `pytest.ini` |

### Decisions taken during implementation

- **A file claiming to be `KVB1` is never fed to the legacy parser.** If it has
  the magic and then fails to parse, that is a hard `std::runtime_error` naming
  the file and the reason, and the file is left untouched on disk. Falling back
  would turn detectable corruption into plausible-looking garbage, and it
  resolves the converse ambiguity (a legacy file whose first line happens to
  start with `KVB1`) the same safe way.
- **Replay stops at the first record it cannot decode**, not just at the first
  physically torn one, and the refused tail is then dropped by a compaction so
  it cannot recur on every restart. Skipping a bad record and continuing was
  rejected: an undecodable record may have been a `DELETE`, so applying what
  follows would rebuild a state this replica never had. Prefix consistency is
  the same guarantee the torn-tail heal gives.
- **The WAL payload is a re-encoded `KVCommand`**, not the client's original
  msgpack bytes: `set()`/`remove()` receive a key and a value, not the wire
  buffer. It decodes to the same command, but it is not byte-identical to what
  `Propose` carried. Consequence: `KVCommand`'s field layout is now an on-disk
  format as well as a wire format.
- **`WalSyncMode` is defined in `storage/wal.hpp`, not in `config.hpp`**, so
  there is exactly one definition; `config/` therefore depends on `storage/` and
  not the other way round, which keeps the WAL usable without application
  config.
- **`Config::defaults()` was rewritten** from a C++20 designated-initializer
  list to default member initializers. Same values; the list warned under
  `-Wpedantic` in C++17, and Phase 2 pulls `config.hpp` into `kv_store.hpp` and
  therefore into every test translation unit, which would have multiplied one
  warning into many.
- **Durability knobs are `Config` fields with named defaults, not positional CLI
  arguments** (R2.8 says "not a magic number", not "make it argv"). The
  `kvdb_node <http_port> <grpc_port> <sidecar_port> <db_file>` contract is
  unchanged.
- **The WAL file is opened lazily**, on the first append, so a node that only
  reads never leaves an empty `kv.wal` behind. The append fd is then held for
  the object's lifetime — reopening per record is not affordable on the write
  path.
- **The torn-tail truncation is not fsynced.** A crash before it lands leaves
  the same torn tail, which the next start heals identically; the heal is
  idempotent.
- **`read_file()` returns `nullopt` only for `ENOENT`.** `EACCES`, `EISDIR` and
  friends throw, so an I/O fault cannot masquerade as a fresh empty store.
- **`atomic_write_file()` documents its post-rename failure semantics**: a throw
  at or before the `rename` means the target is untouched, but a throw from the
  *final directory fsync* means the new contents are installed and only their
  durability is unconfirmed. Callers must not read that throw as "the write did
  not happen". Harmless for compaction: the base file is written and the WAL is
  not truncated, so the next start replays records that are already folded in,
  idempotently.
- **The crash e2e test is quarantined behind a `requires_docker` marker.** The
  suite's standing rule is that tests never start, stop or build anything with
  docker — that is what lets `pytest tests/e2e` run against any cluster, local
  or remote. This test cannot honor it, so `pytest.ini` deselects the marker by
  default and the test is opted into explicitly
  (`pytest tests/e2e -m requires_docker`) rather than the rule being abandoned
  silently. It skips, never fails, when docker or the local compose project is
  unavailable.
- **The crash test asserts on the killed node's disk, while it is dead.** The
  obvious version — write, kill, restart, read back — proves nothing: there are
  no raft snapshots yet, so a restart replays the whole local BoltDB log through
  the state machine and would refill a store that had lost everything. The
  load-bearing assertion is therefore that the keys are already in `kv.db` /
  `kv.wal` before the container is started again. The post-restart read-back is
  deliberately not polled (recovery completes before the HTTP listener opens),
  and it is followed by a fresh write to prove the node rejoined rather than
  merely came back.

### Known issues this phase deliberately did not fix

- **An I/O failure in the store is reported as a deterministic rejection.**
  `set()`/`remove()` can now throw (WAL append, `fsync`, or a compaction rewrite
  failing); `StateMachineService::Apply` catches it and answers
  `success=false`, which `CppFSM.Apply` logs as a verdict that "all replicas
  reject identically and stay consistent" (go-sidecar/internal/fsm/fsm.go). That
  reasoning is correct for a malformed command and **wrong** for a local disk
  error: the node that failed to write really can diverge, while the log line
  says everything is fine. Before Phase 2 this branch was unreachable (the old
  `std::ofstream` swallowed write errors), so this is a new gap, not a
  regression. The fix is a distinct I/O-failure path in `state_machine.hpp` plus
  a divergence branch in the FSM — an error-taxonomy change touching the proto
  contract, which is its own piece of work.
- **The WAL grows on every restart.** Raft replays its entire log through
  `Apply`, and each of those applies writes a fresh WAL record, until compaction
  folds them away. Phase 3 (snapshots) is what bounds this.
- **`fsync` is not `F_FULLFSYNC`.** On Linux, where the containers run, `fsync`
  is the right call. On macOS it does not flush the drive's own write cache, so
  a local (non-container) build's durability claim is weaker than the Linux
  one. Not worth a platform branch for a store that ships in a Linux image.
- **The legacy line-format reader is still there.** R2.3 allows dropping it a
  release later: `parse_legacy_unlocked`, the non-magic branch of
  `load_base_unlocked`, and their tests are the whole of it.
- **A `kv.db.tmp` can survive a crash.** That is the mechanism working as
  intended — nothing reads it and the next compaction replaces it — but it does
  mean an operator will occasionally see one in the data directory.
- **Two Phase 0 byte-level assertions were dropped rather than flipped**
  (`read_file()==""` after a remove, `read_file()=="k=v2\n"` after an
  overwrite). They pinned truncate-and-rewrite-on-every-write, which is exactly
  what R2.7 removes; their intent is now covered by
  `EmptyStoreSerialisesToMagicAndAZeroCount` and
  `OverwriteKeepsOnlyTheLatestValue`.
