# RaftKV C++ Rules

Project-specific rules for `cpp-app/`. These extend the general C++ guidance with what is idiomatic *in this repo*.

## Structure

- **Header-only modules, single translation unit.** All logic lives in `.hpp` files under `cpp-app/src/<domain>/`; `src/main.cpp` is the only `.cpp` and does bootstrap only. Add new code as a header in the right domain directory (`config/`, `commands/`, `storage/`, `raft/`, `network/`) and register it in `KVDB_HEADERS` in `CMakeLists.txt`.
- Everything lives in `namespace kvdb`.
- New subsystems should be injectable behind a small abstract interface, matching `IKVStore` and `IRaftClient` (pure virtual, virtual default destructor, `I` prefix). Concrete classes take dependencies as references in the constructor.

## Style (as practiced in this codebase)

- C++17, compiled with `-Wall -Wextra -Wpedantic -Wno-unused-parameter`. New code must not add warnings.
- Types `PascalCase`, functions/methods `snake_case`, members `snake_case_` (trailing underscore), constants `kPascalCase` (e.g. `kDefaultTimeout`, `kBufferSize`).
- `[[nodiscard]]` on const accessors and factories; `std::optional` for lookups that can miss; exceptions only for fatal setup failures (socket bind, config parse).
- Doxygen-style `/** */` comments on public classes and methods — keep that density when adding code.
- RAII and rule-of-zero; delete copy operations on resource-owning classes (see `HttpServer`). No raw `new`/`delete` — `std::make_unique` where ownership is needed.

## Concurrency

- `PersistentKVStore` guards all access with a single `std::mutex`; any new store method must take the lock.
- **The `_unlocked` suffix is a contract, not a naming style.** Every *public* store method takes `mutex_` for its whole body; every `*_unlocked` helper assumes the caller already holds it and must **never** re-acquire it. `std::mutex` is not recursive, so re-locking is an instant deadlock — and it is reachable, because `set()`/`remove()` call `maybe_compact_unlocked()` while holding the lock. A new helper is either public-and-locking or `_unlocked`-and-not; never both, never neither. (The constructor calls `_unlocked` helpers without the lock: nothing can observe the object yet.)
- The gRPC `StateMachineServer` runs in a detached background thread while the HTTP server blocks the main thread (see `main.cpp`). Both mutate the store concurrently — anything shared between the HTTP handler and `StateMachine::Apply` must be thread-safe.

## Durability (Phase 2 — do not regress this)

`cpp-app/src/storage/` is `kv_store.hpp` (map + recovery) over `wal.hpp` (append/replay/heal), `atomic_file.hpp` (fd-level write + `fsync` + `rename`) and `format.hpp` (length-prefix + CRC32 primitives, no I/O). On-disk layouts are documented in the [README](../../README.md#data-directory).

- **Write-ahead order is the guarantee.** `set()`/`remove()` append the command to the WAL and `fsync` it **before** the in-memory map changes. Never reorder that, never make the fsync conditional on anything but `DurabilityOptions::sync_mode`, and never make the map the source of truth for a write that has not been logged.
- **Never write a file with `std::ofstream`** — it cannot `fsync`, which is why R2.2 exists. Whole-file writes go through `atomic_write_file()` (temp file, `fsync`, `rename`, `fsync` the parent directory); appends go through `Wal`. Both use POSIX fds owned by `fileio::FdGuard` — no bare `close()`, no fd leaked on a throw.
- **Every byte read off disk is untrusted input.** A length prefix must be bounds-checked against the bytes that actually remain before it is used to index, advance or `reserve` — see `format::read_blob` and `PersistentKVStore::parse_binary_unlocked`. Anything else is a heap overflow driven by a corrupt file, and these paths run under ASan/UBSan in CI.
- **Recovery is prefix-consistent.** A torn WAL tail (short record, length overrun, CRC mismatch) truncates the file there and stops; a record that frames correctly but does not decode to a valid `KVCommand` stops replay too. Do not "skip the bad one and continue": a record you cannot read may have been a DELETE, so continuing past it reconstructs a state the replica never had.
- **Compaction writes the new base file *before* truncating the WAL.** A crash between the two costs a re-replay of records already in the base file, and SET/DELETE replay is idempotent. The reverse order loses data.
- **Thresholds are config, not constants in the store**: add tunables to `DurabilityOptions` in `config/config.hpp` (which is why `config/` depends on `storage/` — `WalSyncMode` is defined next to the WAL it configures).
- **`set()`/`remove()` can throw** on an I/O failure. Callers must not swallow it: `StateMachineService::Apply` reports it, and a silently-ignored write failure is a diverged replica.

Verification for anything in this area: the C++ unit tests (`format_test`, `atomic_file_test`, `wal_test`, `kv_store_test`) plus `pytest tests/e2e -m requires_docker`, the SIGKILL test — a unit test cannot prove that `fsync` was really called.

## MsgPack wire format

- The client payload is a MsgPack **map** `{op, key, value}` — `MSGPACK_DEFINE_MAP` in `KVCommand`, not the array variant. Changing fields breaks `test_client.py` and every stored raft log entry; treat the format as versioned.
- The HTTP layer forwards MsgPack bytes without parsing them; validation happens only in `StateMachine::Apply` via `KVCommand::from_msgpack` (which throws on malformed input — keep the try/catch there).
- The same format is now also an **on-disk** format: each WAL record holds a msgpack `KVCommand` (a re-encoding produced by the store from the key and value, not a byte-for-byte copy of the client's request). A change to `KVCommand`'s fields therefore has to stay decodable for records already written to some node's `kv.wal`, not just for raft log entries.

## Build

```bash
cd cpp-app && mkdir -p build && cd build && cmake .. && make -j4
```

- macOS and Linux resolve protobuf differently (CONFIG vs MODULE mode) — `CMakeLists.txt` already handles this; don't simplify it away.
- Never edit `cpp-app/pb/` — it is a stale copy; real stubs are generated into `build/proto/` at build time.
