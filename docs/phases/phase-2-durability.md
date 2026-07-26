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
