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
- The gRPC `StateMachineServer` runs in a detached background thread while the HTTP server blocks the main thread (see `main.cpp`). Both mutate the store concurrently — anything shared between the HTTP handler and `StateMachine::Apply` must be thread-safe.

## MsgPack wire format

- The client payload is a MsgPack **map** `{op, key, value}` — `MSGPACK_DEFINE_MAP` in `KVCommand`, not the array variant. Changing fields breaks `test_client.py` and every stored raft log entry; treat the format as versioned.
- The HTTP layer forwards MsgPack bytes without parsing them; validation happens only in `StateMachine::Apply` via `KVCommand::from_msgpack` (which throws on malformed input — keep the try/catch there).

## Build

```bash
cd cpp-app && mkdir -p build && cd build && cmake .. && make -j4
```

- macOS and Linux resolve protobuf differently (CONFIG vs MODULE mode) — `CMakeLists.txt` already handles this; don't simplify it away.
- Never edit `cpp-app/pb/` — it is a stale copy; real stubs are generated into `build/proto/` at build time.
