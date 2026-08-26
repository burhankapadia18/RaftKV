# Management Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a browser console — cluster overview, key browser, single-key console, user/ACL management — served by the existing C++ engine on port 8080 from the same container, adding no process, port, or runtime dependency.

**Architecture:** Three new JSON routes on the existing hand-rolled HTTP server (`/kv` list, `/cluster/status`, `/auth/users`), backed by a new secondary ordered index over the store's own keys and one new unary gRPC RPC (`RaftNode.Status`). A Preact SPA is built by Vite in a Docker build stage, embedded into `kvdb_node` as `.rodata` by CMake, and served from `/console/` outside the authentication gate. Nothing runs server-side until a browser asks.

**Tech Stack:** C++17 (header-only, gRPC/protobuf/msgpack only), Go 1.24 (`hashicorp/raft`), protobuf 3, Preact + Vite + TypeScript (build stage only), GoogleTest/CTest, pytest.

**Spec:** [`docs/superpowers/specs/2026-08-26-management-console-design.md`](../specs/2026-08-26-management-console-design.md)

## Global Constraints

Every task's requirements implicitly include this section.

- **C++17**, compiled with `-Wall -Wextra -Wpedantic -Wno-unused-parameter`. New code must add **zero** warnings.
- **C++ is header-only by design.** All logic in `.hpp` under `cpp-app/src/<domain>/`; `src/main.cpp` is the only translation unit and does bootstrap only. Every new header must be added to `KVDB_HEADERS` in `cpp-app/CMakeLists.txt`.
- **C++ naming:** types `PascalCase`, functions `snake_case`, members `snake_case_`, constants `kPascalCase`. `[[nodiscard]]` on const accessors and factories. Doxygen `/** */` on public classes and methods.
- **No new C++ dependency.** The binary links only gRPC, protobuf and msgpack. No JSON library — every body is hand-built and every string goes through `json_escape()`.
- **`_unlocked` is a contract:** every public `PersistentKVStore` method takes `mutex_` for its whole body; every `*_unlocked` helper assumes the caller holds it and **must never re-acquire it** (`std::mutex` is not recursive).
- **C++ test sources are listed explicitly** in `cpp-app/CMakeLists.txt` — never globbed.
- **Go:** module is `my-raft-sidecar`. `gofmt -l .` must be empty, `go vet ./...` clean, `go test -race ./...` green. Wrap errors as `fmt.Errorf("context: %w", err)`. Add `var _ Iface = (*Impl)(nil)` compile-time checks.
- **Go generated stubs in `go-sidecar/pb/` are checked in** and regenerated manually; `cpp-app/pb/` is a stale copy — never edit it.
- **Proto:** never renumber or reuse a field tag. Additions only.
- **Node.js appears only in a Docker build stage** and in `console/` dev tooling. The runtime image gains nothing.
- **Use `docker compose`** (the CLI plugin), never the standalone `docker-compose` binary.
- **Commit format:** `<type>: <description>` (types: feat, fix, refactor, docs, test, chore, perf, ci). **No attribution trailer** — attribution is disabled globally.
- **Handler, README table and `tests/e2e/contracts.py` change in the same commit.** That is a standing repo rule.
- Run `clang-format` against `.clang-format` on every touched C++ file; CI blocks on it.

## Deltas from the spec

Two details the spec left open or loose, decided here:

1. **`limit` must be an integer in `1..500`.** The spec said "non-negative", which admits `limit=0` — a degenerate page that cannot advance a cursor. `0` is now a 400. The spec has been updated to match.
2. **`key_count` and `wal_size_bytes` reach the handler through a new `IStoreStats` seam**, not by widening `IKVStore`. `PersistentKVStore` already has both methods but deliberately kept them off `IKVStore` ("widening the storage interface for it would force every test fake to implement it too"). A separate seam honours that decision and keeps `IKVStore` about storage semantics. Defined in Task 5.

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `cpp-app/src/network/static_assets.hpp` | Embedded-asset table type, path lookup, gzip/ETag selection. Pure logic, no `HttpResponse`. |
| `cpp-app/tests/static_assets_test.cpp` | Drives `static_assets.hpp` with a hand-written table. |
| `cpp-app/cmake/GenerateConsoleAssets.cmake` | Reads a `dist/` tree, emits `console_assets_generated.hpp`. |
| `console/package.json`, `console/vite.config.ts`, `console/tsconfig.json` | UI build. |
| `console/scripts/gzip-dist.mjs` | Gzips text assets after `vite build`. |
| `console/src/main.tsx`, `console/src/App.tsx` | App entry and shell. |
| `console/src/lib/api.ts` | The only place `fetch` is called. |
| `console/src/lib/auth.ts` | Credential storage and the `Authorization` header. |
| `console/src/styles/tokens.css`, `console/src/styles/global.css` | Design tokens, base layout. |
| `console/src/components/*` | `NodeCard`, `KeyTable`, `KeyEditor`, `UserTable`, `LoginForm`, `AuthBanner`, `StatPair`. |
| `console/src/pages/ClusterPage.tsx`, `KeysPage.tsx`, `UsersPage.tsx` | One page each. |
| `tests/e2e/test_console.py` | Console + new-route end-to-end. |

**Modified:**

| File | Change |
|---|---|
| `cpp-app/src/storage/kv_store.hpp` | Ordered index, three funnel helpers, `KeyPage`/`scan_keys` on `IKVStore`, `IStoreStats`. |
| `cpp-app/src/raft/raft_client.hpp` | `RaftPeer`, `StatusResult`, `IRaftClient::status()`, gRPC impl. |
| `cpp-app/src/network/http_server.hpp` | Three JSON routes, static routes, percent-encode helper, `route_label` entries. |
| `cpp-app/CMakeLists.txt` | New headers, new test sources, `KVDB_CONSOLE` option, asset generation. |
| `cpp-app/tests/{kv_store,http_handler,state_machine}_test.cpp` | New coverage; fakes gain `scan_keys` + `IStoreStats`. |
| `proto/consensus.proto` | `Status` RPC, `StatusRequest`, `StatusResponse`, `Peer`. |
| `go-sidecar/pb/*` | Regenerated. |
| `go-sidecar/internal/raftnode/node.go` | `ID()`, `LeaderWithID()`, `Configuration()`. |
| `go-sidecar/internal/rpc/server.go` | `RaftStatusReporter`, `WithStatusReporter`, `Status`. |
| `go-sidecar/cmd/sidecar/main.go` | Wire `WithStatusReporter(node)`. |
| `Dockerfile` | `node:22-alpine` console stage. |
| `README.md`, `tests/e2e/contracts.py`, `CLAUDE.md`, `docs/architecture.md`, `CHANGELOG.md` | Documentation. |

---

### Task 1: Ordered key index behind three funnel helpers

Adds the index and routes all six existing `store_` mutation sites through helpers that maintain it. No new public API yet — this task is pure invariant work, and it must leave every existing test green.

**Files:**
- Modify: `cpp-app/src/storage/kv_store.hpp`
- Test: `cpp-app/tests/kv_store_test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `PersistentKVStore::index_` (`std::set<std::string_view>`, private), and private helpers `void put_unlocked(const std::string &key, const std::string &value)`, `bool erase_unlocked(const std::string &key)`, `void replace_all_unlocked(StateMap state)`. Task 2 reads `index_`.

- [ ] **Step 1: Write the failing test**

Append to `cpp-app/tests/kv_store_test.cpp`. `IndexTracksEveryMutation` reaches the index through a temporary accessor the next step adds; it exists so the invariant is observable before `scan_keys` does.

```cpp
// --- Ordered key index (console phase) -------------------------------------
//
// The index holds string_views into store_'s own key strings. These tests exist
// because a dangling view does not crash reliably -- it reads as a corrupted
// key -- so every path that can create or destroy a map node is exercised here.

TEST(PersistentKVStoreIndexTest, TracksInsertUpdateAndErase) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");

  store.set("b", "1");
  store.set("a", "1");
  store.set("c", "1");
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "b", "c"}));

  // An update must NOT double-register: insert_or_assign reuses the node.
  store.set("b", "2");
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "b", "c"}));

  EXPECT_TRUE(store.remove("b"));
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "c"}));

  // Removing a key that was never there must not touch the index.
  EXPECT_FALSE(store.remove("zz"));
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "c"}));
}

TEST(PersistentKVStoreIndexTest, ByteTransparentOrdering) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");

  // Keys with '=', newlines and NUL bytes round-trip and order exactly. The
  // NUL case is the one a naive exclusive cursor would get wrong.
  const std::string with_nul("a\0b", 3);
  store.set("a=b", "1");
  store.set("a\nb", "1");
  store.set(with_nul, "1");
  store.set("a", "1");

  // Byte order: "a" < "a\0b" < "a\nb" < "a=b"  (0x00 < 0x0a < 0x3d)
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", with_nul, "a\nb", "a=b"}));
}

TEST(PersistentKVStoreIndexTest, SurvivesRestoreState) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  store.set("gone", "1");

  StateMap replacement;
  replacement["x"] = "1";
  replacement["y"] = "1";
  store.restore_state(std::move(replacement));

  // A whole-map replace kills every old node, so every old view must be gone.
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"x", "y"}));
}

TEST(PersistentKVStoreIndexTest, RebuiltFromDiskOnReopen) {
  TempDir dir;
  const std::string path = dir.path() + "/kv.db";
  {
    PersistentKVStore store(path);
    store.set("k2", "1");
    store.set("k1", "1");
    store.set("k3", "1");
    EXPECT_TRUE(store.remove("k2"));
  }
  // Reopen: base-file load plus WAL replay must both feed the index.
  PersistentKVStore reopened(path);
  EXPECT_EQ(reopened.ordered_keys_for_test(),
            (std::vector<std::string>{"k1", "k3"}));
}

TEST(PersistentKVStoreIndexTest, SurvivesCompaction) {
  TempDir dir;
  DurabilityOptions options;
  options.wal_max_records = 4;  // force several compactions
  PersistentKVStore store(dir.path() + "/kv.db", options);

  for (int i = 0; i < 40; ++i) {
    store.set("k" + std::to_string(i % 7), std::to_string(i));
  }
  // Compaction rewrites the base file from store_ but never touches the map,
  // so no view may move. A no-op today; pinned so it stays one.
  EXPECT_EQ(store.ordered_keys_for_test().size(), 7u);
}

TEST(PersistentKVStoreIndexTest, RebuiltFromLegacyBaseFile) {
  TempDir dir;
  const std::string path = dir.path() + "/kv.db";
  {
    std::ofstream legacy(path, std::ios::binary);
    legacy << "b=2\n" << "a=1\n" << "c=3\n";
  }
  PersistentKVStore store(path);
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "b", "c"}));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
```

Expected: **compile failure** — `'ordered_keys_for_test' is not a member of 'kvdb::PersistentKVStore'`.

- [ ] **Step 3: Add the index, the helpers and the test accessor**

In `cpp-app/src/storage/kv_store.hpp`, add `#include <set>` and `#include <string_view>` to the include block. Add the member next to `store_` (around line 411):

```cpp
  StateMap store_;

  /**
   * @brief Lexicographically ordered view of every key in @c store_.
   *
   * Holds std::string_view into the KEY STRINGS OWNED BY store_'s own nodes,
   * so no key bytes are copied: an unordered_map node keeps its address across
   * a rehash, which is what makes the views stable. Cost is one RB-tree node
   * per key (48-64 bytes, independent of key length).
   *
   * INVARIANT, and it is a use-after-free if broken: every view in here points
   * at a live node of store_. Exactly three helpers may mutate store_ --
   * put_unlocked, erase_unlocked and replace_all_unlocked -- and every one of
   * them maintains this set. Nothing else in this file may touch store_
   * directly.
   *
   * Byte-transparent, like the store: std::less<std::string_view> compares via
   * char_traits::compare, so keys containing '=', newlines or NUL bytes order
   * exactly.
   */
  std::set<std::string_view> index_;
```

Add the three helpers in the private section, beside the other `_unlocked` helpers:

```cpp
  /**
   * @brief Insert or update one entry, keeping @c index_ in step.
   *
   * The view is taken from the MAP NODE'S key (@c result.first->first), never
   * from @p key. A view of the caller's argument dangles the moment that
   * string dies -- and would appear to work, because the bytes are usually
   * still there.
   *
   * Caller must hold @c mutex_.
   */
  void put_unlocked(const std::string &key, const std::string &value) {
    const auto result = store_.insert_or_assign(key, value);
    if (result.second) {
      // A new node was created. An update reuses the existing node, whose key
      // is already registered, so re-inserting would be wasted work.
      index_.insert(std::string_view(result.first->first));
    }
  }

  /**
   * @brief Erase one entry, keeping @c index_ in step.
   * @return true if the key existed.
   *
   * The view is removed BEFORE the node dies. The reverse order leaves a
   * dangling view in the set for as long as it takes to erase it, and the
   * erase itself compares against it.
   *
   * Caller must hold @c mutex_.
   */
  bool erase_unlocked(const std::string &key) {
    const auto it = store_.find(key);
    if (it == store_.end()) {
      return false;
    }
    index_.erase(std::string_view(it->first));
    store_.erase(it);
    return true;
  }

  /**
   * @brief Replace the whole map, rebuilding @c index_ from scratch.
   *
   * Every pre-existing view is dead and every new node is unregistered, so
   * this is a clear-and-rebuild rather than a diff. O(n log n), and it only
   * runs on a snapshot install or a start-up load.
   *
   * Caller must hold @c mutex_ (the constructor's calls are the documented
   * exception: nothing can observe the object yet).
   */
  void replace_all_unlocked(StateMap state) {
    index_.clear();
    store_ = std::move(state);
    for (const auto &entry : store_) {
      index_.insert(std::string_view(entry.first));
    }
  }
```

Add the test accessor in the public section, next to `key_count()`:

```cpp
  /**
   * @brief The index's contents, in order, as owned strings (tests only).
   *
   * Exists so the string_view invariant is observable. Deliberately NOT on
   * IKVStore and deliberately O(n): production code pages through scan_keys.
   */
  [[nodiscard]] std::vector<std::string> ordered_keys_for_test() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    keys.reserve(index_.size());
    for (const std::string_view key : index_) {
      keys.emplace_back(key);
    }
    return keys;
  }
```

- [ ] **Step 4: Route all six mutation sites through the helpers**

Replace each direct `store_` mutation. `set()` (~line 272):

```cpp
  void set(const std::string &key, const std::string &value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    wal_.append(encode_command(kOpSet, key, value));
    put_unlocked(key, value);
    maybe_compact_unlocked();
  }
```

`remove()` (~line 301):

```cpp
  bool remove(const std::string &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (store_.find(key) == store_.end()) {
      return false;
    }
    wal_.append(encode_command(kOpDelete, key, std::string()));
    erase_unlocked(key);
    maybe_compact_unlocked();
    return true;
  }
```

`restore_state()` (~line 394) — the last line only:

```cpp
    replace_all_unlocked(std::move(state));
```

`load_base_unlocked()` (~line 458) — the KVB1 branch:

```cpp
      try {
        replace_all_unlocked(deserialize_state(*contents));
      } catch (const std::runtime_error &error) {
```

`parse_legacy_unlocked()` (~line 494) — the loop body's last line:

```cpp
      put_unlocked(line.substr(0, eq_pos), line.substr(eq_pos + 1));
```

`apply_payload_unlocked()` (~lines 557/560):

```cpp
    case Operation::SET:
      put_unlocked(cmd.key, cmd.value);
      return true;
    case Operation::DELETE:
      erase_unlocked(cmd.key);
      return true;
```

`erase_unlocked` returns `bool` and is discarded here on purpose — replay does not care whether the key was present. It is not `[[nodiscard]]`, so this compiles clean.

- [ ] **Step 5: Register the new test file — none needed, and verify**

`kv_store_test.cpp` is already in `CMakeLists.txt`. Rebuild and run:

```bash
cmake --build cpp-app/build -j4
ctest --test-dir cpp-app/build --output-on-failure
```

Expected: **all** tests pass, including the pre-existing `kv_store_test` cases (the funnel must not have changed observable behaviour).

- [ ] **Step 6: Verify under the sanitizers**

A dangling `string_view` is exactly what ASan catches and a plain run does not.

```bash
cmake -S cpp-app -B cpp-app/build-asan -DKVDB_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build cpp-app/build-asan -j4
ctest --test-dir cpp-app/build-asan --output-on-failure
```

Expected: PASS with no ASan report.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i cpp-app/src/storage/kv_store.hpp cpp-app/tests/kv_store_test.cpp
git add cpp-app/src/storage/kv_store.hpp cpp-app/tests/kv_store_test.cpp
git commit -m "feat: ordered key index behind three store mutation funnels

Adds std::set<std::string_view> over the map's own keys and routes all six
store_ mutation sites through put_unlocked/erase_unlocked/
replace_all_unlocked, so the 'views point at the map node's key, never the
caller's argument' invariant lives in one place instead of being a
six-site audit. No public API change yet."
```

---

### Task 2: `scan_keys` on `IKVStore`

**Files:**
- Modify: `cpp-app/src/storage/kv_store.hpp`
- Modify: `cpp-app/tests/http_handler_test.cpp` (fake gains the method)
- Modify: `cpp-app/tests/state_machine_test.cpp` (fake gains the method)
- Test: `cpp-app/tests/kv_store_test.cpp`

**Interfaces:**
- Consumes: `PersistentKVStore::index_` from Task 1.
- Produces:
  - `struct IKVStore::KeyPage { std::vector<std::string> keys; bool reached_end = false; };`
  - `[[nodiscard]] virtual KeyPage IKVStore::scan_keys(std::string_view prefix, std::string_view start, size_t limit) const = 0;` — up to `limit` keys carrying `prefix`, from `start` **inclusive**, lexicographic. `reached_end` is true only when no further key carries the prefix.

  Task 3 and Task 6 call `scan_keys`.

- [ ] **Step 1: Write the failing test**

Append to `cpp-app/tests/kv_store_test.cpp`:

```cpp
// --- scan_keys -------------------------------------------------------------
//
// `start` is INCLUSIVE, which is what lets a caller resume past a whole RANGE
// of keys (there is no immediate predecessor of a string, so an exclusive
// cursor cannot express that). Two positions matter downstream:
//   K + '\0'  -- the first position strictly after K
//   "__sys;"  -- the first position past every "__sys:"-prefixed key

namespace {
std::vector<std::string> keys_of(const IKVStore::KeyPage &page) {
  return page.keys;
}
}  // namespace

TEST(PersistentKVStoreScanTest, EmptyStoreReachesEndImmediately) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");

  const IKVStore::KeyPage page = store.scan_keys("", "", 10);
  EXPECT_TRUE(page.keys.empty());
  EXPECT_TRUE(page.reached_end);
}

TEST(PersistentKVStoreScanTest, FiltersByPrefixAndStopsAtTheRangeEnd) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  for (const char *key : {"app:a", "app:b", "app", "apq", "zz"}) {
    store.set(key, "v");
  }

  // "app" is itself a key AND a prefix; it must be included.
  const IKVStore::KeyPage page = store.scan_keys("app", "", 10);
  EXPECT_EQ(keys_of(page), (std::vector<std::string>{"app", "app:a", "app:b"}));
  EXPECT_TRUE(page.reached_end);
}

TEST(PersistentKVStoreScanTest, LimitTruncatesAndDoesNotClaimTheEnd) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  for (const char *key : {"k1", "k2", "k3"}) {
    store.set(key, "v");
  }

  const IKVStore::KeyPage page = store.scan_keys("k", "", 2);
  EXPECT_EQ(keys_of(page), (std::vector<std::string>{"k1", "k2"}));
  EXPECT_FALSE(page.reached_end);
}

TEST(PersistentKVStoreScanTest, StartIsInclusive) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  for (const char *key : {"k1", "k2", "k3"}) {
    store.set(key, "v");
  }

  // Inclusive: "k2" is returned.
  EXPECT_EQ(keys_of(store.scan_keys("k", "k2", 10)),
            (std::vector<std::string>{"k2", "k3"}));

  // K + '\0' is the first position strictly after K.
  EXPECT_EQ(keys_of(store.scan_keys("k", std::string("k2\0", 3), 10)),
            (std::vector<std::string>{"k3"}));
}

TEST(PersistentKVStoreScanTest, StartWorksWhenTheKeyContainsNul) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  const std::string embedded("k\0z", 3);
  store.set(embedded, "v");
  store.set("k\1", "v");

  // Resuming after a key that itself contains a NUL must not skip "k\1".
  const std::string position = embedded + std::string(1, '\0');
  EXPECT_EQ(keys_of(store.scan_keys("k", position, 10)),
            (std::vector<std::string>{"k\1"}));
}

TEST(PersistentKVStoreScanTest, StartBeforeThePrefixSnapsForward) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  store.set("m1", "v");

  // A caller-supplied position that precedes the prefix range must not make
  // the scan stop instantly on a non-matching first key.
  const IKVStore::KeyPage page = store.scan_keys("m", "a", 10);
  EXPECT_EQ(keys_of(page), (std::vector<std::string>{"m1"}));
  EXPECT_TRUE(page.reached_end);
}

TEST(PersistentKVStoreScanTest, ReservedRangeIsSkippableAsARange) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  store.set("__sys:user:alice", "v");
  store.set("__sys:user:bob", "v");
  store.set("a", "v");
  store.set("zz", "v");

  // "__sys;" -- ';' is the byte after ':' -- lands past the whole reserved
  // range in ONE lower_bound, so no reserved key is ever examined.
  const IKVStore::KeyPage page = store.scan_keys("", "__sys;", 10);
  EXPECT_EQ(keys_of(page), (std::vector<std::string>{"a", "zz"}));
  EXPECT_TRUE(page.reached_end);
}

TEST(PersistentKVStoreScanTest, PaginatesAcrossAnEraseBetweenPages) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  for (const char *key : {"k1", "k2", "k3", "k4"}) {
    store.set(key, "v");
  }

  const IKVStore::KeyPage first = store.scan_keys("k", "", 2);
  EXPECT_EQ(keys_of(first), (std::vector<std::string>{"k1", "k2"}));

  // The key the cursor was derived from disappears between pages. A position
  // is not a key, so this must still resume correctly.
  EXPECT_TRUE(store.remove("k2"));
  const std::string position = first.keys.back() + std::string(1, '\0');
  EXPECT_EQ(keys_of(store.scan_keys("k", position, 2)),
            (std::vector<std::string>{"k3", "k4"}));
}

TEST(PersistentKVStoreScanTest, LimitZeroReturnsNothingAndClaimsNothing) {
  TempDir dir;
  PersistentKVStore store(dir.path() + "/kv.db");
  store.set("k", "v");

  // The handler never passes 0 (it rejects limit=0 with a 400), but the store
  // must not claim the range is exhausted if it is asked for nothing.
  const IKVStore::KeyPage page = store.scan_keys("", "", 0);
  EXPECT_TRUE(page.keys.empty());
  EXPECT_FALSE(page.reached_end);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build cpp-app/build -j4
```

Expected: **compile failure** — `'KeyPage' is not a member of 'kvdb::IKVStore'`.

- [ ] **Step 3: Declare `KeyPage` and `scan_keys` on `IKVStore`**

In `cpp-app/src/storage/kv_store.hpp`, inside `class IKVStore` (after `restore_state`):

```cpp
  /**
   * @brief One page of keys, plus whether the prefix range is exhausted.
   *
   * `reached_end` is not derivable from `keys.size() < limit`: a caller that
   * filters the page (for reserved keys, or for an ACL) may need several pages
   * to fill one response, and a short page does not mean the last page.
   */
  struct KeyPage {
    std::vector<std::string> keys;
    bool reached_end = false;
  };

  /**
   * @brief Up to @p limit keys carrying @p prefix, from @p start inclusive.
   *
   * Lexicographic (byte) order. An empty @p start means "the beginning of the
   * prefix range"; a @p start that precedes the range snaps forward to it.
   *
   * @p start is INCLUSIVE on purpose. An exclusive "after key K" cursor cannot
   * express "resume past this entire range of keys", because a string has no
   * immediate predecessor. An inclusive position expresses both: `K + '\0'` is
   * the first position strictly after `K` (any key greater than `K` either
   * extends it, and so is >= `K + '\0'`, or diverges earlier), and `"__sys;"`
   * is the first position past every `"__sys:"`-prefixed key.
   *
   * Keys are copied out; the caller may hold them freely. Deliberately takes no
   * filter callback: that would run caller logic while the store mutex is held.
   * A caller that needs filtering calls this repeatedly instead.
   */
  [[nodiscard]] virtual KeyPage scan_keys(std::string_view prefix,
                                          std::string_view start,
                                          size_t limit) const = 0;
```

- [ ] **Step 4: Implement it on `PersistentKVStore`**

Add to the public section, after `restore_state`:

```cpp
  [[nodiscard]] KeyPage scan_keys(std::string_view prefix,
                                  std::string_view start,
                                  size_t limit) const override {
    KeyPage page;
    if (limit == 0) {
      // Asked for nothing, so nothing is known about what follows.
      return page;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // max(prefix, start): an empty or too-early start snaps to the beginning of
    // the prefix range, so the walk below cannot stop on a key that simply
    // precedes it.
    const std::string_view from = (start > prefix) ? start : prefix;
    page.keys.reserve(limit);

    for (auto it = index_.lower_bound(from); it != index_.end(); ++it) {
      const std::string_view key = *it;
      if (!has_prefix_unlocked(key, prefix)) {
        break; // Ordered, so the first miss ends the range.
      }
      if (page.keys.size() == limit) {
        return page; // Full page; reached_end stays false.
      }
      page.keys.emplace_back(key);
    }

    page.reached_end = true;
    return page;
  }
```

Add the private helper (C++17 has no `starts_with`):

```cpp
  /** @brief True when @p key begins with @p prefix. Pure; no lock needed. */
  [[nodiscard]] static bool has_prefix_unlocked(std::string_view key,
                                                std::string_view prefix) {
    return key.size() >= prefix.size() &&
           key.compare(0, prefix.size(), prefix) == 0;
  }
```

- [ ] **Step 5: Teach the two test fakes the new method**

In `cpp-app/tests/http_handler_test.cpp`, inside `class FakeKVStore : public IKVStore` (its backing member is a `std::map`, which is already ordered):

```cpp
  [[nodiscard]] KeyPage scan_keys(std::string_view prefix,
                                  std::string_view start,
                                  size_t limit) const override {
    KeyPage page;
    if (limit == 0) {
      return page;
    }
    const std::string from = std::string((start > prefix) ? start : prefix);
    for (auto it = entries_.lower_bound(from); it != entries_.end(); ++it) {
      const std::string &key = it->first;
      if (key.size() < prefix.size() ||
          key.compare(0, prefix.size(), prefix) != 0) {
        break;
      }
      if (page.keys.size() == limit) {
        return page;
      }
      page.keys.push_back(key);
    }
    page.reached_end = true;
    return page;
  }
```

Apply the **same** method body to whichever `IKVStore` double
`cpp-app/tests/state_machine_test.cpp` defines. If that double is backed by an
unordered container, first change it to `std::map<std::string, std::string>` so
`lower_bound` is available — it is a test fake and ordering is free there.

- [ ] **Step 6: Run to verify it passes**

```bash
cmake --build cpp-app/build -j4
ctest --test-dir cpp-app/build --output-on-failure
```

Expected: PASS, including every pre-existing test.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i cpp-app/src/storage/kv_store.hpp cpp-app/tests/kv_store_test.cpp \
  cpp-app/tests/http_handler_test.cpp cpp-app/tests/state_machine_test.cpp
git add cpp-app/src/storage/kv_store.hpp cpp-app/tests/kv_store_test.cpp \
  cpp-app/tests/http_handler_test.cpp cpp-app/tests/state_machine_test.cpp
git commit -m "feat: IKVStore::scan_keys with an inclusive start position

Paginated prefix scan over the Task 1 index. The start position is
inclusive rather than exclusive because an exclusive cursor cannot express
'resume past a whole range' -- which is how the reserved __sys: space gets
skipped without a reserved key ever being examined."
```

---

### Task 3: `GET /kv` — paginated key listing

**Files:**
- Modify: `cpp-app/src/network/http_server.hpp`
- Test: `cpp-app/tests/http_handler_test.cpp`

**Interfaces:**
- Consumes: `IKVStore::scan_keys` / `KeyPage` (Task 2); existing `authorize()`, `json_string_array()`, `json_escape()`, `url_decode()`, `auth::is_reserved_key()`, `auth::kSysPrefix`, `AuthContext::key_allowed()`.
- Produces: private statics `percent_encode`, `next_position`, `reserved_range_end`, `pattern_covers_prefix`, `json_percent_array`, and `handle_kv_list`. Task 6 reuses `percent_encode` and `json_percent_array`.

- [ ] **Step 1: Write the failing test**

Append to `cpp-app/tests/http_handler_test.cpp`. Use the fixture shape already in that file (`HandlerTest` with auth off, `AuthenticatedHandlerTest` with auth on).

```cpp
// --- GET /kv (list) --------------------------------------------------------
//
// Every key in the body is PERCENT-ENCODED. Keys are arbitrary bytes; a JSON
// string is Unicode text, so a key holding a raw 0x80 would produce a body no
// parser accepts -- and json_escape() would not save it, because it passes
// bytes >= 0x20 straight through (correct for UTF-8, wrong for arbitrary).

TEST_F(HandlerTest, KvListReturnsPercentEncodedKeysInOrder) {
  store.set("app:b", "2");
  store.set("app:a", "1");
  store.set("other", "3");

  const HttpResponse response = get("/kv?prefix=app%3A");

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.content_type, kJsonContentType);
  EXPECT_EQ(response.body, "{\"keys\":[\"app%3Aa\",\"app%3Ab\"]}");
}

TEST_F(HandlerTest, KvListOmitsCursorAtTheEndOfTheRange) {
  store.set("k1", "v");
  const HttpResponse response = get("/kv?prefix=k");
  // No next_cursor: a client pages until the field is ABSENT, never by
  // comparing the returned count against limit.
  EXPECT_EQ(response.body, "{\"keys\":[\"k1\"]}");
}

TEST_F(HandlerTest, KvListPaginatesWithAnOpaquePosition) {
  for (const char *key : {"k1", "k2", "k3"}) {
    store.set(key, "v");
  }

  const HttpResponse first = get("/kv?prefix=k&limit=2");
  EXPECT_EQ(first.status_code, 200);
  // The position is the last emitted key plus a NUL, so it reveals only a key
  // the caller has already been shown.
  EXPECT_EQ(first.body,
            "{\"keys\":[\"k1\",\"k2\"],\"next_cursor\":\"k2%00\"}");

  const HttpResponse second = get("/kv?prefix=k&limit=2&cursor=k2%00");
  EXPECT_EQ(second.body, "{\"keys\":[\"k3\"]}");
}

TEST_F(HandlerTest, KvListEncodesKeysThatAreNotValidUtf8) {
  store.set(std::string("k\x80\x01", 3), "v");
  const HttpResponse response = get("/kv?prefix=k");
  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.body, "{\"keys\":[\"k%80%01\"]}");
}

TEST_F(HandlerTest, KvListNeverRevealsAReservedKeyOrPosition) {
  store.set("__sys:user:alice", "record");
  store.set("a", "v");

  const HttpResponse response = get("/kv?limit=1");
  EXPECT_EQ(response.status_code, 200);
  // The reserved range is jumped over as a range, so neither the keys array
  // nor the cursor can name a user record.
  EXPECT_EQ(response.body, "{\"keys\":[\"a\"]}");
  EXPECT_EQ(response.body.find("__sys"), std::string::npos);
}

TEST_F(HandlerTest, KvListRefusesAReservedPrefix) {
  const HttpResponse response = get("/kv?prefix=__sys%3A");
  EXPECT_EQ(response.status_code, 403);
  EXPECT_NE(response.body.find("are reserved"), std::string::npos);
}

TEST_F(HandlerTest, KvListRejectsABadLimit) {
  EXPECT_EQ(get("/kv?limit=0").status_code, 400);
  EXPECT_EQ(get("/kv?limit=-1").status_code, 400);
  EXPECT_EQ(get("/kv?limit=abc").status_code, 400);
  EXPECT_EQ(get("/kv?limit=abc").body,
            "{\"error\":\"limit must be an integer between 1 and 500\"}");
}

TEST_F(HandlerTest, KvListClampsAnOverLargeLimit) {
  // A cap is not a client error.
  EXPECT_EQ(get("/kv?limit=100000").status_code, 200);
}

TEST_F(HandlerTest, KvListRejectsMalformedPercentEncoding) {
  const HttpResponse response = get("/kv?prefix=%zz");
  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(response.body,
            "{\"error\":\"malformed percent-encoding in the query "
            "string\"}");
}

TEST_F(HandlerTest, KvListDoesNotCollideWithTheSingleKeyRoute) {
  // "/kv" has no trailing slash; "/kv/" is still the empty-key 400.
  EXPECT_EQ(get("/kv/").status_code, 400);
}

TEST_F(AuthenticatedHandlerTest, KvListRequiresTheReadClass) {
  const HttpResponse response = get_as("writer-only", "pw", "/kv?prefix=app%3A");
  EXPECT_EQ(response.status_code, 403);
  EXPECT_EQ(response.body, "{\"error\":\"permission denied\"}");
}

TEST_F(AuthenticatedHandlerTest, KvListAppliesKeyPatternsAndRequiresACoveredPrefix) {
  store.set("app:mine", "v");
  store.set("other:theirs", "v");
  add_user("scoped", "pw", {"read"}, {"app:*"});

  // Inside the allowance: fine.
  const HttpResponse allowed = get_as("scoped", "pw", "/kv?prefix=app%3A");
  EXPECT_EQ(allowed.status_code, 200);
  EXPECT_EQ(allowed.body, "{\"keys\":[\"app%3Amine\"]}");

  // Outside it: refused, rather than scanning keys it may not see. This is
  // what keeps a filtered-out key name out of the returned position.
  const HttpResponse refused = get_as("scoped", "pw", "/kv?prefix=other%3A");
  EXPECT_EQ(refused.status_code, 403);
  EXPECT_EQ(refused.body,
            "{\"error\":\"prefix must fall within your permitted key "
            "patterns\"}");

  // And an unrestricted scan is refused for the same reason.
  EXPECT_EQ(get_as("scoped", "pw", "/kv").status_code, 403);
}
```

If `HandlerTest` has no `get(path)` / `get_as(user, pw, path)` / `add_user(...)`
helper, add the missing ones next to the fixture's existing request builders,
following the shape already used there — a `HttpRequest` with `method = "GET"`,
`path` set to the part before `?`, `query_string` set to the part after, and for
`get_as` an `Authorization: Basic <base64>` header built with the file's existing
base64 helper.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build cpp-app/build -j4 && ctest --test-dir cpp-app/build --output-on-failure
```

Expected: the `KvList*` tests FAIL with `404 {"error":"not found"}` (the route does not exist).

- [ ] **Step 3: Add the helpers**

In `cpp-app/src/network/http_server.hpp`, add `#include <algorithm>` and `#include <cstdint>` if absent, then add to `KVHttpHandler`'s private section:

```cpp
  /** @brief Default page size for GET /kv. */
  static constexpr size_t kKeyListDefaultLimit = 100;

  /** @brief Largest page GET /kv will serve. Clamped to, not rejected. */
  static constexpr size_t kKeyListMaxLimit = 500;

  /** @brief Keys pulled from the store per scan_keys call. */
  static constexpr size_t kKeyScanChunk = 256;

  /**
   * @brief Percent-encode arbitrary bytes (RFC 3986 unreserved set only).
   *
   * Every key in a JSON body goes through this. Keys are arbitrary bytes and a
   * JSON string is Unicode text: a key holding a raw 0x80 would produce a body
   * no parser accepts, and json_escape() cannot help -- it passes bytes >= 0x20
   * through untouched, which is right for UTF-8 payloads and wrong for
   * arbitrary ones. Encoding to the unreserved set also means the output can be
   * pasted straight back into a query string or into /kv/{key}, which decodes.
   */
  [[nodiscard]] static std::string percent_encode(const std::string &raw) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
      const auto byte = static_cast<unsigned char>(c);
      const bool unreserved =
          (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
          byte == '.' || byte == '~';
      if (unreserved) {
        out.push_back(static_cast<char>(byte));
        continue;
      }
      out.push_back('%');
      out.push_back(kHexDigits[(byte >> 4) & 0x0F]);
      out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
  }

  /** @brief Render keys as a JSON array of percent-encoded strings. */
  [[nodiscard]] static std::string
  json_percent_array(const std::vector<std::string> &keys) {
    std::vector<std::string> encoded;
    encoded.reserve(keys.size());
    for (const std::string &key : keys) {
      encoded.push_back(percent_encode(key));
    }
    return json_string_array(encoded);
  }

  /**
   * @brief The first scan position strictly after @p key.
   *
   * Any key greater than @p key either extends it -- and so is >= key + '\0' --
   * or diverges at an earlier byte and is greater than that too. Works even
   * when @p key itself contains NUL bytes, because the comparison is by length
   * and bytes, not by terminator.
   */
  [[nodiscard]] static std::string next_position(const std::string &key) {
    std::string next = key;
    next.push_back('\0');
    return next;
  }

  /**
   * @brief The first scan position past every reserved (`__sys:`) key.
   *
   * The reserved prefix ends in ':', so incrementing that last byte gives
   * "__sys;" -- which sorts after every "__sys:..." key and before anything
   * that merely starts with "__sys;". Lets the whole reserved range be skipped
   * in ONE lower_bound, so a reserved key is never examined and therefore can
   * never surface in a listing or a cursor.
   */
  [[nodiscard]] static std::string reserved_range_end() {
    std::string past(auth::kSysPrefix);
    past.back() = static_cast<char>(past.back() + 1);
    return past;
  }

  /**
   * @brief True when every key carrying @p prefix matches @p pattern.
   *
   * Only a trailing-'*' literal glob can cover an entire prefix range: a '?' or
   * an interior '*' constrains keys further along, so some key with the prefix
   * would not match, and an exact pattern (no wildcard) covers at most one key.
   *
   * Used to require that a caller with restricted patterns scans INSIDE its own
   * allowance. With that rule, nothing is filtered out mid-scan, so the
   * returned position is always derived from a key the caller was allowed to
   * see.
   */
  [[nodiscard]] static bool pattern_covers_prefix(const std::string &pattern,
                                                  const std::string &prefix) {
    if (pattern.find('?') != std::string::npos) {
      return false;
    }
    const size_t star = pattern.find('*');
    if (star == std::string::npos || star != pattern.size() - 1) {
      return false;
    }
    const std::string literal = pattern.substr(0, star);
    return prefix.size() >= literal.size() &&
           prefix.compare(0, literal.size(), literal) == 0;
  }
```

- [ ] **Step 4: Add `handle_kv_list`**

Add to `KVHttpHandler`'s private section:

```cpp
  /**
   * @brief GET /kv?prefix=&cursor=&limit= - one page of key NAMES.
   *
   * Names only: a page of 500 values could be hundreds of megabytes, and the
   * console fetches a value only when a key is clicked.
   *
   * The page is cut AFTER filtering, not before, which is what keeps the
   * returned position safe: because a page ends on an EMITTED key, the position
   * never names a key the caller could not see. The examine budget bounds the
   * work when filtering is heavy; combined with the covered-prefix rule below,
   * a restricted caller cannot reach it by filtering alone.
   */
  [[nodiscard]] HttpResponse
  handle_kv_list(const HttpRequest &request,
                 const auth::AuthContext &identity) const {
    const std::map<std::string, std::string> params = request.query_params();

    // decode_plus = true: these are QUERY parameters, where '+' means space
    // (see the note on url_decode). Note query_params() returns values
    // UNDECODED, so this cannot be skipped.
    const auto decode_param = [&params](const char *name)
        -> std::optional<std::optional<std::string>> {
      const auto it = params.find(name);
      if (it == params.end()) {
        return std::optional<std::optional<std::string>>(
            std::optional<std::string>(std::string()));
      }
      const std::optional<std::string> decoded = url_decode(it->second, true);
      if (!decoded.has_value()) {
        return std::nullopt;
      }
      return std::optional<std::optional<std::string>>(decoded);
    };

    const auto raw_prefix = decode_param("prefix");
    const auto raw_cursor = decode_param("cursor");
    if (!raw_prefix.has_value() || !raw_cursor.has_value()) {
      return HttpResponse::json_error(
          400, "malformed percent-encoding in the query string");
    }
    const std::string prefix = **raw_prefix;
    const std::string cursor = **raw_cursor;

    size_t limit = kKeyListDefaultLimit;
    const auto limit_param = params.find("limit");
    if (limit_param != params.end()) {
      const std::optional<size_t> parsed = parse_list_limit(limit_param->second);
      if (!parsed.has_value()) {
        return HttpResponse::json_error(
            400, "limit must be an integer between 1 and 500");
      }
      limit = *parsed;
    }

    // Reserved space is not addressable through a data route in EITHER
    // direction, listing included.
    if (std::optional<HttpResponse> refusal = reject_reserved_key(prefix)) {
      return *refusal;
    }
    if (!identity.has_class(auth::CommandClass::kRead)) {
      return HttpResponse::json_error(403, "permission denied");
    }
    if (!prefix_is_covered(identity, prefix)) {
      return HttpResponse::json_error(
          403, "prefix must fall within your permitted key patterns");
    }

    const std::string reserved_end = reserved_range_end();
    const size_t budget = std::max<size_t>(1000, limit * 10);

    std::vector<std::string> emitted;
    std::string position = cursor;
    size_t examined = 0;
    bool exhausted = false;

    while (emitted.size() < limit && examined < budget) {
      const size_t chunk = std::min(kKeyScanChunk, budget - examined);
      const IKVStore::KeyPage page = store_.scan_keys(prefix, position, chunk);
      if (page.keys.empty()) {
        // chunk is always >= 1 here, so an empty page means the range ended.
        exhausted = page.reached_end;
        break;
      }

      bool jumped = false;
      for (const std::string &key : page.keys) {
        ++examined;
        if (auth::is_reserved_key(key)) {
          // Jump the WHOLE reserved range rather than filtering key by key, so
          // no reserved key can ever become the returned position.
          position = reserved_end;
          jumped = true;
          break;
        }
        position = next_position(key);
        if (!identity.key_allowed(key)) {
          continue;
        }
        emitted.push_back(key);
        if (emitted.size() == limit) {
          break;
        }
      }

      if (!jumped && page.reached_end && emitted.size() < limit) {
        exhausted = true;
        break;
      }
    }

    std::string body = "{\"keys\":" + json_percent_array(emitted);
    if (!exhausted) {
      body += ",\"next_cursor\":\"" + percent_encode(position) + "\"";
    }
    body += "}";
    return HttpResponse::json(200, body);
  }

  /**
   * @brief Parse and clamp the `limit` query parameter.
   * @return nullopt when it is not an integer in 1..kKeyListMaxLimit.
   *
   * An over-large value is CLAMPED, not rejected: a server-side cap is not a
   * client error. Zero is rejected, because a page of nothing cannot advance a
   * cursor and would leave a client unable to make progress.
   */
  [[nodiscard]] static std::optional<size_t>
  parse_list_limit(const std::string &raw) {
    if (raw.empty()) {
      return std::nullopt;
    }
    for (const char c : raw) {
      if (c < '0' || c > '9') {
        return std::nullopt; // Covers "-1" and "abc" alike.
      }
    }
    unsigned long long value = 0;
    try {
      value = std::stoull(raw);
    } catch (const std::exception &) {
      return std::nullopt; // Absurdly long digit strings.
    }
    if (value == 0) {
      return std::nullopt;
    }
    return std::min<size_t>(static_cast<size_t>(value), kKeyListMaxLimit);
  }

  /** @brief True when some held pattern covers every key under @p prefix. */
  [[nodiscard]] static bool prefix_is_covered(const auth::AuthContext &identity,
                                              const std::string &prefix) {
    for (const std::string &pattern : identity.patterns) {
      if (pattern_covers_prefix(pattern, prefix)) {
        return true;
      }
    }
    return false;
  }
```

Add `#include <cstdlib>` / `#include <stdexcept>` only if the file does not
already pull them in transitively; `std::stoull` needs `<string>`, which is
already included.

- [ ] **Step 5: Route it and label it**

In `route_request()`, immediately **before** the `kKvPathPrefix` check:

```cpp
    // The list surface. Checked before /kv/{key}: the prefixes cannot collide
    // ("/kv" has no trailing slash, kKvPathPrefix does), but the order makes
    // that obvious rather than incidental.
    if (request.method == "GET" && request.path == "/kv") {
      return handle_kv_list(request, identity);
    }
```

In `route_label()`, before the `kKvPathPrefix` branch:

```cpp
    if (request.path == "/kv") {
      return request.method + " /kv";
    }
```

- [ ] **Step 6: Run to verify it passes**

```bash
cmake --build cpp-app/build -j4 && ctest --test-dir cpp-app/build --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i cpp-app/src/network/http_server.hpp cpp-app/tests/http_handler_test.cpp
git add cpp-app/src/network/http_server.hpp cpp-app/tests/http_handler_test.cpp
git commit -m "feat: GET /kv paginated key listing

Names only, percent-encoded (keys are arbitrary bytes; a JSON string is
Unicode text). The page is cut after filtering so the returned position is
always derived from an emitted key, the reserved __sys: range is jumped as a
range, and a caller with restricted patterns must scan inside its own
allowance -- which together keep a key name the caller cannot read out of
both the listing and the cursor."
```

---

### Task 4: `RaftNode.Status` — proto and the Go side

**Files:**
- Modify: `proto/consensus.proto`
- Modify: `go-sidecar/pb/consensus.pb.go`, `go-sidecar/pb/consensus_grpc.pb.go` (regenerated, not hand-edited)
- Modify: `go-sidecar/internal/raftnode/node.go`
- Modify: `go-sidecar/internal/rpc/server.go`
- Modify: `go-sidecar/cmd/sidecar/main.go`
- Test: `go-sidecar/internal/rpc/server_test.go`

**Interfaces:**
- Consumes: existing `Node.Stats()`, `Node.FirstLogIndex()`.
- Produces:
  - proto: `rpc Status(StatusRequest) returns (StatusResponse)`, `message Peer`, `message StatusRequest`, `message StatusResponse`.
  - Go: `func (n *Node) ID() string`, `func (n *Node) LeaderWithID() (addr string, id string)`, `func (n *Node) Configuration() ([]raft.Server, error)`, `type rpc.RaftStatusReporter interface`, `func (s *Server) WithStatusReporter(r RaftStatusReporter) *Server`, `func (s *Server) Status(ctx, *pb.StatusRequest) (*pb.StatusResponse, error)`.

  Task 5 calls `Status` over gRPC from C++.

- [ ] **Step 1: Extend the proto**

In `proto/consensus.proto`, add to `service RaftNode` after `Read`:

```proto
  // The console phase -- this node's own view of the cluster, for the operator
  // console's overview page. Read-only: it touches no log and needs no leader,
  // so EVERY node answers for itself, including its own belief about who leads.
  // That is deliberate: the page polls all three and shows three independent
  // views, and a disagreement between them is the information.
  rpc Status(StatusRequest) returns (StatusResponse);
```

Add the messages at the end of the file:

```proto
// One member of the committed raft configuration.
message Peer {
  string id = 1;
  string address = 2;
  // "Voter", "Nonvoter" or "Staging", as raft spells it.
  string suffrage = 3;
}

// Empty today; exists so the request can gain fields without changing the
// method signature.
message StatusRequest {}

message StatusResponse {
  string node_id = 1;
  // "Leader", "Follower", "Candidate", "Shutdown" -- raft's own spelling.
  string state = 2;
  uint64 term = 3;
  string leader_id = 4;
  string leader_addr = 5;
  repeated Peer peers = 6;

  uint64 first_log_index = 7;
  uint64 last_log_index = 8;
  uint64 applied_index = 9;
  uint64 commit_index = 10;
  uint64 last_snapshot_index = 11;

  // A PARTIAL failure: the response is otherwise usable but one field could not
  // be read (a failed log-store read, or an unreadable configuration). Empty
  // when everything was read. A total failure is a non-OK gRPC status instead,
  // because there is then nothing truthful to put in the fields.
  string error = 12;
}
```

- [ ] **Step 2: Regenerate the Go stubs and verify they fail to satisfy the server**

```bash
cd go-sidecar
protoc -I ../proto --go_out=. --go-grpc_out=. ../proto/consensus.proto
gofmt -l . && go build ./...
```

Expected: build **succeeds** (the generated `UnimplementedRaftNodeServer` supplies a default `Status`). This step exists to confirm regeneration worked — `git status` must show both files in `pb/` modified.

- [ ] **Step 3: Write the failing Go test**

Append to `go-sidecar/internal/rpc/server_test.go`. Follow the file's existing fake style.

```go
// --- Status ----------------------------------------------------------------

// fakeStatusReporter is a scriptable RaftStatusReporter.
type fakeStatusReporter struct {
	id       string
	stats    map[string]string
	addr     string
	leaderID string
	servers  []raft.Server
	firstIdx uint64
	firstErr error
	confErr  error
}

func (f *fakeStatusReporter) ID() string                   { return f.id }
func (f *fakeStatusReporter) Stats() map[string]string      { return f.stats }
func (f *fakeStatusReporter) LeaderWithID() (string, string) {
	return f.addr, f.leaderID
}
func (f *fakeStatusReporter) Configuration() ([]raft.Server, error) {
	return f.servers, f.confErr
}
func (f *fakeStatusReporter) FirstLogIndex() (uint64, error) {
	return f.firstIdx, f.firstErr
}

func newStatusReporter() *fakeStatusReporter {
	return &fakeStatusReporter{
		id: "node1",
		stats: map[string]string{
			"state":               "Leader",
			"term":                "4",
			"last_log_index":      "118",
			"applied_index":       "118",
			"commit_index":        "118",
			"last_snapshot_index": "0",
		},
		addr:     "node1:8088",
		leaderID: "node1",
		servers: []raft.Server{
			{ID: "node1", Address: "node1:8088", Suffrage: raft.Voter},
			{ID: "node2", Address: "node2:8088", Suffrage: raft.Voter},
		},
		firstIdx: 1,
	}
}

func TestStatusReportsRaftState(t *testing.T) {
	reporter := newStatusReporter()
	server := NewServer(nil, nil).WithStatusReporter(reporter)

	resp, err := server.Status(context.Background(), &pb.StatusRequest{})
	if err != nil {
		t.Fatalf("Status returned error: %v", err)
	}

	if resp.NodeId != "node1" || resp.State != "Leader" || resp.Term != 4 {
		t.Errorf("identity/state wrong: %+v", resp)
	}
	if resp.LeaderId != "node1" || resp.LeaderAddr != "node1:8088" {
		t.Errorf("leader wrong: %+v", resp)
	}
	if resp.FirstLogIndex != 1 || resp.LastLogIndex != 118 ||
		resp.AppliedIndex != 118 || resp.CommitIndex != 118 {
		t.Errorf("indices wrong: %+v", resp)
	}
	if len(resp.Peers) != 2 {
		t.Fatalf("want 2 peers, got %d", len(resp.Peers))
	}
	if resp.Peers[1].Id != "node2" || resp.Peers[1].Address != "node2:8088" ||
		resp.Peers[1].Suffrage != "Voter" {
		t.Errorf("peer wrong: %+v", resp.Peers[1])
	}
	if resp.Error != "" {
		t.Errorf("want no partial error, got %q", resp.Error)
	}
}

func TestStatusSurvivesAFailedLogStoreRead(t *testing.T) {
	reporter := newStatusReporter()
	reporter.firstErr = errors.New("boltdb is unhappy")

	server := NewServer(nil, nil).WithStatusReporter(reporter)
	resp, err := server.Status(context.Background(), &pb.StatusRequest{})
	if err != nil {
		t.Fatalf("Status must keep answering when one field fails: %v", err)
	}
	// Partial failure is a FIELD, not a status: the console needs the rest of
	// the response, and the same reasoning already governs /status's
	// log_store_error.
	if resp.FirstLogIndex != 0 {
		t.Errorf("want first_log_index 0 on a failed read, got %d",
			resp.FirstLogIndex)
	}
	if resp.Error == "" {
		t.Error("want the partial failure reported in Error")
	}
	if resp.LastLogIndex != 118 {
		t.Errorf("the readable fields must survive: %+v", resp)
	}
}

func TestStatusWithoutAReporterIsUnimplemented(t *testing.T) {
	server := NewServer(nil, nil)

	_, err := server.Status(context.Background(), &pb.StatusRequest{})
	if err == nil {
		t.Fatal("want an error when no reporter was supplied")
	}
	// A total failure has nothing truthful to put in the fields, so it is a
	// gRPC status rather than a zero-valued response that reads as a cluster
	// which has lost quorum.
	if status.Code(err) != codes.Unimplemented {
		t.Errorf("want Unimplemented, got %v", status.Code(err))
	}
}
```

Add to that file's imports whatever is missing: `errors`, `github.com/hashicorp/raft`, `google.golang.org/grpc/codes`, `google.golang.org/grpc/status`.

- [ ] **Step 4: Run to verify it fails**

```bash
cd go-sidecar && go test -race ./internal/rpc/
```

Expected: **compile failure** — `server.WithStatusReporter undefined`.

- [ ] **Step 5: Add the three `Node` accessors**

In `go-sidecar/internal/raftnode/node.go`, after `FirstLogIndex`:

```go
// ID returns this node's raft server ID.
//
// Read from the stored config rather than from raft: raft has no accessor for
// its own ID, and the config is the thing that decided it.
func (n *Node) ID() string {
	return n.config.NodeID
}

// LeaderWithID returns the current leader's raft address and server ID.
//
// LeaderAddr() already returns the address alone and is kept for the callers
// that only need it; the console's overview page names the leader, so it needs
// the ID too.
func (n *Node) LeaderWithID() (string, string) {
	addr, id := n.Raft.LeaderWithID()
	return string(addr), string(id)
}

// Configuration returns the servers in the committed raft configuration.
//
// This is what lets the console draw every member of the cluster from a single
// node's answer, rather than needing one reachable node per member.
func (n *Node) Configuration() ([]raft.Server, error) {
	future := n.Raft.GetConfiguration()
	if err := future.Error(); err != nil {
		return nil, fmt.Errorf("failed to read raft configuration: %w", err)
	}
	return future.Configuration().Servers, nil
}
```

- [ ] **Step 6: Add the interface, the setter and `Status`**

In `go-sidecar/internal/rpc/server.go`, after the `LocalReader` interface:

```go
// RaftStatusReporter exposes this node's own raft state for RaftNode.Status.
//
// Consumer-side, like RaftProposer and LocalReader, so internal/rpc still does
// not import internal/raftnode. []raft.Server is a third-party type both
// packages already depend on, not a project type, so using it here does not
// invert the dependency this interface exists to keep pointing one way.
type RaftStatusReporter interface {
	// ID is this node's raft server ID.
	ID() string
	// Stats is raft's own counter map, keyed as hashicorp/raft keys it
	// ("state", "term", "last_log_index", "applied_index", "commit_index",
	// "last_snapshot_index").
	Stats() map[string]string
	// LeaderWithID returns the leader's raft address and server ID; both are
	// empty while no leader is known.
	LeaderWithID() (addr string, id string)
	// Configuration returns the committed cluster membership.
	Configuration() ([]raft.Server, error)
	// FirstLogIndex is where the log now begins -- the only direct evidence
	// that compaction happened.
	FirstLogIndex() (uint64, error)
}
```

Add the field to `Server`:

```go
	statusReporter RaftStatusReporter
```

Add the setter after `WithLocalReader`:

```go
// WithStatusReporter supplies the raft state RaftNode.Status reports.
//
// Separate from NewServer for the same reason WithLocalReader is: Status is
// optional, and without it the RPC answers Unimplemented rather than a
// zero-valued response that would render as a cluster with no peers and no
// leader. Propose -- the path every prior phase depends on -- is untouched.
func (s *Server) WithStatusReporter(reporter RaftStatusReporter) *Server {
	s.statusReporter = reporter
	return s
}
```

Add the handler:

```go
// Status reports this node's own view of the cluster (console phase).
//
// Needs no leader and no log access, so every node answers for itself. A field
// that cannot be read is reported in StatusResponse.error while the rest of the
// response stands -- the same choice management's /status already makes for
// log_store_error, and for the same reason: the readiness and dashboard callers
// need the other fields more than they need a 500.
func (s *Server) Status(ctx context.Context, req *pb.StatusRequest) (*pb.StatusResponse, error) {
	if s.statusReporter == nil {
		return nil, status.Error(codes.Unimplemented,
			"this sidecar was built without a status reporter")
	}

	reporter := s.statusReporter
	stats := reporter.Stats()
	leaderAddr, leaderID := reporter.LeaderWithID()

	resp := &pb.StatusResponse{
		NodeId:            reporter.ID(),
		State:             stats["state"],
		Term:              parseRaftStat(stats, "term"),
		LeaderId:          leaderID,
		LeaderAddr:        leaderAddr,
		LastLogIndex:      parseRaftStat(stats, "last_log_index"),
		AppliedIndex:      parseRaftStat(stats, "applied_index"),
		CommitIndex:       parseRaftStat(stats, "commit_index"),
		LastSnapshotIndex: parseRaftStat(stats, "last_snapshot_index"),
	}

	// Partial failures accumulate into one field rather than short-circuiting:
	// a console that can show the log indices but not the peer list is more
	// useful than an error.
	var partial []string

	firstIndex, err := reporter.FirstLogIndex()
	if err != nil {
		partial = append(partial, err.Error())
	} else {
		resp.FirstLogIndex = firstIndex
	}

	servers, err := reporter.Configuration()
	if err != nil {
		partial = append(partial, err.Error())
	} else {
		resp.Peers = make([]*pb.Peer, 0, len(servers))
		for _, server := range servers {
			resp.Peers = append(resp.Peers, &pb.Peer{
				Id:       string(server.ID),
				Address:  string(server.Address),
				Suffrage: server.Suffrage.String(),
			})
		}
	}

	resp.Error = strings.Join(partial, "; ")
	return resp, nil
}

// parseRaftStat reads one decimal counter out of raft's Stats() map.
//
// Raft reports them as strings and a missing or unparseable entry is not worth
// failing a whole status response over, so it reads as 0. The keys are raft's,
// not ours; see RaftStatusReporter.Stats.
func parseRaftStat(stats map[string]string, key string) uint64 {
	value, err := strconv.ParseUint(stats[key], 10, 64)
	if err != nil {
		return 0
	}
	return value
}
```

Add `strconv`, `google.golang.org/grpc/codes` and `google.golang.org/grpc/status` to the imports (`strings` is already there).

- [ ] **Step 7: Add the compile-time interface check**

In `go-sidecar/internal/rpc/server_test.go`, beside the file's existing checks:

```go
// Lives in the TEST file, like management's RaftControl check: putting it in
// raftnode would make raftnode import rpc and invert the dependency
// RaftStatusReporter exists to break.
var _ RaftStatusReporter = (*raftnode.Node)(nil)
```

Add `my-raft-sidecar/internal/raftnode` to the test file's imports.

- [ ] **Step 8: Wire it in `main.go`**

In `go-sidecar/cmd/sidecar/main.go`, change the server construction (~line 158):

```go
	grpcServer := rpc.NewServer(node, forwarder).
		WithLocalReader(storeReader).
		WithStatusReporter(node)
```

- [ ] **Step 9: Run to verify it passes**

```bash
cd go-sidecar
test -z "$(gofmt -l .)" && go vet ./... && go test -race ./...
```

Expected: PASS, every package.

- [ ] **Step 10: Commit**

```bash
cd /Users/burhankapdawala/Documents/mine/p-repos/RaftKV
git add proto/consensus.proto go-sidecar/pb go-sidecar/internal/raftnode/node.go \
  go-sidecar/internal/rpc/server.go go-sidecar/internal/rpc/server_test.go \
  go-sidecar/cmd/sidecar/main.go
git commit -m "feat: RaftNode.Status RPC reporting this node's raft state

Read-only and leader-free, so every node answers for itself -- which is what
makes the console's overview page honest: it polls all three and a
disagreement between them is the information.

A field that cannot be read is reported in StatusResponse.error while the
rest of the response stands, matching what management's /status already does
with log_store_error. A MISSING reporter is Unimplemented instead, because a
zero-valued response renders as a cluster that has lost quorum."
```

---

### Task 5: `IRaftClient::status()`, `IStoreStats` and `GET /cluster/status`

**Files:**
- Modify: `cpp-app/src/raft/raft_client.hpp`
- Modify: `cpp-app/src/storage/kv_store.hpp` (add `IStoreStats`)
- Modify: `cpp-app/src/network/http_server.hpp`
- Modify: `cpp-app/src/main.cpp` (pass the stats seam to the handler)
- Test: `cpp-app/tests/http_handler_test.cpp`

**Interfaces:**
- Consumes: proto `StatusRequest`/`StatusResponse`/`Peer` (Task 4); `percent_encode`, `json_string_array` (Task 3).
- Produces:
  - `struct kvdb::RaftPeer { std::string id, address, suffrage; }`
  - `struct kvdb::StatusResult` with `bool ok; std::string error; std::string node_id, state, leader_id, leader_addr, partial_error; std::uint64_t term, first_log_index, last_log_index, applied_index, commit_index, last_snapshot_index; std::vector<RaftPeer> peers;` and factories `StatusResult::failure(std::string)`.
  - `[[nodiscard]] virtual StatusResult IRaftClient::status() = 0;`
  - `class kvdb::IStoreStats` with `[[nodiscard]] virtual std::size_t key_count() const = 0;` and `[[nodiscard]] virtual std::size_t wal_size_bytes() const = 0;`
  - `KVHttpHandler(IRaftClient&, const IKVStore&, const auth::IAuthEngine&, const IStoreStats&)` — a **fourth** constructor parameter.

- [ ] **Step 1: Write the failing test**

Append to `cpp-app/tests/http_handler_test.cpp`:

```cpp
// --- GET /cluster/status ---------------------------------------------------

TEST_F(HandlerTest, ClusterStatusRendersRaftAndLocalState) {
  raft.status_result = StatusResult{};
  raft.status_result.ok = true;
  raft.status_result.node_id = "node1";
  raft.status_result.state = "Leader";
  raft.status_result.term = 4;
  raft.status_result.leader_id = "node1";
  raft.status_result.leader_addr = "node1:8088";
  raft.status_result.first_log_index = 1;
  raft.status_result.last_log_index = 118;
  raft.status_result.applied_index = 117;
  raft.status_result.commit_index = 118;
  raft.status_result.last_snapshot_index = 0;
  raft.status_result.peers = {RaftPeer{"node1", "node1:8088", "Voter"},
                              RaftPeer{"node2", "node2:8088", "Voter"}};
  stats.keys = 42;
  stats.wal_bytes = 9310;

  const HttpResponse response = get("/cluster/status");

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.content_type, kJsonContentType);
  EXPECT_EQ(response.body,
            "{\"node_id\":\"node1\",\"state\":\"Leader\",\"term\":4,"
            "\"leader_id\":\"node1\",\"leader_addr\":\"node1:8088\","
            "\"peers\":["
            "{\"id\":\"node1\",\"address\":\"node1:8088\","
            "\"suffrage\":\"Voter\"},"
            "{\"id\":\"node2\",\"address\":\"node2:8088\","
            "\"suffrage\":\"Voter\"}],"
            "\"first_log_index\":1,\"last_log_index\":118,"
            "\"applied_index\":117,\"commit_index\":118,"
            "\"last_snapshot_index\":0,"
            "\"key_count\":42,\"wal_bytes\":9310,"
            "\"auth_enabled\":false}");
}

TEST_F(HandlerTest, ClusterStatusIs502WhenTheSidecarIsUnreachable) {
  raft.status_result = StatusResult::failure("sidecar unreachable");

  const HttpResponse response = get("/cluster/status");

  // 502, not 200-with-zeros. A dashboard rendering "term 0, no peers, not
  // leader" for a node whose sidecar is merely unreachable looks exactly like
  // a cluster that has lost quorum, which is worse than an error.
  EXPECT_EQ(response.status_code, 502);
  EXPECT_EQ(response.body, "{\"error\":\"sidecar unreachable\"}");
}

TEST_F(HandlerTest, ClusterStatusReportsAPartialFailure) {
  raft.status_result = StatusResult{};
  raft.status_result.ok = true;
  raft.status_result.node_id = "node1";
  raft.status_result.state = "Follower";
  raft.status_result.partial_error = "failed to read first log index";

  const HttpResponse response = get("/cluster/status");
  EXPECT_EQ(response.status_code, 200);
  EXPECT_NE(response.body.find("\"error\":\"failed to read first log index\""),
            std::string::npos);
}

TEST_F(HandlerTest, ClusterStatusRejectsANonGetMethod) {
  HttpRequest request;
  request.method = "POST";
  request.path = "/cluster/status";
  const HttpResponse response = handler.route_request(request);
  EXPECT_EQ(response.status_code, 405);
}

TEST_F(AuthenticatedHandlerTest, ClusterStatusRequiresTheReadClassOnly) {
  // read-class, no key pattern check: it addresses no key, and requiring admin
  // would hide the overview page from the users most likely to open it.
  add_user("reader", "pw", {"read"}, {"nothing:*"});
  raft.status_result = StatusResult{};
  raft.status_result.ok = true;
  raft.status_result.node_id = "node1";

  EXPECT_EQ(get_as("reader", "pw", "/cluster/status").status_code, 200);
  EXPECT_EQ(get_as("writer-only", "pw", "/cluster/status").status_code, 403);
  EXPECT_EQ(get("/cluster/status").status_code, 401);
}

TEST_F(HandlerTest, ClusterStatusReportsAuthEnabled) {
  // The console shows an "unauthenticated" banner off this field, so it must
  // reflect the engine's real configuration rather than the request.
  raft.status_result = StatusResult{};
  raft.status_result.ok = true;
  EXPECT_NE(get("/cluster/status").body.find("\"auth_enabled\":false"),
            std::string::npos);
}
```

Extend the fixture's fakes in the same file:

```cpp
/** @brief In-memory IStoreStats double. */
class FakeStoreStats : public IStoreStats {
public:
  std::size_t keys = 0;
  std::size_t wal_bytes = 0;

  [[nodiscard]] std::size_t key_count() const override { return keys; }
  [[nodiscard]] std::size_t wal_size_bytes() const override {
    return wal_bytes;
  }
};
```

Add `StatusResult status_result;` to the existing `FakeRaftClient` plus:

```cpp
  [[nodiscard]] StatusResult status() override { return status_result; }
```

Add `FakeStoreStats stats;` to both fixtures and pass it as the handler's fourth
constructor argument wherever the fixtures build `handler`.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build cpp-app/build -j4
```

Expected: **compile failure** — `'IStoreStats' has not been declared`.

- [ ] **Step 3: Add `IStoreStats`**

In `cpp-app/src/storage/kv_store.hpp`, above `class IKVStore`:

```cpp
/**
 * @brief Observability seam over a store: counters, not storage semantics.
 *
 * Deliberately SEPARATE from IKVStore. key_count() and wal_size_bytes() were
 * kept off IKVStore on purpose -- widening the storage interface for
 * observability forces every test fake to implement it -- and that decision
 * still holds. What changed is that the HTTP handler is now a consumer of these
 * counters (GET /cluster/status), and it holds interfaces, not the concrete
 * store. So they get their own seam rather than moving.
 *
 * PersistentKVStore implements both interfaces; main.cpp holds the concrete
 * type and injects it.
 */
class IStoreStats {
public:
  virtual ~IStoreStats() = default;

  /** @brief Keys currently held in the local store. */
  [[nodiscard]] virtual std::size_t key_count() const = 0;

  /** @brief Current size of the write-ahead log, in bytes. */
  [[nodiscard]] virtual std::size_t wal_size_bytes() const = 0;
};
```

Change the class declaration to inherit both, and mark the two existing methods
`override` (they already have the right signatures — confirm both return
`size_t`; if either is declared `size_t` rather than `std::size_t` that is the
same type and needs no change):

```cpp
class PersistentKVStore final : public IKVStore, public IStoreStats {
```

```cpp
  [[nodiscard]] size_t key_count() const override {
```

```cpp
  [[nodiscard]] size_t wal_size_bytes() const override {
```

Add `var`-style compile-time proof is a Go idiom; in C++ the `override` keyword
already enforces it.

- [ ] **Step 4: Add `RaftPeer`, `StatusResult` and `IRaftClient::status()`**

In `cpp-app/src/raft/raft_client.hpp`, add `#include <cstdint>` and
`#include <vector>`, then after `ReadResult`:

```cpp
/** @brief One member of the committed raft configuration. */
struct RaftPeer {
  std::string id;
  std::string address;
  std::string suffrage;
};

/**
 * @brief Outcome of a cluster-status query.
 *
 * @c ok distinguishes "the sidecar answered" from "it did not". The numeric
 * fields are only meaningful when @c ok is true: a zero-valued status renders
 * as a cluster with no peers and no leader, which is indistinguishable from a
 * real loss of quorum, so a failed call must never be dressed up as one.
 *
 * @c partial_error is different: the sidecar answered, but one field inside it
 * could not be read. The rest of the response stands.
 */
struct StatusResult {
  bool ok = false;
  std::string error;

  std::string node_id;
  std::string state;
  std::string leader_id;
  std::string leader_addr;
  std::string partial_error;

  std::uint64_t term = 0;
  std::uint64_t first_log_index = 0;
  std::uint64_t last_log_index = 0;
  std::uint64_t applied_index = 0;
  std::uint64_t commit_index = 0;
  std::uint64_t last_snapshot_index = 0;

  std::vector<RaftPeer> peers;

  [[nodiscard]] static StatusResult failure(std::string reason) {
    StatusResult result;
    result.ok = false;
    result.error = std::move(reason);
    return result;
  }
};
```

Add to `class IRaftClient`:

```cpp
  /**
   * @brief This node's own view of the cluster, for the operator console.
   *
   * Read-only and leader-free: the sidecar answers for itself and does not
   * forward, so a follower's answer is its own state (including its own belief
   * about who leads) rather than the leader's.
   */
  [[nodiscard]] virtual StatusResult status() = 0;
```

Implement it on `GrpcRaftClient`, following the deadline pattern the existing
`propose`/`read` use in that file:

```cpp
  [[nodiscard]] StatusResult status() override {
    consensus::StatusRequest request;
    consensus::StatusResponse reply;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(kStatusTimeoutSeconds));

    const grpc::Status grpc_status = stub_->Status(&context, request, &reply);
    if (!grpc_status.ok()) {
      return StatusResult::failure(grpc_status.error_message().empty()
                                       ? "sidecar unreachable"
                                       : grpc_status.error_message());
    }

    StatusResult result;
    result.ok = true;
    result.node_id = reply.node_id();
    result.state = reply.state();
    result.leader_id = reply.leader_id();
    result.leader_addr = reply.leader_addr();
    result.partial_error = reply.error();
    result.term = reply.term();
    result.first_log_index = reply.first_log_index();
    result.last_log_index = reply.last_log_index();
    result.applied_index = reply.applied_index();
    result.commit_index = reply.commit_index();
    result.last_snapshot_index = reply.last_snapshot_index();
    result.peers.reserve(static_cast<size_t>(reply.peers_size()));
    for (const consensus::Peer &peer : reply.peers()) {
      result.peers.push_back(RaftPeer{peer.id(), peer.address(),
                                      peer.suffrage()});
    }
    return result;
  }
```

Add the constant beside the file's other timeouts:

```cpp
/**
 * @brief Deadline for a status query, in seconds.
 *
 * Shorter than the propose deadline on purpose: this backs an interactive page
 * that polls, so a hung sidecar must surface as an error quickly rather than
 * stacking up requests behind a five-second wait.
 */
inline constexpr int kStatusTimeoutSeconds = 2;
```

- [ ] **Step 5: Add the handler route**

In `cpp-app/src/network/http_server.hpp`, add the member and constructor
parameter to `KVHttpHandler`:

```cpp
  KVHttpHandler(IRaftClient &raft_client, const IKVStore &store,
                const auth::IAuthEngine &auth, const IStoreStats &stats)
      : raft_client_(raft_client), store_(store), auth_(auth), stats_(stats) {}
```

```cpp
  const IStoreStats &stats_;
```

Add the constants and handler:

```cpp
  /** @brief The cluster overview route. */
  static constexpr const char *kClusterStatusPath = "/cluster/status";
```

```cpp
  /**
   * @brief GET /cluster/status - this node's view of the cluster.
   *
   * Requires the READ class and applies NO key-pattern check, because it
   * addresses no key. That is a judgment call: cluster topology is not user
   * data, the management API on :6000 is separately token-gated, and requiring
   * admin would hide the overview page from exactly the users most likely to
   * open it.
   *
   * key_count / wal_bytes / auth_enabled are local to this process and cost no
   * gRPC hop; everything else comes from the sidecar.
   */
  [[nodiscard]] HttpResponse
  handle_cluster_status(const HttpRequest &request,
                        const auth::AuthContext &identity) const {
    if (request.method != "GET") {
      return HttpResponse::json_error(
          405, "method not allowed on /cluster/status: use GET");
    }
    if (!identity.has_class(auth::CommandClass::kRead)) {
      return HttpResponse::json_error(403, "permission denied");
    }

    const StatusResult status = raft_client_.status();
    if (!status.ok) {
      // 502, never 200-with-zeros: see StatusResult's comment.
      return HttpResponse::json_error(502, status.error);
    }

    std::string body = "{";
    body += "\"node_id\":\"" + json_escape(status.node_id) + "\"";
    body += ",\"state\":\"" + json_escape(status.state) + "\"";
    body += ",\"term\":" + std::to_string(status.term);
    body += ",\"leader_id\":\"" + json_escape(status.leader_id) + "\"";
    body += ",\"leader_addr\":\"" + json_escape(status.leader_addr) + "\"";
    body += ",\"peers\":[";
    for (size_t i = 0; i < status.peers.size(); ++i) {
      if (i != 0) {
        body += ",";
      }
      const RaftPeer &peer = status.peers[i];
      body += "{\"id\":\"" + json_escape(peer.id) + "\"";
      body += ",\"address\":\"" + json_escape(peer.address) + "\"";
      body += ",\"suffrage\":\"" + json_escape(peer.suffrage) + "\"}";
    }
    body += "]";
    body += ",\"first_log_index\":" + std::to_string(status.first_log_index);
    body += ",\"last_log_index\":" + std::to_string(status.last_log_index);
    body += ",\"applied_index\":" + std::to_string(status.applied_index);
    body += ",\"commit_index\":" + std::to_string(status.commit_index);
    body +=
        ",\"last_snapshot_index\":" + std::to_string(status.last_snapshot_index);
    body += ",\"key_count\":" + std::to_string(stats_.key_count());
    body += ",\"wal_bytes\":" + std::to_string(stats_.wal_size_bytes());
    body += ",\"auth_enabled\":";
    body += auth_.enabled() ? "true" : "false";
    if (!status.partial_error.empty()) {
      body += ",\"error\":\"" + json_escape(status.partial_error) + "\"";
    }
    body += "}";
    return HttpResponse::json(200, body);
  }
```

In `route_request()`, immediately before the `/kv` list check:

```cpp
    if (request.path == kClusterStatusPath) {
      return handle_cluster_status(request, identity);
    }
```

In `route_label()`, beside the other fixed paths, extend the existing equality
branch so `kClusterStatusPath` is included:

```cpp
    if (request.path == kWhoamiPath || request.path == kClusterStatusPath ||
        request.path == "/insert-val" || request.path == "/get-val" ||
        request.path == "/metrics") {
      return request.method + " " + request.path;
    }
```

- [ ] **Step 6: Pass the stats seam in `main.cpp`**

`PersistentKVStore` now implements both interfaces, so the same object is passed
twice. Find where `KVHttpHandler` is constructed in `cpp-app/src/main.cpp` and add
the fourth argument — `store` again:

```cpp
    // `store` is passed twice on purpose: PersistentKVStore implements both
    // IKVStore (storage) and IStoreStats (counters), and the handler holds each
    // behind its own interface so a test can fake them independently.
    KVHttpHandler handler(*raft_client, store, auth_engine, store);
```

- [ ] **Step 7: Run to verify it passes**

```bash
cmake --build cpp-app/build -j4 && ctest --test-dir cpp-app/build --output-on-failure
```

Expected: PASS.

- [ ] **Step 8: Format and commit**

```bash
clang-format -i cpp-app/src/raft/raft_client.hpp cpp-app/src/storage/kv_store.hpp \
  cpp-app/src/network/http_server.hpp cpp-app/src/main.cpp \
  cpp-app/tests/http_handler_test.cpp
git add cpp-app/src/raft/raft_client.hpp cpp-app/src/storage/kv_store.hpp \
  cpp-app/src/network/http_server.hpp cpp-app/src/main.cpp \
  cpp-app/tests/http_handler_test.cpp
git commit -m "feat: GET /cluster/status over the new Status RPC

Adds IRaftClient::status() and an IStoreStats seam for key_count/wal_bytes --
a separate interface rather than widening IKVStore, honouring the existing
decision to keep observability off the storage interface.

An unreachable sidecar is a 502, never 200 with zeroed fields: a dashboard
showing 'term 0, no peers, not leader' is indistinguishable from a cluster
that has lost quorum."
```

---

### Task 6: `GET /auth/users` — list users

Retires the "No user listing" limitation. Same scan primitive, pointed at the
reserved prefix, which is legitimate here because `/auth/*` is not a data route.

**Files:**
- Modify: `cpp-app/src/network/http_server.hpp`
- Test: `cpp-app/tests/http_handler_test.cpp`
- Modify: `CLAUDE.md` (remove the limitation — the same commit, per the standing rule)

**Interfaces:**
- Consumes: `IKVStore::scan_keys` (Task 2), existing `authorize_admin()`, `json_string_array()`, `auth::kUserKeyPrefix`.
- Produces: `handle_users_list`.

- [ ] **Step 1: Write the failing test**

Append to `cpp-app/tests/http_handler_test.cpp`:

```cpp
// --- GET /auth/users -------------------------------------------------------

TEST_F(AuthenticatedHandlerTest, UsersListReturnsNamesInOrder) {
  add_user("carol", "pw", {"read"}, {"*"});
  add_user("alice", "pw", {"read"}, {"*"});
  add_user("bob", "pw", {"read"}, {"*"});

  const HttpResponse response = get_as("admin", admin_password, "/auth/users");

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.content_type, kJsonContentType);
  // Names only. Usernames are charset-restricted to [A-Za-z0-9_.-] by
  // auth::username_error, so unlike data keys they are safe raw in JSON.
  EXPECT_EQ(response.body, "{\"users\":[\"alice\",\"bob\",\"carol\"]}");
}

TEST_F(AuthenticatedHandlerTest, UsersListNeverLeaksASaltOrAHash) {
  add_user("alice", "s3cret", {"read"}, {"*"});

  const HttpResponse response = get_as("admin", admin_password, "/auth/users");

  EXPECT_EQ(response.body.find("s3cret"), std::string::npos);
  EXPECT_EQ(response.body.find("salt"), std::string::npos);
  EXPECT_EQ(response.body.find("hash"), std::string::npos);
  // The route must never deserialize a UserRecord into the response at all.
  EXPECT_EQ(response.body.find("classes"), std::string::npos);
}

TEST_F(AuthenticatedHandlerTest, UsersListOmitsTheBootstrapAdmin) {
  // The configured admin is not a record, and PUT /auth/users/admin is already
  // refused, so listing it would advertise an account this API cannot manage.
  const HttpResponse response = get_as("admin", admin_password, "/auth/users");
  EXPECT_EQ(response.body, "{\"users\":[]}");
}

TEST_F(AuthenticatedHandlerTest, UsersListRequiresTheAdminClass) {
  add_user("reader", "pw", {"read"}, {"*"});
  const HttpResponse response = get_as("reader", "pw", "/auth/users");
  EXPECT_EQ(response.status_code, 403);
  EXPECT_EQ(response.body, "{\"error\":\"permission denied\"}");
}

TEST_F(AuthenticatedHandlerTest, UsersListRejectsANonGetMethod) {
  HttpRequest request;
  request.method = "DELETE";
  request.path = "/auth/users";
  request.headers["authorization"] = basic_header("admin", admin_password);
  EXPECT_EQ(handler.route_request(request).status_code, 405);
}

TEST_F(HandlerTest, UsersListIsClosedWhenAuthIsDisabled) {
  // Default-closed, exactly like the rest of /auth/*: with no admin password
  // there is no way to authenticate an administrator.
  const HttpResponse response = get("/auth/users");
  EXPECT_EQ(response.status_code, 403);
  EXPECT_NE(response.body.find("RAFTKV_ADMIN_PASSWORD"), std::string::npos);
}
```

Use whatever the fixture already calls the admin password and the
`Authorization`-header builder; if `basic_header` does not exist, add it beside
the fixture's existing credential helper.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build cpp-app/build -j4 && ctest --test-dir cpp-app/build --output-on-failure
```

Expected: the `UsersList*` tests FAIL with 404 — `handle_auth` currently answers
`404 {"error":"not found"}` for any `/auth/*` path that is neither
`/auth/whoami` nor under `/auth/users/`.

- [ ] **Step 3: Add the handler**

In `cpp-app/src/network/http_server.hpp`, add the constant beside
`kAuthUsersPrefix`:

```cpp
  /** @brief The user-collection route (no trailing slash). */
  static constexpr const char *kAuthUsersPath = "/auth/users";
```

Add the handler in the private section, near the other `/auth/*` helpers:

```cpp
  /**
   * @brief GET /auth/users - the names of every stored user.
   *
   * Names ONLY. This route must never deserialize a UserRecord into its
   * response: a record carries the salt and the password hash, and the whole
   * reason `__sys:` is unreadable through data routes is to keep those two out
   * of any response body.
   *
   * The bootstrap admin is absent because it is defined by configuration rather
   * than by a record -- PUT /auth/users/admin is already refused, so listing it
   * would advertise an account this API cannot manage.
   *
   * Scanning the reserved prefix is legitimate here in a way it is not on a data
   * route: /auth/* IS the user surface, and this reads only the key names.
   */
  [[nodiscard]] HttpResponse
  handle_users_list(const HttpRequest &request,
                    const auth::AuthContext &identity) const {
    if (request.method != "GET") {
      return HttpResponse::json_error(
          405, "method not allowed on /auth/users: use GET");
    }
    if (std::optional<HttpResponse> denied = authorize_admin(identity)) {
      return *denied;
    }

    const std::string prefix(auth::kUserKeyPrefix);
    std::vector<std::string> names;
    std::string position;

    // The store has no iteration API beyond scan_keys, so page through it. The
    // page size bounds the store lock, not the response: every user is
    // returned, because there is no cursor on this route and an admin listing
    // that silently stopped at 500 users would be a lie.
    for (;;) {
      const IKVStore::KeyPage page =
          store_.scan_keys(prefix, position, kKeyScanChunk);
      for (const std::string &key : page.keys) {
        position = next_position(key);
        names.push_back(key.substr(prefix.size()));
      }
      if (page.reached_end) {
        break;
      }
    }

    return HttpResponse::json(200, "{\"users\":" + json_string_array(names) +
                                       "}");
  }
```

- [ ] **Step 4: Route it inside `handle_auth`**

In `handle_auth`, after the `kWhoamiPath` block and **before** the
`kAuthUsersPrefix` check that currently 404s:

```cpp
    if (request.path == kAuthUsersPath) {
      return handle_users_list(request, identity);
    }
```

Placed after the `!auth_.enabled()` guard at the top of `handle_auth`, so the
route stays default-closed with no admin password configured.

In `route_label()`, add the collection path to the fixed-path branch — the
existing `kAuthUsersPrefix` branch matches `/auth/users/` with a trailing slash,
so `/auth/users` needs its own entry:

```cpp
    if (request.path == kAuthUsersPath) {
      return request.method + " /auth/users";
    }
```

Put it **before** the `kAuthUsersPrefix` branch for clarity, though the two
cannot collide.

- [ ] **Step 5: Run to verify it passes**

```bash
cmake --build cpp-app/build -j4 && ctest --test-dir cpp-app/build --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Retire the limitation in `CLAUDE.md`**

Delete this bullet from the "Known Limitations" list:

```markdown
- **No user listing**: the store is one flat map with no prefix scan, so users are addressed by name one at a time. Adding `LIST` means adding an iteration API to `IKVStore`.
```

Amend the in-memory bullet to state the index's cost. Replace:

```markdown
- **The whole dataset lives in memory**, and a snapshot holds a second copy while it is being written
```

with:

```markdown
- **The whole dataset lives in memory**, at roughly 48-64 bytes per key more than the values themselves for the ordered key index that backs `GET /kv`, and a snapshot holds a second copy while it is being written
```

- [ ] **Step 7: Format and commit**

```bash
clang-format -i cpp-app/src/network/http_server.hpp cpp-app/tests/http_handler_test.cpp
git add cpp-app/src/network/http_server.hpp cpp-app/tests/http_handler_test.cpp CLAUDE.md
git commit -m "feat: GET /auth/users lists user names

Names only -- the route never deserializes a UserRecord, so a salt or a hash
cannot reach a response body. Returns every user rather than a page: an admin
listing that silently stopped at 500 would be a lie.

Retires the 'No user listing' limitation, whose stated cause (one flat map
with no prefix scan) is what scan_keys removed."
```

---

### Task 7: `static_assets.hpp` — embedded-asset lookup and selection

Pure logic, no `HttpResponse` and no generated data, so it is fully unit-testable
before any of the build machinery exists.

**Files:**
- Create: `cpp-app/src/network/static_assets.hpp`
- Create: `cpp-app/tests/static_assets_test.cpp`
- Modify: `cpp-app/CMakeLists.txt` (register both)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct kvdb::StaticAsset { const char *path; const char *content_type; const unsigned char *bytes; std::size_t size; const unsigned char *gzip_bytes; std::size_t gzip_size; const char *etag; bool immutable_cache; };`
  - `struct kvdb::StaticAssetPick { bool not_modified; const unsigned char *bytes; std::size_t size; bool gzipped; };`
  - `[[nodiscard]] const StaticAsset *kvdb::find_static_asset(const StaticAsset *table, std::size_t count, std::string_view path);`
  - `[[nodiscard]] StaticAssetPick kvdb::pick_static_asset(const StaticAsset &asset, std::string_view accept_encoding, std::string_view if_none_match);`
  - `[[nodiscard]] bool kvdb::accepts_gzip(std::string_view accept_encoding);`

  Task 8 consumes all of these.

- [ ] **Step 1: Write the failing test**

Create `cpp-app/tests/static_assets_test.cpp`:

```cpp
/**
 * @file static_assets_test.cpp
 * @brief Tests for embedded static-asset lookup and variant selection.
 *
 * Driven with a HAND-WRITTEN table, so these tests need neither Node nor a
 * generated header. The generated table is exercised end-to-end instead
 * (tests/e2e/test_console.py).
 */

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "network/static_assets.hpp"

namespace kvdb {
namespace {

constexpr unsigned char kIndexRaw[] = {'<', 'h', '1', '>'};
constexpr unsigned char kIndexGzip[] = {0x1f, 0x8b, 0x00, 0x01, 0x02};
constexpr unsigned char kAppRaw[] = {'v', 'a', 'r'};

constexpr StaticAsset kTable[] = {
    {"/console/", "text/html; charset=utf-8", kIndexRaw, sizeof(kIndexRaw),
     kIndexGzip, sizeof(kIndexGzip), "\"idx1\"", false},
    {"/console/assets/app-abc123.js", "text/javascript; charset=utf-8", kAppRaw,
     sizeof(kAppRaw), nullptr, 0, "\"app1\"", true},
};

constexpr std::size_t kTableCount = sizeof(kTable) / sizeof(kTable[0]);

TEST(FindStaticAssetTest, MatchesAnExactPath) {
  const StaticAsset *asset =
      find_static_asset(kTable, kTableCount, "/console/");
  ASSERT_NE(asset, nullptr);
  EXPECT_STREQ(asset->etag, "\"idx1\"");
}

TEST(FindStaticAssetTest, ReturnsNullForAnUnknownPath) {
  EXPECT_EQ(find_static_asset(kTable, kTableCount, "/console/nope"), nullptr);
  // No SPA catch-all: the console uses hash routing, so an unknown path under
  // /console/ is a genuine 404 rather than "here is the app".
  EXPECT_EQ(find_static_asset(kTable, kTableCount, "/console/keys"), nullptr);
}

TEST(FindStaticAssetTest, HandlesAnEmptyTable) {
  // This is the KVDB_CONSOLE=OFF shape: the table exists and is empty, so the
  // handler needs no preprocessor branch.
  EXPECT_EQ(find_static_asset(nullptr, 0, "/console/"), nullptr);
}

TEST(AcceptsGzipTest, DetectsTheTokenAnywhereInTheHeader) {
  EXPECT_TRUE(accepts_gzip("gzip"));
  EXPECT_TRUE(accepts_gzip("gzip, deflate, br"));
  EXPECT_TRUE(accepts_gzip("deflate, gzip"));
  EXPECT_TRUE(accepts_gzip("GZIP"));
  EXPECT_TRUE(accepts_gzip("gzip;q=1.0, identity;q=0.5"));
}

TEST(AcceptsGzipTest, RejectsAnAbsentOrUnrelatedHeader) {
  EXPECT_FALSE(accepts_gzip(""));
  EXPECT_FALSE(accepts_gzip("identity"));
  EXPECT_FALSE(accepts_gzip("deflate, br"));
  // Must not match a longer token that merely contains "gzip".
  EXPECT_FALSE(accepts_gzip("notgzipreally"));
}

TEST(PickStaticAssetTest, PrefersGzipWhenBothExistAndTheClientAcceptsIt) {
  const StaticAssetPick pick = pick_static_asset(kTable[0], "gzip, br", "");
  EXPECT_FALSE(pick.not_modified);
  EXPECT_TRUE(pick.gzipped);
  EXPECT_EQ(pick.bytes, kIndexGzip);
  EXPECT_EQ(pick.size, sizeof(kIndexGzip));
}

TEST(PickStaticAssetTest, FallsBackToRawWhenTheClientDoesNotAcceptGzip) {
  // curl sends no Accept-Encoding, and keeping the raw bytes embedded is what
  // keeps `curl http://localhost:8080/console/` readable for debugging.
  const StaticAssetPick pick = pick_static_asset(kTable[0], "", "");
  EXPECT_FALSE(pick.gzipped);
  EXPECT_EQ(pick.bytes, kIndexRaw);
}

TEST(PickStaticAssetTest, FallsBackToRawWhenNoGzipVariantWasEmbedded) {
  const StaticAssetPick pick = pick_static_asset(kTable[1], "gzip", "");
  EXPECT_FALSE(pick.gzipped);
  EXPECT_EQ(pick.bytes, kAppRaw);
}

TEST(PickStaticAssetTest, ReportsNotModifiedOnAMatchingEtag) {
  const StaticAssetPick pick = pick_static_asset(kTable[0], "gzip", "\"idx1\"");
  EXPECT_TRUE(pick.not_modified);
  EXPECT_EQ(pick.size, 0u);
}

TEST(PickStaticAssetTest, MatchesAnEtagInsideAList) {
  // RFC 9110: If-None-Match may carry several validators.
  const StaticAssetPick pick =
      pick_static_asset(kTable[0], "", "\"other\", \"idx1\"");
  EXPECT_TRUE(pick.not_modified);
}

TEST(PickStaticAssetTest, HonoursTheWildcardValidator) {
  EXPECT_TRUE(pick_static_asset(kTable[0], "", "*").not_modified);
}

TEST(PickStaticAssetTest, ServesTheBodyOnAStaleEtag) {
  const StaticAssetPick pick = pick_static_asset(kTable[0], "", "\"old\"");
  EXPECT_FALSE(pick.not_modified);
  EXPECT_EQ(pick.bytes, kIndexRaw);
}

}  // namespace
}  // namespace kvdb
```

- [ ] **Step 2: Register the test and run it to verify it fails**

In `cpp-app/CMakeLists.txt`, add to `add_executable(kvdb_tests ...)`:

```cmake
        tests/static_assets_test.cpp
```

```bash
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
```

Expected: **compile failure** — `network/static_assets.hpp: No such file`.

- [ ] **Step 3: Write the header**

Create `cpp-app/src/network/static_assets.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <string_view>

namespace kvdb {

/**
 * @brief One asset compiled into the binary.
 *
 * Both a raw and a gzip variant are embedded, and the gzip one may be absent
 * (@c gzip_bytes == nullptr) for assets compression does not help. Embedding
 * both costs roughly 250 KB of .rodata on a multi-megabyte binary and buys two
 * things: no decompressor has to be linked into the engine to serve a client
 * that does not advertise gzip, and `curl .../console/` stays readable.
 *
 * Every member is a pointer or a POD, so a table of these is `constexpr` and
 * lives entirely in .rodata -- nothing is copied, allocated or initialised at
 * start-up. The console costs the database nothing until a browser asks.
 */
struct StaticAsset {
  /** @brief Request path this asset answers, matched exactly. */
  const char *path;
  const char *content_type;

  const unsigned char *bytes;
  std::size_t size;

  /** @brief Pre-compressed variant, or nullptr when none was embedded. */
  const unsigned char *gzip_bytes;
  std::size_t gzip_size;

  /** @brief Strong validator, quoted, derived from the content at build time. */
  const char *etag;

  /**
   * @brief True for content-hashed assets, which may be cached forever.
   *
   * index.html is false: its URL never changes, so it must be revalidated (and
   * answers 304 via @c etag). Hashed assets under /console/assets/ are true.
   */
  bool immutable_cache;
};

/** @brief What to send for one asset, given the request's conditional headers. */
struct StaticAssetPick {
  bool not_modified = false;
  const unsigned char *bytes = nullptr;
  std::size_t size = 0;
  bool gzipped = false;
};

/**
 * @brief Look up @p path in @p table.
 * @return The asset, or nullptr when there is no exact match.
 *
 * A linear scan, deliberately: the table holds fewer than twenty entries, so a
 * hash map would cost more in code and start-up than it saves in comparisons.
 *
 * There is NO fallback to index.html. The console uses hash routing (#/keys),
 * so every real asset has an exact path and an unknown one is a genuine 404 --
 * answering "here is the app" to every typo makes a 404 unobservable.
 *
 * Safe with @p table == nullptr and @p count == 0, which is the shape the
 * generated table takes when the console was not built in.
 */
[[nodiscard]] inline const StaticAsset *
find_static_asset(const StaticAsset *table, std::size_t count,
                  std::string_view path) {
  for (std::size_t i = 0; i < count; ++i) {
    if (path == table[i].path) {
      return &table[i];
    }
  }
  return nullptr;
}

/**
 * @brief True when @p accept_encoding lists gzip as a whole token.
 *
 * Token-aware rather than a substring search: "notgzipreally" is not an offer
 * of gzip, and serving compressed bytes to a client that cannot decompress
 * them produces garbage rather than an error.
 *
 * Case-insensitive, and q-values are ignored -- a client that writes
 * "gzip;q=0" is vanishingly rare and gets correct-but-compressed output, which
 * is a far smaller problem than parsing quality values by hand here.
 */
[[nodiscard]] inline bool accepts_gzip(std::string_view accept_encoding) {
  static constexpr std::string_view kToken = "gzip";

  std::size_t position = 0;
  while (position < accept_encoding.size()) {
    std::size_t end = accept_encoding.find(',', position);
    if (end == std::string_view::npos) {
      end = accept_encoding.size();
    }
    std::string_view token = accept_encoding.substr(position, end - position);

    // Trim spaces, then drop any ";q=..." parameters.
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
      token.remove_prefix(1);
    }
    const std::size_t semicolon = token.find(';');
    if (semicolon != std::string_view::npos) {
      token = token.substr(0, semicolon);
    }
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
      token.remove_suffix(1);
    }

    if (token.size() == kToken.size()) {
      bool equal = true;
      for (std::size_t i = 0; i < token.size(); ++i) {
        const char lowered = (token[i] >= 'A' && token[i] <= 'Z')
                                 ? static_cast<char>(token[i] - 'A' + 'a')
                                 : token[i];
        if (lowered != kToken[i]) {
          equal = false;
          break;
        }
      }
      if (equal) {
        return true;
      }
    }

    position = end + 1;
  }
  return false;
}

/**
 * @brief True when @p if_none_match satisfies @p etag.
 *
 * RFC 9110 allows a list of validators and the wildcard "*". A substring search
 * is enough for the list case because the etags here are quoted build-time
 * hashes, so one cannot appear inside another by accident.
 */
[[nodiscard]] inline bool etag_matches(std::string_view if_none_match,
                                       std::string_view etag) {
  if (if_none_match.empty()) {
    return false;
  }
  if (if_none_match == "*") {
    return true;
  }
  return if_none_match.find(etag) != std::string_view::npos;
}

/**
 * @brief Decide what to send for @p asset.
 *
 * The conditional check runs FIRST: a 304 needs no body, so there is no point
 * choosing a variant for one.
 */
[[nodiscard]] inline StaticAssetPick
pick_static_asset(const StaticAsset &asset, std::string_view accept_encoding,
                  std::string_view if_none_match) {
  StaticAssetPick pick;

  if (etag_matches(if_none_match, asset.etag)) {
    pick.not_modified = true;
    return pick;
  }

  if (asset.gzip_bytes != nullptr && accepts_gzip(accept_encoding)) {
    pick.bytes = asset.gzip_bytes;
    pick.size = asset.gzip_size;
    pick.gzipped = true;
    return pick;
  }

  pick.bytes = asset.bytes;
  pick.size = asset.size;
  return pick;
}

} // namespace kvdb
```

- [ ] **Step 4: Register the header and run to verify it passes**

In `cpp-app/CMakeLists.txt`, add to `KVDB_HEADERS`, before `http_request.hpp`:

```cmake
    src/network/static_assets.hpp
```

```bash
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
ctest --test-dir cpp-app/build --output-on-failure -R static_assets
```

Expected: PASS. Then run the whole suite:

```bash
ctest --test-dir cpp-app/build --output-on-failure
```

- [ ] **Step 5: Format and commit**

```bash
clang-format -i cpp-app/src/network/static_assets.hpp cpp-app/tests/static_assets_test.cpp
git add cpp-app/src/network/static_assets.hpp cpp-app/tests/static_assets_test.cpp \
  cpp-app/CMakeLists.txt
git commit -m "feat: embedded static-asset lookup and variant selection

Pure logic over a constexpr table -- path lookup, token-aware Accept-Encoding
negotiation, ETag revalidation -- so it is unit-testable with a hand-written
table before any build machinery exists, and needs no Node in the cpp CI job.

No SPA catch-all: the console uses hash routing, so an unknown path under
/console/ is a real 404 rather than a silently-served index.html."
```

---

### Task 8: CMake asset generation and the `/console/` routes

**Files:**
- Create: `cpp-app/cmake/GenerateConsoleAssets.cmake`
- Modify: `cpp-app/CMakeLists.txt`
- Modify: `cpp-app/src/network/http_server.hpp`
- Test: `cpp-app/tests/http_handler_test.cpp`

**Interfaces:**
- Consumes: `find_static_asset`, `pick_static_asset`, `StaticAsset` (Task 7).
- Produces:
  - Generated header `${CMAKE_BINARY_DIR}/console/console_assets_generated.hpp` defining `kvdb::kConsoleAssets` (a `constexpr StaticAsset[]`, possibly empty) and `kvdb::kConsoleAssetCount`.
  - CMake option `KVDB_CONSOLE` (default **OFF** in this task; Task 9 flips it to ON) and cache variable `KVDB_CONSOLE_DIST`.
  - Handler routes `GET /` → 302, `GET /console/...` → asset or 404.

- [ ] **Step 1: Write the failing test**

Append to `cpp-app/tests/http_handler_test.cpp`. These assert the
**empty-table** behaviour, which is what the `cpp` CI job (no Node) sees; the
populated table is covered end-to-end in Task 13.

```cpp
// --- Console static routes -------------------------------------------------
//
// The C++ test build has no Node, so the generated asset table is EMPTY here.
// That is the KVDB_CONSOLE=OFF shape and it is a real deployment mode, so it
// gets a real contract rather than being untested.

TEST_F(HandlerTest, RootRedirectsToTheConsole) {
  HttpRequest request;
  request.method = "GET";
  request.path = "/";
  const HttpResponse response = handler.route_request(request);

  EXPECT_EQ(response.status_code, 302);
  bool has_location = false;
  for (const auto &header : response.extra_headers) {
    if (header.first == "Location" && header.second == "/console/") {
      has_location = true;
    }
  }
  EXPECT_TRUE(has_location);
}

TEST_F(HandlerTest, ConsolePathIs404WhenNotBuiltIn) {
  const HttpResponse response = get("/console/");
  EXPECT_EQ(response.status_code, 404);
  // An answer, not a mystery: an operator hitting this needs to know the
  // binary was configured without the console, not that they mistyped.
  EXPECT_EQ(response.body,
            "{\"error\":\"console not built into this binary\"}");
}

TEST_F(HandlerTest, ConsoleAssetPathIs404WhenNotBuiltIn) {
  EXPECT_EQ(get("/console/assets/app-abc123.js").status_code, 404);
}

TEST_F(AuthenticatedHandlerTest, ConsoleRoutesSkipTheAuthenticationGate) {
  // Deliberate exception to authenticate-once-before-any-route: these bytes
  // are compile-time constants holding no cluster state, and a page that
  // needed a credential to LOAD could not render a login form.
  //
  // With an empty table that is observable as 404-not-401.
  const HttpResponse response = get("/console/");
  EXPECT_EQ(response.status_code, 404);
  EXPECT_NE(response.status_code, 401);

  // And the redirect must not demand a credential either.
  HttpRequest request;
  request.method = "GET";
  request.path = "/";
  EXPECT_EQ(handler.route_request(request).status_code, 302);
}

TEST_F(HandlerTest, ConsoleRejectsANonGetMethod) {
  HttpRequest request;
  request.method = "POST";
  request.path = "/console/";
  EXPECT_EQ(handler.route_request(request).status_code, 405);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build cpp-app/build -j4 && ctest --test-dir cpp-app/build --output-on-failure
```

Expected: FAIL — `/` and `/console/` both answer the generic
`404 {"error":"not found"}`, and the redirect assertions fail.

- [ ] **Step 3: Write the generator**

Create `cpp-app/cmake/GenerateConsoleAssets.cmake`:

```cmake
# Turns a Vite `dist/` tree into a constexpr StaticAsset table in a header.
#
# ALWAYS generates the header, even when the console is disabled -- an empty
# table then. That is what keeps the preprocessor out of the HTTP handler: the
# lookup simply finds nothing and answers "not built into this binary".
#
# Emits, for each asset, the raw bytes plus the pre-gzipped bytes produced by
# console/scripts/gzip-dist.mjs (`<file>.gz`, optional). The ETag is a hash of
# the raw content, so it is stable across rebuilds of identical input.

function(_kvdb_emit_byte_array OUT_VAR NAME FILE_PATH)
    file(READ "${FILE_PATH}" HEX_CONTENT HEX)
    string(LENGTH "${HEX_CONTENT}" HEX_LENGTH)
    math(EXPR BYTE_COUNT "${HEX_LENGTH} / 2")

    # Two hex chars -> "0xAB,". Wrapped every 16 bytes so the generated header
    # is diffable and does not trip compiler line-length limits.
    string(REGEX REPLACE "(..)" "0x\\1," BYTES "${HEX_CONTENT}")
    string(REGEX REPLACE "((0x..,){16})" "\\1\n    " BYTES "${BYTES}")

    set(${OUT_VAR}
        "constexpr unsigned char ${NAME}[${BYTE_COUNT}] = {\n    ${BYTES}\n};\n"
        PARENT_SCOPE)
endfunction()

function(_kvdb_content_type OUT_VAR FILE_NAME)
    # Only the types Vite actually emits. An unknown extension is
    # application/octet-stream rather than a guess: a wrong Content-Type on a
    # script is a page that silently does not run.
    if(FILE_NAME MATCHES "\\.html$")
        set(${OUT_VAR} "text/html; charset=utf-8" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.js$")
        set(${OUT_VAR} "text/javascript; charset=utf-8" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.css$")
        set(${OUT_VAR} "text/css; charset=utf-8" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.svg$")
        set(${OUT_VAR} "image/svg+xml" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.woff2$")
        set(${OUT_VAR} "font/woff2" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.json$")
        set(${OUT_VAR} "application/json" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.ico$")
        set(${OUT_VAR} "image/x-icon" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "application/octet-stream" PARENT_SCOPE)
    endif()
endfunction()

# kvdb_generate_console_assets(<output-header> <dist-dir-or-empty>)
function(kvdb_generate_console_assets OUTPUT_HEADER DIST_DIR)
    set(ARRAYS "")
    set(ENTRIES "")
    set(INDEX 0)

    if(DIST_DIR)
        if(NOT EXISTS "${DIST_DIR}/index.html")
            # FATAL, never a silent skip. A node that quietly ships without a
            # console is the same class of failure as a TLS surface that
            # quietly falls back to plaintext: the operator asked for a thing
            # and got something else.
            message(FATAL_ERROR
                "KVDB_CONSOLE=ON but no built console at ${DIST_DIR} "
                "(index.html missing). Build it with `npm ci && npm run build` "
                "in console/, point -DKVDB_CONSOLE_DIST at the output, or "
                "configure with -DKVDB_CONSOLE=OFF.")
        endif()

        file(GLOB_RECURSE ASSET_FILES RELATIVE "${DIST_DIR}"
             "${DIST_DIR}/*")
        list(SORT ASSET_FILES)

        foreach(RELATIVE_PATH ${ASSET_FILES})
            # The .gz siblings are attached to their source, not listed.
            if(RELATIVE_PATH MATCHES "\\.gz$")
                continue()
            endif()

            set(FULL_PATH "${DIST_DIR}/${RELATIVE_PATH}")
            _kvdb_emit_byte_array(RAW_ARRAY "kConsoleRaw${INDEX}" "${FULL_PATH}")
            string(APPEND ARRAYS "${RAW_ARRAY}")

            set(GZIP_POINTER "nullptr")
            set(GZIP_SIZE "0")
            if(EXISTS "${FULL_PATH}.gz")
                _kvdb_emit_byte_array(GZIP_ARRAY "kConsoleGzip${INDEX}"
                                      "${FULL_PATH}.gz")
                string(APPEND ARRAYS "${GZIP_ARRAY}")
                set(GZIP_POINTER "kConsoleGzip${INDEX}")
                set(GZIP_SIZE "sizeof(kConsoleGzip${INDEX})")
            endif()

            file(SHA256 "${FULL_PATH}" CONTENT_HASH)
            string(SUBSTRING "${CONTENT_HASH}" 0 16 SHORT_HASH)
            _kvdb_content_type(CONTENT_TYPE "${RELATIVE_PATH}")

            # index.html is reachable at "/console/" (its URL never changes, so
            # it revalidates); everything else keeps its hashed name and may be
            # cached forever.
            if(RELATIVE_PATH STREQUAL "index.html")
                set(REQUEST_PATH "/console/")
                set(IMMUTABLE "false")
            else()
                set(REQUEST_PATH "/console/${RELATIVE_PATH}")
                set(IMMUTABLE "true")
            endif()

            string(APPEND ENTRIES
                "    {\"${REQUEST_PATH}\", \"${CONTENT_TYPE}\", "
                "kConsoleRaw${INDEX}, sizeof(kConsoleRaw${INDEX}), "
                "${GZIP_POINTER}, ${GZIP_SIZE}, "
                "\"\\\"${SHORT_HASH}\\\"\", ${IMMUTABLE}},\n")

            math(EXPR INDEX "${INDEX} + 1")
        endforeach()
    endif()

    if(INDEX EQUAL 0)
        # An empty table still has to be a valid array, and a zero-length array
        # is ill-formed in C++ -- so use a null pointer and a zero count.
        set(TABLE_BODY
            "inline constexpr const StaticAsset *kConsoleAssets = nullptr;\ninline constexpr std::size_t kConsoleAssetCount = 0;\n")
    else()
        set(TABLE_BODY
            "inline constexpr StaticAsset kConsoleAssetTable[] = {\n${ENTRIES}};\ninline constexpr const StaticAsset *kConsoleAssets = kConsoleAssetTable;\ninline constexpr std::size_t kConsoleAssetCount =\n    sizeof(kConsoleAssetTable) / sizeof(kConsoleAssetTable[0]);\n")
    endif()

    file(WRITE "${OUTPUT_HEADER}"
"// GENERATED by cmake/GenerateConsoleAssets.cmake. Do not edit.
//
// ${INDEX} console asset(s) embedded. An empty table is a valid configuration
// (KVDB_CONSOLE=OFF): the handler then finds nothing and answers \"console not
// built into this binary\", which is why nothing here needs a #ifdef.
#pragma once

#include <cstddef>

#include \"network/static_assets.hpp\"

namespace kvdb {

${ARRAYS}
${TABLE_BODY}
} // namespace kvdb
")

    message(STATUS "Console assets: ${INDEX} file(s) embedded")
endfunction()
```

- [ ] **Step 4: Wire the generator into `CMakeLists.txt`**

In `cpp-app/CMakeLists.txt`, after the `KVDB_HEADERS` block and **before**
`add_executable(kvdb_node ...)`:

```cmake
# --- 5b. Console assets (embedded at build time) ---
#
# Defaults to OFF until console/ exists; Task 9 flips it to ON. The header is
# generated in BOTH configurations -- empty when off -- so the HTTP handler
# needs no preprocessor branch and the test build compiles identically.

option(KVDB_CONSOLE "Embed the built management console into kvdb_node" OFF)

set(KVDB_CONSOLE_DIST "${CMAKE_CURRENT_SOURCE_DIR}/../console/dist"
    CACHE PATH "Built console assets (Vite dist/) to embed")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateConsoleAssets.cmake)

set(KVDB_CONSOLE_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/console")
file(MAKE_DIRECTORY "${KVDB_CONSOLE_HEADER_DIR}")

if(KVDB_CONSOLE)
    kvdb_generate_console_assets(
        "${KVDB_CONSOLE_HEADER_DIR}/console_assets_generated.hpp"
        "${KVDB_CONSOLE_DIST}")
else()
    kvdb_generate_console_assets(
        "${KVDB_CONSOLE_HEADER_DIR}/console_assets_generated.hpp"
        "")
endif()
```

Add `${KVDB_CONSOLE_HEADER_DIR}` to the include directories of **both** targets:

```cmake
target_include_directories(kvdb_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROTO_BINARY_DIR}
    ${KVDB_CONSOLE_HEADER_DIR}
)
```

```cmake
    target_include_directories(kvdb_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PROTO_BINARY_DIR}
        ${KVDB_CONSOLE_HEADER_DIR}
    )
```

- [ ] **Step 5: Add the routes**

In `cpp-app/src/network/http_server.hpp`, add to the include block:

```cpp
#include "console_assets_generated.hpp"
#include "static_assets.hpp"
```

Add the constants and handler to `KVHttpHandler`'s private section:

```cpp
  /** @brief Prefix of the embedded console. */
  static constexpr const char *kConsolePathPrefix = "/console/";

  /** @brief Where "/" sends a browser. */
  static constexpr const char *kConsoleIndexPath = "/console/";

  /**
   * @brief Serve one embedded console asset.
   *
   * Cache policy: index.html revalidates (its URL never changes, so a
   * no-cache + ETag pair makes a revisit a 304 with no body), while
   * content-hashed assets are immutable for a year.
   *
   * Every header value here is a compile-time constant or a build-time hash,
   * which is what makes them safe in extra_headers -- that list is not
   * validated for CRLF, so nothing attacker-influenced may go into one.
   */
  [[nodiscard]] static HttpResponse
  handle_console(const HttpRequest &request) {
    if (request.method != "GET") {
      return HttpResponse::json_error(
          405, "method not allowed on /console/: use GET");
    }

    const StaticAsset *asset = find_static_asset(
        kConsoleAssets, kConsoleAssetCount, request.path);
    if (asset == nullptr) {
      if (kConsoleAssetCount == 0) {
        return HttpResponse::json_error(
            404, "console not built into this binary");
      }
      return HttpResponse::json_error(404, "not found");
    }

    const StaticAssetPick pick = pick_static_asset(
        *asset, request.header("accept-encoding"),
        request.header("if-none-match"));

    HttpResponse response;
    response.status_code = pick.not_modified ? 304 : 200;
    response.content_type = asset->content_type;
    if (!pick.not_modified) {
      response.body.assign(reinterpret_cast<const char *>(pick.bytes),
                           pick.size);
      if (pick.gzipped) {
        response.extra_headers.emplace_back("Content-Encoding", "gzip");
      }
    }

    response.extra_headers.emplace_back("ETag", asset->etag);
    response.extra_headers.emplace_back(
        "Cache-Control", asset->immutable_cache
                             ? "public, max-age=31536000, immutable"
                             : "no-cache");
    response.extra_headers.emplace_back("X-Content-Type-Options", "nosniff");
    response.extra_headers.emplace_back("Referrer-Policy", "no-referrer");
    response.extra_headers.emplace_back(
        "Content-Security-Policy",
        "default-src 'self'; object-src 'none'; base-uri 'none'; "
        "frame-ancestors 'none'");
    return response;
  }
```

`HttpResponse::reason_phrase` must learn 302 and 304 — add both cases beside the
existing ones:

```cpp
    case 302:
      return "Found";
    case 304:
      return "Not Modified";
```

In `route_request()`, **before** the authentication gate and immediately after
the `/metrics` block:

```cpp
    // The console's static bytes, OUTSIDE the authentication gate.
    //
    // A deliberate exception to authenticate-once-before-any-route, and the
    // only one besides /metrics. It is safe for a narrow reason: these are
    // compile-time constants containing no keys, no values and no
    // configuration. Every API call the loaded page then makes goes through the
    // gate normally -- and a page that needed a credential to LOAD could not
    // render a login form.
    if (request.path.rfind(kConsolePathPrefix, 0) == 0) {
      return handle_console(request);
    }
    if (request.method == "GET" && request.path == "/") {
      HttpResponse redirect = HttpResponse::json(302, "{\"ok\":true}");
      redirect.extra_headers.emplace_back("Location", kConsoleIndexPath);
      return redirect;
    }
```

In `route_label()`, before the fixed-path branch:

```cpp
    // ONE label for every asset. Labelling per file would create a time series
    // per asset -- the same unbounded-cardinality mistake that keeps the key
    // out of /kv/{key}.
    if (request.path.rfind(kConsolePathPrefix, 0) == 0) {
      return request.method + " /console/*";
    }
```

- [ ] **Step 6: Run to verify it passes**

```bash
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
ctest --test-dir cpp-app/build --output-on-failure
```

Expected: PASS. Also confirm the fatal-error path works:

```bash
cmake -S cpp-app -B /tmp/kvdb-console-check -DKVDB_CONSOLE=ON \
  -DKVDB_CONSOLE_DIST=/tmp/does-not-exist 2>&1 | tail -5
```

Expected: `CMake Error` naming the missing `index.html` and the three ways out.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i cpp-app/src/network/http_server.hpp cpp-app/tests/http_handler_test.cpp
git add cpp-app/cmake/GenerateConsoleAssets.cmake cpp-app/CMakeLists.txt \
  cpp-app/src/network/http_server.hpp cpp-app/tests/http_handler_test.cpp
git commit -m "feat: embed console assets at build time and serve /console/

CMake turns a Vite dist/ into a constexpr StaticAsset table in .rodata --
nothing allocated or initialised at start-up, so the console costs the
database nothing until a browser asks.

The header is generated in BOTH configurations, empty when KVDB_CONSOLE=OFF,
so the handler needs no #ifdef and the no-Node test build compiles
identically. KVDB_CONSOLE=ON with no dist/ is a fatal configure error rather
than a silent skip.

Static routes sit outside the authentication gate: compile-time constants
holding no cluster state, and a page needing a credential to load could not
render a login form."
```

---

### Task 9: `console/` scaffold, the Docker build stage, and `KVDB_CONSOLE=ON`

Produces a real (if minimal) `dist/`, so the Task 8 pipeline has something to
embed and the default can flip to ON. The three pages arrive in Tasks 10–12.

**Files:**
- Create: `console/package.json`, `console/package-lock.json` (generated), `console/tsconfig.json`, `console/vite.config.ts`, `console/index.html`, `console/.gitignore`
- Create: `console/scripts/gzip-dist.mjs`
- Create: `console/src/main.tsx`, `console/src/App.tsx`, `console/src/styles/tokens.css`, `console/src/styles/global.css`
- Modify: `cpp-app/CMakeLists.txt` (`KVDB_CONSOLE` default ON)
- Modify: `Dockerfile`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: the Task 8 generator's expectations — `dist/index.html` must exist, hashed assets under `dist/assets/`, optional `<file>.gz` siblings.
- Produces: `npm run build` in `console/` writing `console/dist/`; `App` exported from `src/App.tsx`; CSS custom properties in `tokens.css` consumed by Tasks 10–12.

- [ ] **Step 1: Create the package manifest and TypeScript config**

`console/package.json`:

```json
{
  "name": "raftkv-console",
  "private": true,
  "version": "0.0.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "tsc --noEmit && vite build && node scripts/gzip-dist.mjs",
    "typecheck": "tsc --noEmit"
  },
  "dependencies": {
    "preact": "10.26.4"
  },
  "devDependencies": {
    "@preact/preset-vite": "2.10.1",
    "typescript": "5.7.3",
    "vite": "6.0.11"
  }
}
```

Exact versions, not ranges: this is embedded into a database binary, and a
transitive bump between a local build and a CI build would change the bytes.

`console/tsconfig.json`:

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "module": "ESNext",
    "moduleResolution": "bundler",
    "jsx": "react-jsx",
    "jsxImportSource": "preact",
    "strict": true,
    "noUnusedLocals": true,
    "noUnusedParameters": true,
    "noFallthroughCasesInSwitch": true,
    "noUncheckedIndexedAccess": true,
    "isolatedModules": true,
    "skipLibCheck": true,
    "noEmit": true
  },
  "include": ["src"]
}
```

`console/.gitignore`:

```gitignore
node_modules/
dist/
```

- [ ] **Step 2: Create the Vite config and the HTML entry**

`console/vite.config.ts`:

```ts
import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';

// The console is served from /console/ by the C++ engine, so every emitted URL
// must be relative to that base -- an absolute /assets/... would 404.
export default defineConfig({
  plugins: [preact()],
  base: '/console/',
  build: {
    // One JS chunk and one CSS file. Code-splitting would buy nothing here
    // (three pages, all reachable immediately) and each extra chunk is another
    // byte array in the binary and another entry in the asset table.
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        manualChunks: undefined,
      },
    },
    // Inlining would put asset bytes into index.html, which is the one file
    // served with no-cache -- so every reload would re-download them.
    assetsInlineLimit: 0,
    target: 'es2022',
    sourcemap: false,
  },
});
```

`console/index.html`:

```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>RaftKV Console</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
```

- [ ] **Step 3: Write the gzip step**

`console/scripts/gzip-dist.mjs`:

```js
// Pre-compresses text assets next to their source, as `<file>.gz`.
//
// CMake has no portable raw-gzip primitive (file(ARCHIVE_CREATE) is tar-based),
// so this runs in the Node stage that already exists to build the app. The C++
// side embeds BOTH variants and picks by Accept-Encoding.
//
// Only compressible types are gzipped: a .woff2 or .png is already compressed,
// and embedding a second, larger copy would waste .rodata for nothing.

import { createReadStream, createWriteStream } from 'node:fs';
import { readdir, stat } from 'node:fs/promises';
import { pipeline } from 'node:stream/promises';
import { createGzip } from 'node:zlib';
import { join, extname } from 'node:path';

const DIST = new URL('../dist/', import.meta.url).pathname;
const COMPRESSIBLE = new Set(['.html', '.js', '.css', '.svg', '.json', '.map']);

async function* walk(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) {
      yield* walk(path);
    } else {
      yield path;
    }
  }
}

let compressed = 0;
for await (const path of walk(DIST)) {
  if (!COMPRESSIBLE.has(extname(path))) continue;

  const target = `${path}.gz`;
  await pipeline(
    createReadStream(path),
    // Level 9: this runs once at build time and the output ships in a binary,
    // so build seconds are worth bytes.
    createGzip({ level: 9 }),
    createWriteStream(target),
  );

  // A .gz that is not smaller is dead weight in .rodata. Keeping it would be
  // strictly worse than serving the raw bytes.
  const [raw, gz] = await Promise.all([stat(path), stat(target)]);
  if (gz.size >= raw.size) {
    const { unlink } = await import('node:fs/promises');
    await unlink(target);
    continue;
  }
  compressed += 1;
}

console.log(`gzip-dist: compressed ${compressed} file(s)`);
```

- [ ] **Step 4: Write the design tokens**

`console/src/styles/tokens.css`. The direction is deliberate: a **Swiss/technical
operator console** — flat planes separated by hairline rules rather than shadows,
strong scale contrast between labels and figures, tabular monospace numerals, one
semantic accent per raft state. No gradient blobs, no uniform card grid.

```css
:root {
  /* Neutrals. A warm-grey paper rather than pure white: this page is read for
     long stretches, and #fff against a dark terminal is fatiguing. */
  --color-surface: oklch(98.5% 0.003 90);
  --color-surface-raised: oklch(100% 0 0);
  --color-surface-sunken: oklch(96% 0.004 90);
  --color-rule: oklch(88% 0.005 90);
  --color-rule-strong: oklch(72% 0.008 90);
  --color-text: oklch(22% 0.008 90);
  --color-text-muted: oklch(50% 0.008 90);

  /* Semantic, not decorative: each maps to one raft state or outcome. */
  --color-leader: oklch(52% 0.16 150);
  --color-follower: oklch(55% 0.11 250);
  --color-candidate: oklch(68% 0.15 75);
  --color-danger: oklch(52% 0.19 25);
  --color-accent: oklch(48% 0.15 265);

  --font-sans: ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto,
    "Helvetica Neue", sans-serif;
  --font-mono: ui-monospace, "SF Mono", "Cascadia Mono", Menlo, Consolas,
    monospace;

  /* Scale contrast is the main hierarchy device here, so the steps are wide. */
  --text-micro: 0.6875rem;
  --text-label: 0.75rem;
  --text-body: 0.875rem;
  --text-figure: clamp(1.375rem, 1.1rem + 0.9vw, 1.875rem);
  --text-title: clamp(1.125rem, 1rem + 0.5vw, 1.375rem);

  /* Deliberately non-uniform: dense inside a card, generous between sections. */
  --space-hair: 0.25rem;
  --space-tight: 0.5rem;
  --space-snug: 0.75rem;
  --space-base: 1rem;
  --space-loose: 1.75rem;
  --space-section: clamp(2rem, 1.5rem + 2vw, 3.5rem);

  --radius-sm: 3px;
  --radius-md: 6px;

  --duration-fast: 120ms;
  --duration-normal: 220ms;
  --ease-out-expo: cubic-bezier(0.16, 1, 0.3, 1);
}

@media (prefers-color-scheme: dark) {
  :root {
    --color-surface: oklch(19% 0.008 265);
    --color-surface-raised: oklch(23% 0.009 265);
    --color-surface-sunken: oklch(16% 0.008 265);
    --color-rule: oklch(31% 0.01 265);
    --color-rule-strong: oklch(45% 0.012 265);
    --color-text: oklch(94% 0.004 265);
    --color-text-muted: oklch(68% 0.008 265);

    --color-leader: oklch(72% 0.15 150);
    --color-follower: oklch(74% 0.11 250);
    --color-candidate: oklch(80% 0.14 75);
    --color-danger: oklch(70% 0.17 25);
    --color-accent: oklch(76% 0.13 265);
  }
}

/* Both themes are intentional; neither is an afterthought. Reduced motion
   removes transitions entirely rather than shortening them. */
@media (prefers-reduced-motion: reduce) {
  :root {
    --duration-fast: 0ms;
    --duration-normal: 0ms;
  }
}
```

- [ ] **Step 5: Write the global stylesheet**

`console/src/styles/global.css`:

```css
@import './tokens.css';

*,
*::before,
*::after {
  box-sizing: border-box;
}

body {
  margin: 0;
  background: var(--color-surface);
  color: var(--color-text);
  font: var(--text-body) / 1.5 var(--font-sans);
  -webkit-font-smoothing: antialiased;
}

/* Every figure on this page is compared against another figure, so digits must
   not shift width between polls. */
.numeric {
  font-family: var(--font-mono);
  font-variant-numeric: tabular-nums;
}

.app-shell {
  max-width: 78rem;
  margin: 0 auto;
  padding: var(--space-loose) var(--space-base) var(--space-section);
}

.app-header {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: var(--space-base);
  padding-bottom: var(--space-snug);
  border-bottom: 1px solid var(--color-rule-strong);
}

.app-title {
  margin: 0;
  font-size: var(--text-title);
  font-weight: 650;
  letter-spacing: -0.015em;
}

.app-nav {
  display: flex;
  gap: var(--space-hair);
  margin-left: auto;
}

.app-nav a {
  padding: var(--space-hair) var(--space-tight);
  border-radius: var(--radius-sm);
  color: var(--color-text-muted);
  font-size: var(--text-label);
  font-weight: 550;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  text-decoration: none;
  transition: color var(--duration-fast) var(--ease-out-expo),
    background-color var(--duration-fast) var(--ease-out-expo);
}

.app-nav a:hover {
  color: var(--color-text);
  background: var(--color-surface-sunken);
}

.app-nav a[aria-current='page'] {
  color: var(--color-surface-raised);
  background: var(--color-text);
}

/* Focus is designed, not defaulted: a keyboard operator must always know where
   they are. */
:focus-visible {
  outline: 2px solid var(--color-accent);
  outline-offset: 2px;
}

.banner {
  margin: var(--space-base) 0 0;
  padding: var(--space-tight) var(--space-snug);
  border-left: 3px solid var(--color-candidate);
  background: var(--color-surface-sunken);
  color: var(--color-text-muted);
  font-size: var(--text-label);
}

.banner strong {
  color: var(--color-text);
}

.error-note {
  margin: var(--space-base) 0 0;
  padding: var(--space-tight) var(--space-snug);
  border-left: 3px solid var(--color-danger);
  background: var(--color-surface-sunken);
  color: var(--color-text);
  font-size: var(--text-label);
}
```

- [ ] **Step 6: Write a minimal app that renders**

`console/src/App.tsx` — replaced wholesale in Task 10; it exists here so
`vite build` produces a `dist/` for the embedding pipeline to consume.

```tsx
export function App() {
  return (
    <div class="app-shell">
      <header class="app-header">
        <h1 class="app-title">RaftKV Console</h1>
      </header>
      <p class="banner">
        <strong>Scaffold.</strong> Pages arrive in the next tasks.
      </p>
    </div>
  );
}
```

`console/src/main.tsx`:

```tsx
import { render } from 'preact';

import { App } from './App';
import './styles/global.css';

const root = document.getElementById('root');
if (root === null) {
  throw new Error('#root is missing from index.html');
}
render(<App />, root);
```

- [ ] **Step 7: Build it and verify the artefacts the generator expects**

```bash
cd console && npm install && npm run build && ls -R dist | head -20
```

Expected: `dist/index.html` exists, `dist/assets/` holds hashed `.js` and `.css`,
and `.gz` siblings sit beside the compressible ones. Commit
`console/package-lock.json` — a lockfile is what makes the embedded bytes
reproducible.

- [ ] **Step 8: Flip `KVDB_CONSOLE` to ON and verify the embed**

In `cpp-app/CMakeLists.txt`:

```cmake
option(KVDB_CONSOLE "Embed the built management console into kvdb_node" ON)
```

Update the comment above it:

```cmake
# Defaults ON: a release binary without its console is not the artefact anyone
# wants. A local build with no Node opts out explicitly with -DKVDB_CONSOLE=OFF,
# and the routes then answer "console not built into this binary" rather than
# failing mysteriously.
```

```bash
cd /Users/burhankapdawala/Documents/mine/p-repos/RaftKV
cmake -S cpp-app -B cpp-app/build -DKVDB_BUILD_TESTS=ON
cmake --build cpp-app/build -j4
```

Expected: `-- Console assets: N file(s) embedded` with N ≥ 3 in the configure
output.

Now confirm the **test** build still sees an empty table. The `cpp` CI job has no
Node, so it configures with the console off:

```bash
cmake -S cpp-app -B /tmp/kvdb-nonode -DKVDB_BUILD_TESTS=ON -DKVDB_CONSOLE=OFF
cmake --build /tmp/kvdb-nonode -j4
ctest --test-dir /tmp/kvdb-nonode --output-on-failure
```

Expected: `-- Console assets: 0 file(s) embedded`, and PASS — the Task 8
"not built into this binary" tests are what this configuration exercises.

- [ ] **Step 9: Add the Docker build stage**

In `Dockerfile`, insert **before** the `cpp_builder` stage:

```dockerfile
# --- Stage 1b: Build the management console ---
#
# Node exists ONLY here. The runtime image below gains nothing: the built assets
# are embedded into kvdb_node as .rodata by CMake in the next stage.
FROM node:22-alpine AS console_builder
WORKDIR /console

# Manifests first, so a source-only change does not re-run npm ci.
COPY console/package.json console/package-lock.json ./
# `npm ci`, not `npm install`: it installs exactly the lockfile, which is what
# makes the bytes embedded in the binary reproducible.
RUN npm ci

COPY console ./
RUN npm run build
```

In the `cpp_builder` stage, add the copy before the build and pass the option:

```dockerfile
COPY proto /app/proto
COPY cpp-app /app/cpp-app
COPY --from=console_builder /console/dist /app/console/dist

WORKDIR /app/cpp-app/build

RUN rm -rf * && cmake -DCMAKE_PREFIX_PATH=/app \
      -DKVDB_CONSOLE=ON -DKVDB_CONSOLE_DIST=/app/console/dist .. \
    && make -j4
```

`KVDB_CONSOLE_DIST` is passed explicitly because the default path is relative to
`cpp-app/`, and the image lays the tree out differently.

- [ ] **Step 10: Ignore the build output**

Append to the repo-root `.gitignore`:

```gitignore
# Console build output. node_modules and dist are also ignored by
# console/.gitignore; listed here too so a `git add -A` from the repo root
# cannot pick them up -- the same mistake that once committed bench/kvbench.
console/node_modules/
console/dist/
```

- [ ] **Step 11: Verify the whole image builds and the console is served**

```bash
docker build -t raftkv:latest .
docker compose up -d
curl -si http://localhost:8080/ | head -3
curl -s http://localhost:8080/console/ | head -3
curl -si -H 'Accept-Encoding: gzip' http://localhost:8080/console/ \
  | grep -i content-encoding
docker compose down
```

Expected: `302` with `Location: /console/`; the index HTML; and
`Content-Encoding: gzip`.

- [ ] **Step 12: Keep the no-Node CI jobs building**

Flipping the default to ON breaks every CI job that configures CMake without
Node. Four jobs in `.github/workflows/ci.yml` do: `cpp`, `cpp-sanitizers`,
`cpp-tsan` and `fuzz`. Add `-DKVDB_CONSOLE=OFF` to each of their `cmake -S ...`
invocations, with this comment above the first one:

```yaml
          # -DKVDB_CONSOLE=OFF: this job has no Node, and KVDB_CONSOLE defaults
          # to ON. Off is a real deployment mode with its own tested contract
          # ("console not built into this binary"), so this job exercises that
          # shape rather than being a special case. The e2e job builds the
          # Docker image, which is where the console is actually embedded.
```

Verify no other `cmake -S cpp-app` call site was missed:

```bash
grep -n "cmake -S cpp-app" .github/workflows/*.yml
```

Every hit must either pass `-DKVDB_CONSOLE=OFF` or run inside `docker build`.

- [ ] **Step 13: Commit**

```bash
git add console .gitignore Dockerfile cpp-app/CMakeLists.txt .github/workflows/ci.yml
git commit -m "feat: console build scaffold, Docker stage, KVDB_CONSOLE on by default

Preact + Vite + TypeScript with pinned exact versions and a committed
lockfile, because these bytes end up inside a database binary and a
transitive bump would change them.

Node lives only in a build stage; the runtime image gains nothing. A local
build without Node configures -DKVDB_CONSOLE=OFF and gets the documented
'not built into this binary' 404 -- which is also the shape the no-Node cpp
CI job tests."
```

---

### Task 10: API client, credentials, shell and the cluster page

**Files:**
- Create: `console/src/lib/api.ts`, `console/src/lib/auth.ts`, `console/src/lib/format.ts`
- Create: `console/src/hooks/useVisiblePoll.ts`, `console/src/hooks/useHashRoute.ts`
- Create: `console/src/components/LoginForm.tsx`, `console/src/components/AuthBanner.tsx`, `console/src/components/StatPair.tsx`, `console/src/components/NodeCard.tsx`
- Create: `console/src/pages/ClusterPage.tsx`
- Create: `console/src/styles/cluster.css`
- Modify: `console/src/App.tsx`

**Interfaces:**
- Consumes: `GET /cluster/status` (Task 5), CSS custom properties from `tokens.css` (Task 9).
- Produces, and Tasks 11–12 rely on these exact names:
  - `api.ts`: `type ApiError = { status: number; message: string }`, `class ApiFailure extends Error { readonly status: number }`, `getClusterStatus(): Promise<ClusterStatus>`, `listKeys(opts: { prefix: string; cursor?: string; limit?: number }): Promise<KeyPage>`, `getValue(key: string, linearizable: boolean): Promise<string | null>`, `putValue(key: string, value: string): Promise<void>`, `deleteValue(key: string): Promise<void>`, `listUsers(): Promise<string[]>`, `getUser(name: string): Promise<UserRecord>`, `putUser(name: string, body: UserUpsert): Promise<void>`, `deleteUser(name: string): Promise<void>`, `whoami(): Promise<Identity>`
  - types `ClusterStatus`, `Peer`, `KeyPage`, `UserRecord`, `UserUpsert`, `Identity`
  - `auth.ts`: `loadCredential(): string | null`, `storeCredential(user: string, password: string): void`, `clearCredential(): void`, `authHeader(): Record<string, string>`
  - `useVisiblePoll(fn, intervalMs, enabled)`, `useHashRoute(): [string, (r: string) => void]`
  - `formatBytes(n)`, `formatInt(n)`

- [ ] **Step 1: Write the credential module**

`console/src/lib/auth.ts`:

```ts
// Credential handling for the console.
//
// The credential is a Basic pair the operator just typed, held in
// sessionStorage. Stated plainly because it matters: this is WEAKER than an
// httpOnly cookie session -- a cross-site scripting bug in this app could read
// it. The mitigations are the CSP the engine sends with index.html and the
// no-innerHTML rule below. A real session-token endpoint would need a new
// committed record type and a token store, which is a larger change than the
// whole console; it is the documented upgrade path, not an oversight.
//
// sessionStorage rather than localStorage: it dies with the tab, so a shared
// machine does not keep an admin credential around after the operator leaves.

const STORAGE_KEY = 'raftkv.console.credential';

/** @returns The stored `user:password` base64, or null. */
export function loadCredential(): string | null {
  try {
    return sessionStorage.getItem(STORAGE_KEY);
  } catch {
    // Private-browsing modes and hardened settings THROW on access rather than
    // returning null. The console must still render (and simply ask again).
    return null;
  }
}

export function storeCredential(user: string, password: string): void {
  // btoa operates on latin1, so a non-ASCII password has to be encoded to
  // bytes first -- otherwise btoa throws and the login silently fails.
  const bytes = new TextEncoder().encode(`${user}:${password}`);
  let latin1 = '';
  for (const byte of bytes) latin1 += String.fromCharCode(byte);
  try {
    sessionStorage.setItem(STORAGE_KEY, btoa(latin1));
  } catch {
    // Nothing to do but continue unauthenticated; the next call 401s and the
    // login form reappears.
  }
}

export function clearCredential(): void {
  try {
    sessionStorage.removeItem(STORAGE_KEY);
  } catch {
    /* see loadCredential */
  }
}

export function authHeader(): Record<string, string> {
  const credential = loadCredential();
  return credential === null ? {} : { Authorization: `Basic ${credential}` };
}
```

- [ ] **Step 2: Write the API client**

`console/src/lib/api.ts`. This is the **only** module that calls `fetch`.

```ts
import { authHeader } from './auth';

export interface Peer {
  id: string;
  address: string;
  suffrage: string;
}

export interface ClusterStatus {
  node_id: string;
  state: string;
  term: number;
  leader_id: string;
  leader_addr: string;
  peers: Peer[];
  first_log_index: number;
  last_log_index: number;
  applied_index: number;
  commit_index: number;
  last_snapshot_index: number;
  key_count: number;
  wal_bytes: number;
  auth_enabled: boolean;
  /** A partial failure inside an otherwise usable response. */
  error?: string;
}

export interface KeyPage {
  /** Percent-encoded, because a key is arbitrary bytes. Decode to display. */
  keys: string[];
  next_cursor?: string;
}

export interface UserRecord {
  name: string;
  classes: string[];
  patterns: string[];
  enabled: boolean;
}

export interface UserUpsert {
  password?: string;
  classes: string[];
  patterns: string[];
  enabled: boolean;
}

export interface Identity {
  name: string;
  classes: string[];
  patterns: string[];
}

/**
 * A non-2xx answer, carrying the status so callers can act on the engine's
 * documented retry contract: 503 means retry, 502 means it will fail again,
 * 401 means the credential is missing, 403 means it was refused.
 */
export class ApiFailure extends Error {
  readonly status: number;

  constructor(status: number, message: string) {
    super(message);
    this.name = 'ApiFailure';
    this.status = status;
  }
}

/** The engine's error envelope is always `{"error": "..."}`. */
async function failureFrom(response: Response): Promise<ApiFailure> {
  const text = await response.text();
  try {
    const parsed = JSON.parse(text) as { error?: string };
    if (typeof parsed.error === 'string') {
      return new ApiFailure(response.status, parsed.error);
    }
  } catch {
    // A body that is not JSON is still worth surfacing verbatim.
  }
  return new ApiFailure(response.status, text || `HTTP ${response.status}`);
}

async function request(path: string, init: RequestInit = {}): Promise<Response> {
  const response = await fetch(path, {
    ...init,
    headers: { ...authHeader(), ...(init.headers ?? {}) },
    // Same-origin only. The console is served by the node it talks to, so
    // there is no cross-origin case to allow.
    credentials: 'omit',
    cache: 'no-store',
  });
  if (!response.ok) {
    throw await failureFrom(response);
  }
  return response;
}

async function requestJson<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await request(path, init);
  return (await response.json()) as T;
}

export function getClusterStatus(): Promise<ClusterStatus> {
  return requestJson<ClusterStatus>('/cluster/status');
}

export function listKeys(opts: {
  prefix: string;
  cursor?: string;
  limit?: number;
}): Promise<KeyPage> {
  const params = new URLSearchParams();
  if (opts.prefix !== '') params.set('prefix', opts.prefix);
  if (opts.cursor !== undefined && opts.cursor !== '') {
    // The cursor comes back percent-encoded and must go out unchanged;
    // URLSearchParams would double-encode the '%'. Hence the manual join below.
    params.set('limit', String(opts.limit ?? 100));
    const base = params.toString();
    return requestJson<KeyPage>(`/kv?${base}&cursor=${opts.cursor}`);
  }
  params.set('limit', String(opts.limit ?? 100));
  return requestJson<KeyPage>(`/kv?${params.toString()}`);
}

/**
 * @param key Raw (decoded) key.
 * @returns The value, or null when the key is absent (404 is not an error here).
 */
export async function getValue(
  key: string,
  linearizable: boolean,
): Promise<string | null> {
  const suffix = linearizable ? '?consistency=linearizable' : '';
  try {
    const response = await request(`/kv/${encodeURIComponent(key)}${suffix}`);
    return await response.text();
  } catch (error) {
    if (error instanceof ApiFailure && error.status === 404) {
      return null;
    }
    throw error;
  }
}

export async function putValue(key: string, value: string): Promise<void> {
  await request(`/kv/${encodeURIComponent(key)}`, {
    method: 'PUT',
    body: value,
  });
}

export async function deleteValue(key: string): Promise<void> {
  await request(`/kv/${encodeURIComponent(key)}`, { method: 'DELETE' });
}

export async function listUsers(): Promise<string[]> {
  const body = await requestJson<{ users: string[] }>('/auth/users');
  return body.users;
}

export function getUser(name: string): Promise<UserRecord> {
  return requestJson<UserRecord>(`/auth/users/${encodeURIComponent(name)}`);
}

export async function putUser(name: string, body: UserUpsert): Promise<void> {
  await request(`/auth/users/${encodeURIComponent(name)}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
}

export async function deleteUser(name: string): Promise<void> {
  await request(`/auth/users/${encodeURIComponent(name)}`, {
    method: 'DELETE',
  });
}

export function whoami(): Promise<Identity> {
  return requestJson<Identity>('/auth/whoami');
}

/**
 * Decode a percent-encoded key from `listKeys` for display.
 *
 * Keys are arbitrary bytes, so a key can hold a sequence that is not valid
 * UTF-8 and decodeURIComponent then throws. The escaped form is shown instead
 * of losing the row.
 */
export function decodeKey(encoded: string): string {
  try {
    return decodeURIComponent(encoded);
  } catch {
    return encoded;
  }
}
```

Before writing this, confirm the request body shape `PUT /auth/users/{name}`
actually accepts by reading `handle_auth`'s upsert branch in
`cpp-app/src/network/http_server.hpp`, and make `UserUpsert` match it exactly. If
that route takes msgpack or a different field set, adjust `UserUpsert` and
`putUser` — the C++ side is the contract, not this file.

- [ ] **Step 3: Write the two hooks and the formatters**

`console/src/hooks/useVisiblePoll.ts`:

```ts
import { useEffect, useRef } from 'preact/hooks';

/**
 * Run `fn` immediately, then every `intervalMs` -- but ONLY while the tab is
 * visible.
 *
 * The visibility gate is the whole point. The database is the primary tenant of
 * this container, and a console tab left open in a background window for a week
 * must not keep asking it questions. A hidden tab polls nothing and resumes on
 * the next `visibilitychange`.
 */
export function useVisiblePoll(
  fn: () => void,
  intervalMs: number,
  enabled: boolean,
): void {
  // Held in a ref so a re-created closure does not restart the timer, which
  // would make the effective interval depend on render frequency.
  const latest = useRef(fn);
  latest.current = fn;

  useEffect(() => {
    if (!enabled) return;

    let timer: number | undefined;

    const stop = () => {
      if (timer !== undefined) {
        clearInterval(timer);
        timer = undefined;
      }
    };

    const start = () => {
      if (timer !== undefined) return;
      latest.current();
      timer = window.setInterval(() => latest.current(), intervalMs);
    };

    const onVisibilityChange = () => {
      if (document.visibilityState === 'visible') start();
      else stop();
    };

    onVisibilityChange();
    document.addEventListener('visibilitychange', onVisibilityChange);
    return () => {
      document.removeEventListener('visibilitychange', onVisibilityChange);
      stop();
    };
  }, [intervalMs, enabled]);
}
```

`console/src/hooks/useHashRoute.ts`:

```ts
import { useEffect, useState } from 'preact/hooks';

/**
 * The current hash route, e.g. "cluster".
 *
 * Hash routing rather than the History API so the engine needs no catch-all
 * rewrite: `/console/` is the only HTML path that exists, and an unknown path
 * under `/console/` stays a real 404.
 */
export function useHashRoute(): [string, (route: string) => void] {
  const read = () => window.location.hash.replace(/^#\/?/, '') || 'cluster';
  const [route, setRoute] = useState(read);

  useEffect(() => {
    const onHashChange = () => setRoute(read());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  return [route, (next: string) => {
    window.location.hash = `#/${next}`;
  }];
}
```

`console/src/lib/format.ts`:

```ts
export function formatInt(value: number): string {
  return value.toLocaleString('en-US');
}

export function formatBytes(value: number): string {
  if (value < 1024) return `${value} B`;
  const units = ['KiB', 'MiB', 'GiB', 'TiB'];
  let scaled = value / 1024;
  let unit = 0;
  while (scaled >= 1024 && unit < units.length - 1) {
    scaled /= 1024;
    unit += 1;
  }
  return `${scaled.toFixed(scaled < 10 ? 1 : 0)} ${units[unit]}`;
}
```

- [ ] **Step 4: Write the shared components**

`console/src/components/AuthBanner.tsx`:

```tsx
/**
 * Shown whenever the engine reports authentication is OFF.
 *
 * Not decoration. The default compose profile has no client authentication, so
 * anyone who can reach this port can read and write every key. The console does
 * not create that exposure -- it is served from the port that already has it --
 * but it is the first thing that makes it visible, so it says so.
 */
export function AuthBanner() {
  return (
    <p class="banner" role="status">
      <strong>Unauthenticated.</strong> This node has no client authentication:
      anyone who can reach it can read and write every key. Enable ACLs with{' '}
      <code>docker-compose.auth.yml</code> and{' '}
      <code>RAFTKV_ADMIN_PASSWORD</code>.
    </p>
  );
}
```

`console/src/components/StatPair.tsx`:

```tsx
interface Props {
  label: string;
  value: string;
  /** Rendered small and muted beneath the figure. */
  note?: string;
  tone?: 'normal' | 'danger';
}

/** A label above a figure. Scale contrast is what makes the figure readable. */
export function StatPair({ label, value, note, tone = 'normal' }: Props) {
  return (
    <div class="stat-pair">
      <span class="stat-label">{label}</span>
      <span class={`stat-value numeric${tone === 'danger' ? ' is-danger' : ''}`}>
        {value}
      </span>
      {note !== undefined && <span class="stat-note">{note}</span>}
    </div>
  );
}
```

`console/src/components/LoginForm.tsx`:

```tsx
import { useState } from 'preact/hooks';

import { storeCredential } from '../lib/auth';

interface Props {
  /** Message from the rejected request, if any. */
  reason?: string;
  onAuthenticated: () => void;
}

export function LoginForm({ reason, onAuthenticated }: Props) {
  const [user, setUser] = useState('');
  const [password, setPassword] = useState('');

  const onSubmit = (event: Event) => {
    event.preventDefault();
    storeCredential(user, password);
    onAuthenticated();
  };

  return (
    <form class="login" onSubmit={onSubmit}>
      <h2 class="login-title">Sign in</h2>
      {reason !== undefined && (
        <p class="error-note" role="alert">
          {reason}
        </p>
      )}
      <label class="field">
        <span class="field-label">User</span>
        <input
          class="field-input"
          value={user}
          autocomplete="username"
          required
          onInput={(event) => setUser((event.target as HTMLInputElement).value)}
        />
      </label>
      <label class="field">
        <span class="field-label">Password</span>
        <input
          class="field-input"
          type="password"
          value={password}
          autocomplete="current-password"
          required
          onInput={(event) =>
            setPassword((event.target as HTMLInputElement).value)
          }
        />
      </label>
      <button class="button is-primary" type="submit">
        Sign in
      </button>
    </form>
  );
}
```

`console/src/components/NodeCard.tsx`:

```tsx
import type { ClusterStatus } from '../lib/api';
import { formatBytes, formatInt } from '../lib/format';
import { StatPair } from './StatPair';

interface Props {
  /** The node's base URL as the browser reaches it, for display. */
  label: string;
  status?: ClusterStatus;
  error?: string;
}

function stateClass(state: string): string {
  const lowered = state.toLowerCase();
  if (lowered === 'leader') return 'is-leader';
  if (lowered === 'candidate') return 'is-candidate';
  return 'is-follower';
}

export function NodeCard({ label, status, error }: Props) {
  if (status === undefined) {
    return (
      <article class="node-card is-unreachable">
        <header class="node-card-head">
          <h3 class="node-card-title">{label}</h3>
          <span class="state-chip is-danger">unreachable</span>
        </header>
        <p class="error-note">{error ?? 'No response.'}</p>
      </article>
    );
  }

  // Replication lag, not a raw index: the number an operator acts on is how far
  // this node's applied state trails the log it has been given.
  const lag = Math.max(0, status.last_log_index - status.applied_index);

  return (
    <article class={`node-card ${stateClass(status.state)}`}>
      <header class="node-card-head">
        <h3 class="node-card-title">{status.node_id || label}</h3>
        <span class={`state-chip ${stateClass(status.state)}`}>
          {status.state.toLowerCase()}
        </span>
      </header>

      {status.error !== undefined && status.error !== '' && (
        <p class="error-note" role="alert">
          Partial: {status.error}
        </p>
      )}

      <div class="stat-grid">
        <StatPair label="Term" value={formatInt(status.term)} />
        <StatPair
          label="Applied"
          value={formatInt(status.applied_index)}
          note={`commit ${formatInt(status.commit_index)}`}
        />
        <StatPair
          label="Lag"
          value={formatInt(lag)}
          note="log − applied"
          tone={lag > 0 ? 'danger' : 'normal'}
        />
        <StatPair label="Keys" value={formatInt(status.key_count)} />
        <StatPair label="WAL" value={formatBytes(status.wal_bytes)} />
        <StatPair
          label="Log"
          value={`${formatInt(status.first_log_index)}–${formatInt(status.last_log_index)}`}
          note={`snapshot ${formatInt(status.last_snapshot_index)}`}
        />
      </div>

      <footer class="node-card-foot">
        <span class="field-label">Leader</span>
        <span class="numeric">
          {status.leader_id === '' ? 'none (election)' : status.leader_id}
        </span>
      </footer>
    </article>
  );
}
```

- [ ] **Step 5: Write the cluster page**

`console/src/pages/ClusterPage.tsx`:

```tsx
import { useCallback, useState } from 'preact/hooks';

import { ApiFailure, getClusterStatus, type ClusterStatus } from '../lib/api';
import { useVisiblePoll } from '../hooks/useVisiblePoll';
import { NodeCard } from '../components/NodeCard';

const POLL_INTERVAL_MS = 3000;

interface Props {
  onUnauthorized: (reason: string) => void;
  onStatus: (status: ClusterStatus) => void;
}

/**
 * The overview.
 *
 * Polls only THIS node, which is deliberate: /cluster/status is answered
 * locally by whichever node served the page, and it carries the whole committed
 * peer list. Fanning out to sibling nodes would need their browser-reachable
 * URLs, which a page served from one of them cannot know (published host ports
 * are a compose detail, and behind the secure profile's proxy there is one
 * address for all three). So the peer table below is the cluster's own view of
 * its membership, and the card is this node's view of itself -- which is the
 * honest thing to show.
 */
export function ClusterPage({ onUnauthorized, onStatus }: Props) {
  const [status, setStatus] = useState<ClusterStatus | undefined>(undefined);
  const [error, setError] = useState<string | undefined>(undefined);
  const [paused, setPaused] = useState(false);

  const refresh = useCallback(() => {
    getClusterStatus()
      .then((next) => {
        setStatus(next);
        setError(undefined);
        onStatus(next);
      })
      .catch((failure: unknown) => {
        if (failure instanceof ApiFailure && failure.status === 401) {
          onUnauthorized(failure.message);
          return;
        }
        setError(
          failure instanceof Error ? failure.message : 'Request failed.',
        );
      });
  }, [onUnauthorized, onStatus]);

  useVisiblePoll(refresh, POLL_INTERVAL_MS, !paused);

  return (
    <section class="page" aria-labelledby="cluster-heading">
      <div class="page-head">
        <h2 class="page-title" id="cluster-heading">
          Cluster
        </h2>
        <div class="page-actions">
          <button class="button" type="button" onClick={refresh}>
            Refresh
          </button>
          <button
            class="button"
            type="button"
            aria-pressed={paused}
            onClick={() => setPaused((was) => !was)}
          >
            {paused ? 'Resume polling' : 'Pause polling'}
          </button>
        </div>
      </div>

      <p class="page-note">
        Polls every {POLL_INTERVAL_MS / 1000}s while this tab is visible, and
        not at all while it is hidden.
      </p>

      <div class="node-grid">
        <NodeCard label="this node" status={status} error={error} />
      </div>

      <h3 class="section-title">Members</h3>
      {status === undefined ? (
        <p class="page-note">No configuration yet.</p>
      ) : (
        <div class="table-scroll">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">ID</th>
                <th scope="col">Raft address</th>
                <th scope="col">Suffrage</th>
                <th scope="col">Role</th>
              </tr>
            </thead>
            <tbody>
              {status.peers.map((peer) => (
                <tr key={peer.id}>
                  <td class="numeric">{peer.id}</td>
                  <td class="numeric">{peer.address}</td>
                  <td>{peer.suffrage}</td>
                  <td>
                    {peer.id === status.leader_id ? (
                      <span class="state-chip is-leader">leader</span>
                    ) : (
                      <span class="state-chip is-follower">follower</span>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
```

- [ ] **Step 6: Write the cluster stylesheet**

`console/src/styles/cluster.css`:

```css
/* Layout is bento-ish rather than a uniform card grid: the node card is a wide
   plane of figures, the member table is a dense list, and they are separated by
   a rule instead of a shadow. */

.page {
  margin-top: var(--space-loose);
}

.page-head {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: var(--space-snug);
}

.page-title {
  margin: 0;
  font-size: var(--text-title);
  font-weight: 620;
  letter-spacing: -0.015em;
}

.page-actions {
  display: flex;
  gap: var(--space-tight);
  margin-left: auto;
}

.page-note {
  margin: var(--space-hair) 0 var(--space-base);
  color: var(--color-text-muted);
  font-size: var(--text-label);
}

.section-title {
  margin: var(--space-loose) 0 var(--space-tight);
  padding-bottom: var(--space-hair);
  border-bottom: 1px solid var(--color-rule);
  font-size: var(--text-label);
  font-weight: 650;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--color-text-muted);
}

.node-grid {
  display: grid;
  gap: var(--space-base);
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 22rem), 1fr));
}

.node-card {
  padding: var(--space-snug) var(--space-base) var(--space-base);
  border: 1px solid var(--color-rule);
  /* The state colour enters as a left edge, not a fill: one semantic stripe
     reads at a glance without turning the card into a status badge. */
  border-left: 3px solid var(--color-rule-strong);
  border-radius: var(--radius-md);
  background: var(--color-surface-raised);
}

.node-card.is-leader {
  border-left-color: var(--color-leader);
}
.node-card.is-follower {
  border-left-color: var(--color-follower);
}
.node-card.is-candidate {
  border-left-color: var(--color-candidate);
}
.node-card.is-unreachable {
  border-left-color: var(--color-danger);
}

.node-card-head {
  display: flex;
  align-items: baseline;
  gap: var(--space-tight);
  margin-bottom: var(--space-snug);
}

.node-card-title {
  margin: 0;
  font-family: var(--font-mono);
  font-size: var(--text-body);
  font-weight: 600;
}

.state-chip {
  padding: 0 var(--space-hair);
  border-radius: var(--radius-sm);
  background: var(--color-surface-sunken);
  color: var(--color-text-muted);
  font-size: var(--text-micro);
  font-weight: 650;
  letter-spacing: 0.06em;
  text-transform: uppercase;
}

.state-chip.is-leader {
  background: color-mix(in oklab, var(--color-leader) 16%, transparent);
  color: var(--color-leader);
}
.state-chip.is-follower {
  background: color-mix(in oklab, var(--color-follower) 16%, transparent);
  color: var(--color-follower);
}
.state-chip.is-candidate {
  background: color-mix(in oklab, var(--color-candidate) 20%, transparent);
  color: var(--color-candidate);
}
.state-chip.is-danger {
  background: color-mix(in oklab, var(--color-danger) 16%, transparent);
  color: var(--color-danger);
}

.stat-grid {
  display: grid;
  gap: var(--space-snug) var(--space-base);
  grid-template-columns: repeat(auto-fit, minmax(6.5rem, 1fr));
}

.stat-pair {
  display: flex;
  flex-direction: column;
  gap: 1px;
}

.stat-label {
  color: var(--color-text-muted);
  font-size: var(--text-micro);
  font-weight: 620;
  letter-spacing: 0.07em;
  text-transform: uppercase;
}

.stat-value {
  font-size: var(--text-figure);
  font-weight: 500;
  line-height: 1.1;
  letter-spacing: -0.02em;
}

.stat-value.is-danger {
  color: var(--color-danger);
}

.stat-note {
  color: var(--color-text-muted);
  font-size: var(--text-micro);
}

.node-card-foot {
  display: flex;
  align-items: baseline;
  gap: var(--space-tight);
  margin-top: var(--space-base);
  padding-top: var(--space-tight);
  border-top: 1px solid var(--color-rule);
}

/* Wide content scrolls inside its own container; the page body never scrolls
   horizontally. */
.table-scroll {
  overflow-x: auto;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: var(--text-body);
}

.data-table th {
  padding: var(--space-hair) var(--space-tight);
  border-bottom: 1px solid var(--color-rule-strong);
  color: var(--color-text-muted);
  font-size: var(--text-micro);
  font-weight: 650;
  letter-spacing: 0.07em;
  text-align: left;
  text-transform: uppercase;
  white-space: nowrap;
}

.data-table td {
  padding: var(--space-tight);
  border-bottom: 1px solid var(--color-rule);
  vertical-align: top;
}

.data-table tbody tr {
  transition: background-color var(--duration-fast) var(--ease-out-expo);
}

.data-table tbody tr:hover {
  background: var(--color-surface-sunken);
}

.button {
  padding: var(--space-hair) var(--space-snug);
  border: 1px solid var(--color-rule-strong);
  border-radius: var(--radius-sm);
  background: var(--color-surface-raised);
  color: var(--color-text);
  font: inherit;
  font-size: var(--text-label);
  font-weight: 560;
  cursor: pointer;
  transition: transform var(--duration-fast) var(--ease-out-expo),
    background-color var(--duration-fast) var(--ease-out-expo),
    border-color var(--duration-fast) var(--ease-out-expo);
}

.button:hover {
  background: var(--color-surface-sunken);
  border-color: var(--color-text-muted);
}

/* Compositor-friendly only: transform, never a size or margin change. */
.button:active {
  transform: translateY(1px);
}

.button.is-primary {
  border-color: var(--color-accent);
  background: var(--color-accent);
  color: var(--color-surface-raised);
}

.button.is-danger {
  border-color: var(--color-danger);
  color: var(--color-danger);
}

.button[aria-pressed='true'] {
  border-color: var(--color-text);
  background: var(--color-text);
  color: var(--color-surface-raised);
}

.login {
  max-width: 22rem;
  margin: var(--space-section) auto;
  padding: var(--space-base);
  border: 1px solid var(--color-rule);
  border-radius: var(--radius-md);
  background: var(--color-surface-raised);
}

.login-title {
  margin: 0 0 var(--space-snug);
  font-size: var(--text-title);
  font-weight: 620;
}

.field {
  display: block;
  margin-bottom: var(--space-snug);
}

.field-label {
  display: block;
  margin-bottom: 2px;
  color: var(--color-text-muted);
  font-size: var(--text-micro);
  font-weight: 620;
  letter-spacing: 0.07em;
  text-transform: uppercase;
}

.field-input,
.field-textarea {
  width: 100%;
  padding: var(--space-hair) var(--space-tight);
  border: 1px solid var(--color-rule-strong);
  border-radius: var(--radius-sm);
  background: var(--color-surface);
  color: var(--color-text);
  font: inherit;
  font-family: var(--font-mono);
  font-size: var(--text-body);
}

.field-textarea {
  min-height: 8rem;
  resize: vertical;
}
```

- [ ] **Step 7: Wire the shell**

Replace `console/src/App.tsx`:

```tsx
import { useCallback, useState } from 'preact/hooks';

import { AuthBanner } from './components/AuthBanner';
import { LoginForm } from './components/LoginForm';
import { useHashRoute } from './hooks/useHashRoute';
import { clearCredential, loadCredential } from './lib/auth';
import type { ClusterStatus } from './lib/api';
import { ClusterPage } from './pages/ClusterPage';
import './styles/cluster.css';

const ROUTES = [
  { id: 'cluster', label: 'Cluster' },
  { id: 'keys', label: 'Keys' },
  { id: 'users', label: 'Users' },
] as const;

export function App() {
  const [route, setRoute] = useHashRoute();
  const [status, setStatus] = useState<ClusterStatus | undefined>(undefined);
  const [authReason, setAuthReason] = useState<string | undefined>(undefined);
  // Bumped to force a remount after signing in or out, so every page refetches
  // with the new credential instead of showing the previous identity's data.
  const [session, setSession] = useState(0);

  const onUnauthorized = useCallback((reason: string) => {
    clearCredential();
    setAuthReason(reason);
  }, []);

  const onStatus = useCallback((next: ClusterStatus) => {
    setStatus(next);
    setAuthReason(undefined);
  }, []);

  const signOut = () => {
    clearCredential();
    setStatus(undefined);
    setAuthReason(undefined);
    setSession((n) => n + 1);
  };

  // A 401 always means "authenticate", whether or not a credential was stored.
  if (authReason !== undefined) {
    return (
      <div class="app-shell">
        <LoginForm
          reason={authReason}
          onAuthenticated={() => {
            setAuthReason(undefined);
            setSession((n) => n + 1);
          }}
        />
      </div>
    );
  }

  const authEnabled = status?.auth_enabled === true;
  const signedIn = loadCredential() !== null;

  return (
    <div class="app-shell">
      <header class="app-header">
        <h1 class="app-title">RaftKV</h1>
        <nav class="app-nav" aria-label="Console sections">
          {ROUTES.map((entry) => (
            <a
              key={entry.id}
              href={`#/${entry.id}`}
              aria-current={route === entry.id ? 'page' : undefined}
              onClick={() => setRoute(entry.id)}
            >
              {entry.label}
            </a>
          ))}
        </nav>
        {authEnabled && signedIn && (
          <button class="button" type="button" onClick={signOut}>
            Sign out
          </button>
        )}
      </header>

      {status !== undefined && !status.auth_enabled && <AuthBanner />}

      <main key={session}>
        {route === 'cluster' && (
          <ClusterPage onUnauthorized={onUnauthorized} onStatus={onStatus} />
        )}
        {route === 'keys' && (
          <p class="page-note">The key browser arrives in the next task.</p>
        )}
        {route === 'users' && (
          <p class="page-note">User management arrives in the next task.</p>
        )}
      </main>
    </div>
  );
}
```

- [ ] **Step 8: Typecheck, build and verify in a browser**

```bash
cd console && npm run build
cd .. && docker build -t raftkv:latest . && docker compose up -d
```

Open `http://localhost:8080/` — it must redirect to `/console/`, show the node
card with a live term and applied index, list all three members with `node1`
marked leader, and display the unauthenticated banner. Confirm the poll stops
when the tab is hidden (DevTools → Network, switch tabs, watch requests cease).

Then verify the authenticated path:

```bash
docker compose down
export RAFTKV_ADMIN_PASSWORD="$(openssl rand -hex 16)"
docker compose -f docker-compose.yml -f docker-compose.auth.yml up -d
echo "admin password: $RAFTKV_ADMIN_PASSWORD"
```

Reload `/console/` — the page must load (assets are outside the auth gate),
present the login form, accept `admin` + that password, and then show the node
card with no banner and a working Sign out. Finish with `docker compose down`.

- [ ] **Step 9: Commit**

```bash
git add console
git commit -m "feat: console API client, credential handling, shell and cluster page

Polling is gated on document.visibilityState: the database is this
container's primary tenant, and a tab left open in a background window must
not keep asking it questions.

The page polls only the node that served it. /cluster/status is answered
locally and carries the committed peer list, so one call gives this node's
own view plus the cluster's membership -- fanning out would need sibling URLs
a served page cannot know (published host ports are a compose detail, and the
secure profile puts one address in front of all three)."
```

---

### Task 11: The keys page

**Files:**
- Create: `console/src/components/KeyTable.tsx`, `console/src/components/KeyEditor.tsx`
- Create: `console/src/pages/KeysPage.tsx`
- Modify: `console/src/App.tsx`
- Modify: `console/src/styles/cluster.css` (key-page rules)

**Interfaces:**
- Consumes: `listKeys`, `getValue`, `putValue`, `deleteValue`, `decodeKey`, `ApiFailure` (Task 10); `.data-table`, `.button`, `.field-*` classes (Task 10).
- Produces: `KeysPage` (props `{ onUnauthorized: (reason: string) => void }`), `KeyTable`, `KeyEditor`.

- [ ] **Step 1: Write the key table**

`console/src/components/KeyTable.tsx`:

```tsx
import { decodeKey } from '../lib/api';

interface Props {
  /** Percent-encoded keys, exactly as the API returned them. */
  encodedKeys: string[];
  selected?: string;
  onSelect: (rawKey: string) => void;
}

/**
 * Keys arrive percent-encoded because a key is arbitrary bytes and a JSON
 * string is Unicode text. Decoding happens here, for display only -- the raw
 * key is what goes back to the API.
 */
export function KeyTable({ encodedKeys, selected, onSelect }: Props) {
  if (encodedKeys.length === 0) {
    return <p class="page-note">No keys under this prefix.</p>;
  }

  return (
    <div class="table-scroll">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">Key</th>
            <th scope="col" class="col-shrink">
              <span class="visually-hidden">Actions</span>
            </th>
          </tr>
        </thead>
        <tbody>
          {encodedKeys.map((encoded) => {
            const raw = decodeKey(encoded);
            return (
              <tr
                key={encoded}
                class={raw === selected ? 'is-selected' : undefined}
              >
                {/* Rendered as text, never innerHTML: a key is attacker-supplied
                    bytes and this is the CSP's backstop, not its replacement. */}
                <td class="numeric key-cell">{raw}</td>
                <td class="col-shrink">
                  <button
                    class="button"
                    type="button"
                    onClick={() => onSelect(raw)}
                  >
                    Open
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
```

- [ ] **Step 2: Write the key editor**

`console/src/components/KeyEditor.tsx`:

```tsx
import { useEffect, useState } from 'preact/hooks';

import {
  ApiFailure,
  deleteValue,
  getValue,
  putValue,
} from '../lib/api';

interface Props {
  /** Raw key. Empty string means "the single-key console with nothing loaded". */
  keyName: string;
  onKeyNameChange: (next: string) => void;
  onMutated: () => void;
  onUnauthorized: (reason: string) => void;
}

type Status =
  | { kind: 'idle' }
  | { kind: 'busy' }
  | { kind: 'note'; text: string }
  | { kind: 'error'; text: string };

/**
 * Get / put / delete one key, with the consistency toggle.
 *
 * The engine's retry contract is surfaced rather than hidden: a 503 says retry
 * (and does NOT mean the write did not happen -- writes are at-least-once under
 * failure), a 502 says it will fail again. Papering over that difference is the
 * mistake the chaos harness made.
 */
export function KeyEditor({
  keyName,
  onKeyNameChange,
  onMutated,
  onUnauthorized,
}: Props) {
  const [value, setValue] = useState('');
  const [linearizable, setLinearizable] = useState(false);
  const [status, setStatus] = useState<Status>({ kind: 'idle' });

  const handle = (failure: unknown): void => {
    if (failure instanceof ApiFailure) {
      if (failure.status === 401) {
        onUnauthorized(failure.message);
        return;
      }
      const suffix =
        failure.status === 503
          ? ' — retry; this does not mean the write was lost'
          : failure.status === 502
            ? ' — retrying the same request will fail the same way'
            : '';
      setStatus({
        kind: 'error',
        text: `${failure.status}: ${failure.message}${suffix}`,
      });
      return;
    }
    setStatus({
      kind: 'error',
      text: failure instanceof Error ? failure.message : 'Request failed.',
    });
  };

  const load = (): void => {
    if (keyName === '') return;
    setStatus({ kind: 'busy' });
    getValue(keyName, linearizable)
      .then((loaded) => {
        if (loaded === null) {
          setValue('');
          setStatus({ kind: 'note', text: 'Key not found.' });
          return;
        }
        setValue(loaded);
        setStatus({ kind: 'idle' });
      })
      .catch(handle);
  };

  // Reload whenever the selected key changes, so clicking a row in the table
  // shows that row's value rather than the previous one's.
  useEffect(() => {
    if (keyName !== '') load();
  }, [keyName]);

  const save = (): void => {
    setStatus({ kind: 'busy' });
    putValue(keyName, value)
      .then(() => {
        setStatus({ kind: 'note', text: 'Written.' });
        onMutated();
      })
      .catch(handle);
  };

  const remove = (): void => {
    setStatus({ kind: 'busy' });
    deleteValue(keyName)
      .then(() => {
        setValue('');
        // DELETE is idempotent and 200 describes the resulting state, not
        // whether anything changed -- so this deliberately does not claim the
        // key existed.
        setStatus({ kind: 'note', text: 'Deleted (or already absent).' });
        onMutated();
      })
      .catch(handle);
  };

  const busy = status.kind === 'busy';

  return (
    <section class="key-editor" aria-labelledby="key-editor-heading">
      <h3 class="section-title" id="key-editor-heading">
        Key console
      </h3>

      <label class="field">
        <span class="field-label">Key</span>
        <input
          class="field-input"
          value={keyName}
          placeholder="exact key"
          onInput={(event) =>
            onKeyNameChange((event.target as HTMLInputElement).value)
          }
        />
      </label>

      <label class="field">
        <span class="field-label">Value</span>
        <textarea
          class="field-textarea"
          value={value}
          onInput={(event) =>
            setValue((event.target as HTMLTextAreaElement).value)
          }
        />
      </label>

      <div class="page-actions is-left">
        <button
          class="button"
          type="button"
          aria-pressed={linearizable}
          onClick={() => setLinearizable((was) => !was)}
        >
          {linearizable ? 'Linearizable read' : 'Local read'}
        </button>
        <button
          class="button"
          type="button"
          disabled={busy || keyName === ''}
          onClick={load}
        >
          Load
        </button>
        <button
          class="button is-primary"
          type="button"
          disabled={busy || keyName === ''}
          onClick={save}
        >
          Write
        </button>
        <button
          class="button is-danger"
          type="button"
          disabled={busy || keyName === ''}
          onClick={remove}
        >
          Delete
        </button>
      </div>

      <p class="page-note">
        A local read is served from this node and may be stale. A linearizable
        read forwards to the leader and costs roughly 7× as much.
      </p>

      {status.kind === 'note' && (
        <p class="banner" role="status">
          {status.text}
        </p>
      )}
      {status.kind === 'error' && (
        <p class="error-note" role="alert">
          {status.text}
        </p>
      )}
    </section>
  );
}
```

- [ ] **Step 3: Write the page**

`console/src/pages/KeysPage.tsx`:

```tsx
import { useCallback, useEffect, useState } from 'preact/hooks';

import { ApiFailure, listKeys } from '../lib/api';
import { KeyEditor } from '../components/KeyEditor';
import { KeyTable } from '../components/KeyTable';

const PAGE_SIZE = 100;

interface Props {
  onUnauthorized: (reason: string) => void;
}

export function KeysPage({ onUnauthorized }: Props) {
  const [prefix, setPrefix] = useState('');
  const [encodedKeys, setEncodedKeys] = useState<string[]>([]);
  // A stack, so "Back" is exact rather than a re-scan from the beginning: the
  // API's cursor is forward-only.
  const [cursors, setCursors] = useState<string[]>([]);
  const [nextCursor, setNextCursor] = useState<string | undefined>(undefined);
  const [selected, setSelected] = useState('');
  const [error, setError] = useState<string | undefined>(undefined);

  const fetchPage = useCallback(
    (cursor: string | undefined) => {
      listKeys({ prefix, cursor, limit: PAGE_SIZE })
        .then((page) => {
          setEncodedKeys(page.keys);
          setNextCursor(page.next_cursor);
          setError(undefined);
        })
        .catch((failure: unknown) => {
          if (failure instanceof ApiFailure && failure.status === 401) {
            onUnauthorized(failure.message);
            return;
          }
          setEncodedKeys([]);
          setNextCursor(undefined);
          setError(
            failure instanceof Error ? failure.message : 'Request failed.',
          );
        });
    },
    [prefix, onUnauthorized],
  );

  // No polling here: a key listing changes when the operator changes it, and a
  // background scan of the store is exactly the cost this console must not add.
  useEffect(() => {
    setCursors([]);
    fetchPage(undefined);
  }, [fetchPage]);

  const goForward = (): void => {
    if (nextCursor === undefined) return;
    setCursors((stack) => [...stack, nextCursor]);
    fetchPage(nextCursor);
  };

  const goBack = (): void => {
    setCursors((stack) => {
      const shortened = stack.slice(0, -1);
      fetchPage(shortened[shortened.length - 1]);
      return shortened;
    });
  };

  return (
    <section class="page" aria-labelledby="keys-heading">
      <div class="page-head">
        <h2 class="page-title" id="keys-heading">
          Keys
        </h2>
      </div>

      <label class="field">
        <span class="field-label">Prefix</span>
        <input
          class="field-input"
          value={prefix}
          placeholder="all keys"
          onInput={(event) =>
            setPrefix((event.target as HTMLInputElement).value)
          }
        />
      </label>

      <p class="page-note">
        Values are fetched only when a key is opened — a page of {PAGE_SIZE}{' '}
        values could be very large.
      </p>

      {error !== undefined && (
        <p class="error-note" role="alert">
          {error}
        </p>
      )}

      <KeyTable
        encodedKeys={encodedKeys}
        selected={selected}
        onSelect={setSelected}
      />

      <div class="page-actions is-left">
        <button
          class="button"
          type="button"
          disabled={cursors.length === 0}
          onClick={goBack}
        >
          Back
        </button>
        <button
          class="button"
          type="button"
          /* Paging stops on an ABSENT cursor, never on a short page: filtering
             can shorten a page while more keys remain. */
          disabled={nextCursor === undefined}
          onClick={goForward}
        >
          Next
        </button>
        <button class="button" type="button" onClick={() => fetchPage(cursors[cursors.length - 1])}>
          Refresh
        </button>
      </div>

      <KeyEditor
        keyName={selected}
        onKeyNameChange={setSelected}
        onMutated={() => fetchPage(cursors[cursors.length - 1])}
        onUnauthorized={onUnauthorized}
      />
    </section>
  );
}
```

- [ ] **Step 4: Add the page's styles**

Append to `console/src/styles/cluster.css`:

```css
.page-actions.is-left {
  margin: var(--space-base) 0 0;
  margin-left: 0;
  flex-wrap: wrap;
}

.col-shrink {
  width: 1%;
  white-space: nowrap;
}

/* A key may be long and may contain anything; it wraps rather than widening
   the table past the viewport. */
.key-cell {
  max-width: 44rem;
  overflow-wrap: anywhere;
}

.data-table tbody tr.is-selected {
  background: color-mix(in oklab, var(--color-accent) 10%, transparent);
}

.key-editor {
  margin-top: var(--space-section);
  padding-top: var(--space-tight);
}

.button:disabled {
  opacity: 0.45;
  cursor: not-allowed;
}

.button:disabled:hover {
  background: var(--color-surface-raised);
  border-color: var(--color-rule-strong);
}

.visually-hidden {
  position: absolute;
  width: 1px;
  height: 1px;
  margin: -1px;
  padding: 0;
  overflow: hidden;
  clip-path: inset(50%);
  white-space: nowrap;
}
```

- [ ] **Step 5: Route it**

In `console/src/App.tsx`, add the import and replace the `keys` placeholder:

```tsx
import { KeysPage } from './pages/KeysPage';
```

```tsx
        {route === 'keys' && <KeysPage onUnauthorized={onUnauthorized} />}
```

- [ ] **Step 6: Build and verify in a browser**

```bash
cd console && npm run build
cd .. && docker build -t raftkv:latest . && docker compose up -d
curl -X PUT --data-binary 'hello' http://localhost:8080/kv/app:greeting
curl -X PUT --data-binary 'world' http://localhost:8080/kv/app:other
curl -X PUT --data-binary 'x' http://localhost:8080/kv/zz
```

At `http://localhost:8080/console/#/keys`: an empty prefix lists all three keys;
`app:` narrows to two; opening `app:greeting` shows `hello`; editing and writing
succeeds and the list refreshes; deleting removes it; the linearizable toggle
still returns the value. Verify a key with awkward bytes round-trips:

```bash
curl -X PUT --data-binary 'v' 'http://localhost:8080/kv/we%20ird%2Fkey'
```

It must appear in the listing as `we ird/key` and open correctly. Then
`docker compose down`.

- [ ] **Step 7: Commit**

```bash
git add console
git commit -m "feat: console keys page with prefix listing and single-key console

Values are fetched only on open -- a page of 100 values could be hundreds of
megabytes, which is the opposite of the requirement.

Paging stops on an ABSENT cursor rather than a short page, because filtering
can shorten a page while more keys remain, and the editor surfaces the
engine's 502/503 retry distinction instead of flattening it: a 503 does not
mean the write was lost."
```

---

### Task 12: The users page

**Files:**
- Create: `console/src/components/UserTable.tsx`, `console/src/components/UserForm.tsx`
- Create: `console/src/pages/UsersPage.tsx`
- Modify: `console/src/App.tsx`

**Interfaces:**
- Consumes: `listUsers`, `getUser`, `putUser`, `deleteUser`, `whoami`, `ApiFailure`, `UserRecord`, `UserUpsert` (Task 10).
- Produces: `UsersPage` (props `{ authEnabled: boolean; onUnauthorized: (reason: string) => void }`).

- [ ] **Step 1: Write the user table**

`console/src/components/UserTable.tsx`:

```tsx
interface Props {
  names: string[];
  onSelect: (name: string) => void;
  onDelete: (name: string) => void;
}

export function UserTable({ names, onSelect, onDelete }: Props) {
  if (names.length === 0) {
    return (
      <p class="page-note">
        No users yet. The bootstrap administrator comes from{' '}
        <code>RAFTKV_ADMIN_PASSWORD</code> and is not a record, so it is not
        listed here and cannot be edited.
      </p>
    );
  }

  return (
    <div class="table-scroll">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">User</th>
            <th scope="col" class="col-shrink">
              <span class="visually-hidden">Actions</span>
            </th>
          </tr>
        </thead>
        <tbody>
          {names.map((name) => (
            <tr key={name}>
              <td class="numeric">{name}</td>
              <td class="col-shrink">
                <div class="row-actions">
                  <button
                    class="button"
                    type="button"
                    onClick={() => onSelect(name)}
                  >
                    Edit
                  </button>
                  <button
                    class="button is-danger"
                    type="button"
                    onClick={() => onDelete(name)}
                  >
                    Delete
                  </button>
                </div>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
```

- [ ] **Step 2: Write the user form**

`console/src/components/UserForm.tsx`:

```tsx
import { useState } from 'preact/hooks';

import type { UserRecord, UserUpsert } from '../lib/api';

const CLASSES = ['read', 'write', 'admin'] as const;

interface Props {
  /** The record being edited, or undefined when creating. */
  existing?: UserRecord;
  onSubmit: (name: string, body: UserUpsert) => void;
  onCancel: () => void;
}

export function UserForm({ existing, onSubmit, onCancel }: Props) {
  const [name, setName] = useState(existing?.name ?? '');
  const [password, setPassword] = useState('');
  const [classes, setClasses] = useState<string[]>(existing?.classes ?? ['read']);
  const [patterns, setPatterns] = useState((existing?.patterns ?? ['*']).join('\n'));
  const [enabled, setEnabled] = useState(existing?.enabled ?? true);

  const toggleClass = (cls: string): void => {
    setClasses((held) =>
      held.includes(cls) ? held.filter((c) => c !== cls) : [...held, cls],
    );
  };

  const submit = (event: Event): void => {
    event.preventDefault();
    const body: UserUpsert = {
      classes,
      // An empty pattern list DENIES every key; blank lines must not silently
      // become one, and trailing whitespace in a glob is a pattern that never
      // matches.
      patterns: patterns
        .split('\n')
        .map((line) => line.trim())
        .filter((line) => line !== ''),
      enabled,
    };
    // Omitted rather than sent empty on an edit: an empty password would either
    // be rejected or set a blank one, and neither is "leave it alone".
    if (password !== '') body.password = password;
    onSubmit(name, body);
  };

  return (
    <form class="user-form" onSubmit={submit}>
      <h3 class="section-title">
        {existing === undefined ? 'Create user' : `Edit ${existing.name}`}
      </h3>

      <label class="field">
        <span class="field-label">Name</span>
        <input
          class="field-input"
          value={name}
          required
          readOnly={existing !== undefined}
          pattern="[A-Za-z0-9_.\-]+"
          onInput={(event) => setName((event.target as HTMLInputElement).value)}
        />
        <span class="stat-note">Letters, digits, '_', '.' and '-' only.</span>
      </label>

      <label class="field">
        <span class="field-label">
          Password{existing !== undefined && ' (blank leaves it unchanged)'}
        </span>
        <input
          class="field-input"
          type="password"
          value={password}
          autocomplete="new-password"
          required={existing === undefined}
          onInput={(event) =>
            setPassword((event.target as HTMLInputElement).value)
          }
        />
      </label>

      <fieldset class="field">
        <legend class="field-label">Command classes</legend>
        <div class="chip-row">
          {CLASSES.map((cls) => (
            <button
              key={cls}
              class="button"
              type="button"
              aria-pressed={classes.includes(cls)}
              onClick={() => toggleClass(cls)}
            >
              {cls}
            </button>
          ))}
        </div>
      </fieldset>

      <label class="field">
        <span class="field-label">Key patterns, one per line</span>
        <textarea
          class="field-textarea"
          value={patterns}
          onInput={(event) =>
            setPatterns((event.target as HTMLTextAreaElement).value)
          }
        />
        <span class="stat-note">
          Globs with '*' and '?' and no escapes, so a key containing '*' cannot
          be named exactly. An empty list denies every key.
        </span>
      </label>

      <label class="field checkbox-field">
        <input
          type="checkbox"
          checked={enabled}
          onChange={(event) =>
            setEnabled((event.target as HTMLInputElement).checked)
          }
        />
        <span>Enabled</span>
      </label>

      <div class="page-actions is-left">
        <button class="button is-primary" type="submit">
          {existing === undefined ? 'Create' : 'Save'}
        </button>
        <button class="button" type="button" onClick={onCancel}>
          Cancel
        </button>
      </div>
    </form>
  );
}
```

- [ ] **Step 3: Write the page**

`console/src/pages/UsersPage.tsx`:

```tsx
import { useCallback, useEffect, useState } from 'preact/hooks';

import {
  ApiFailure,
  deleteUser,
  getUser,
  listUsers,
  putUser,
  whoami,
  type Identity,
  type UserRecord,
  type UserUpsert,
} from '../lib/api';
import { UserForm } from '../components/UserForm';
import { UserTable } from '../components/UserTable';

interface Props {
  authEnabled: boolean;
  onUnauthorized: (reason: string) => void;
}

type Editing =
  | { mode: 'none' }
  | { mode: 'create' }
  | { mode: 'edit'; record: UserRecord };

export function UsersPage({ authEnabled, onUnauthorized }: Props) {
  const [names, setNames] = useState<string[]>([]);
  const [identity, setIdentity] = useState<Identity | undefined>(undefined);
  const [editing, setEditing] = useState<Editing>({ mode: 'none' });
  const [error, setError] = useState<string | undefined>(undefined);
  const [note, setNote] = useState<string | undefined>(undefined);

  const handle = useCallback(
    (failure: unknown) => {
      if (failure instanceof ApiFailure) {
        if (failure.status === 401) {
          onUnauthorized(failure.message);
          return;
        }
        setError(`${failure.status}: ${failure.message}`);
        return;
      }
      setError(failure instanceof Error ? failure.message : 'Request failed.');
    },
    [onUnauthorized],
  );

  const refresh = useCallback(() => {
    if (!authEnabled) return;
    listUsers()
      .then((loaded) => {
        setNames(loaded);
        setError(undefined);
      })
      .catch(handle);
    whoami().then(setIdentity).catch(handle);
  }, [authEnabled, handle]);

  useEffect(refresh, [refresh]);

  const save = (name: string, body: UserUpsert): void => {
    putUser(name, body)
      .then(() => {
        setEditing({ mode: 'none' });
        // A user record is a raft entry, so it is committed here but may take a
        // replication delay to be usable on another node.
        setNote(`Saved ${name}. Replicating to the other nodes.`);
        refresh();
      })
      .catch(handle);
  };

  const remove = (name: string): void => {
    // A native confirm(): this is destructive and irreversible, and a custom
    // modal would be more code for strictly less trust.
    if (!window.confirm(`Delete user "${name}"?`)) return;
    deleteUser(name)
      .then(() => {
        setNote(`Deleted ${name}.`);
        refresh();
      })
      .catch(handle);
  };

  const edit = (name: string): void => {
    getUser(name)
      .then((record) => setEditing({ mode: 'edit', record }))
      .catch(handle);
  };

  if (!authEnabled) {
    return (
      <section class="page" aria-labelledby="users-heading">
        <h2 class="page-title" id="users-heading">
          Users
        </h2>
        <p class="banner">
          <strong>Authentication is off.</strong> User management is closed
          without <code>RAFTKV_ADMIN_PASSWORD</code>, because with no admin
          password there is no way to authenticate an administrator. Start the
          cluster with <code>docker-compose.auth.yml</code>.
        </p>
      </section>
    );
  }

  return (
    <section class="page" aria-labelledby="users-heading">
      <div class="page-head">
        <h2 class="page-title" id="users-heading">
          Users
        </h2>
        <div class="page-actions">
          <button
            class="button is-primary"
            type="button"
            onClick={() => setEditing({ mode: 'create' })}
          >
            New user
          </button>
        </div>
      </div>

      {identity !== undefined && (
        <p class="page-note">
          Signed in as <strong>{identity.name}</strong> — classes{' '}
          {identity.classes.join(', ') || 'none'}; patterns{' '}
          {identity.patterns.join(', ') || 'none'}.
        </p>
      )}

      {note !== undefined && (
        <p class="banner" role="status">
          {note}
        </p>
      )}
      {error !== undefined && (
        <p class="error-note" role="alert">
          {error}
        </p>
      )}

      <UserTable names={names} onSelect={edit} onDelete={remove} />

      {editing.mode === 'create' && (
        <UserForm onSubmit={save} onCancel={() => setEditing({ mode: 'none' })} />
      )}
      {editing.mode === 'edit' && (
        <UserForm
          existing={editing.record}
          onSubmit={save}
          onCancel={() => setEditing({ mode: 'none' })}
        />
      )}
    </section>
  );
}
```

- [ ] **Step 4: Add the remaining styles**

Append to `console/src/styles/cluster.css`:

```css
.row-actions {
  display: flex;
  gap: var(--space-hair);
}

.chip-row {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-hair);
}

.user-form {
  max-width: 34rem;
  margin-top: var(--space-section);
}

.user-form fieldset {
  margin: 0 0 var(--space-snug);
  padding: 0;
  border: 0;
}

.checkbox-field {
  display: flex;
  align-items: center;
  gap: var(--space-tight);
}
```

- [ ] **Step 5: Route it**

In `console/src/App.tsx`:

```tsx
import { UsersPage } from './pages/UsersPage';
```

```tsx
        {route === 'users' && (
          <UsersPage
            authEnabled={authEnabled}
            onUnauthorized={onUnauthorized}
          />
        )}
```

- [ ] **Step 6: Build and verify against the auth profile**

```bash
cd console && npm run build && cd ..
docker build -t raftkv:latest .
export RAFTKV_ADMIN_PASSWORD="$(openssl rand -hex 16)"
docker compose -f docker-compose.yml -f docker-compose.auth.yml up -d
echo "admin password: $RAFTKV_ADMIN_PASSWORD"
```

At `#/users`, signed in as `admin`: create `alice` with `read` and `app:*`;
confirm she appears in the list; open a private window at
`http://localhost:8082/console/` (node3) and sign in as `alice` — the account
must work there, which is the replication path. Confirm `alice` sees `#/users`
refused (403, not a crash) and that `#/keys` with prefix `other:` is refused with
the covered-prefix message. Edit `alice` leaving the password blank and confirm
she can still sign in. Delete her. Then:

```bash
docker compose -f docker-compose.yml -f docker-compose.auth.yml down
```

Also confirm the auth-off path: `docker compose up -d`, then `#/users` shows the
"authentication is off" banner rather than an error. `docker compose down`.

- [ ] **Step 7: Commit**

```bash
git add console
git commit -m "feat: console users page for Redis-style ACLs

The page states the real constraints rather than implying more: three command
classes, globs with no escapes, no quotas, no audit log. An edit with a blank
password omits the field rather than sending an empty one, because neither
rejecting it nor setting a blank password is 'leave it alone'.

Rendered only when the engine reports auth_enabled -- with no admin password
the whole /auth surface is default-closed, so a form there would be a form
that cannot work."
```

---

### Task 13: End-to-end tests

The only layer that can prove the embedded assets, the real `Status` RPC and
replication of a listed key actually work together.

**Files:**
- Create: `tests/e2e/test_console.py`
- Modify: `tests/e2e/test_auth.py`
- Modify: `tests/e2e/test_secure_profile.py`
- Modify: `tests/e2e/contracts.py`
- Modify: `tests/e2e/README.md`

**Interfaces:**
- Consumes: every route from Tasks 3, 5, 6 and 8; existing conftest fixtures `node_urls`, `wait_until` and the `requires_auth` / `requires_secure` markers.
- Produces: constants in `contracts.py` — `CONSOLE_INDEX_PATH`, `CONSOLE_HTML_CONTENT_TYPE`, `KV_LIST_PATH`, `CLUSTER_STATUS_PATH`, `AUTH_USERS_PATH`, `KEY_LIST_LIMIT_ERROR`, `PREFIX_NOT_COVERED_ERROR`, `CONSOLE_NOT_BUILT_ERROR`.

- [ ] **Step 1: Add the contract constants**

Append to `tests/e2e/contracts.py`, keeping the file's existing docstring style:

```python
# Console surface -----------------------------------------------------------
#
# Copied from cpp-app/src/network/http_server.hpp. The handler, the README
# tables and this file change in the same commit -- that is a standing rule in
# this repository, not a nicety.

#: Where "/" redirects, and where the console's index.html is served.
CONSOLE_INDEX_PATH = "/console/"

#: index.html's Content-Type. Hashed assets carry their own types.
CONSOLE_HTML_CONTENT_TYPE = "text/html; charset=utf-8"

#: Answered when the binary was configured with -DKVDB_CONSOLE=OFF. An answer
#: rather than a mystery: the operator needs to know it was a build choice.
CONSOLE_NOT_BUILT_ERROR = "console not built into this binary"

#: Paginated key listing. NOTE the absent trailing slash -- "/kv/" is still the
#: single-key route and still answers 400 for an empty key.
KV_LIST_PATH = "/kv"

#: This node's own view of the cluster.
CLUSTER_STATUS_PATH = "/cluster/status"

#: User names, admin only.
AUTH_USERS_PATH = "/auth/users"

#: `limit` is clamped upward but rejected at 0 -- a page of nothing cannot
#: advance a cursor, so it would leave a client unable to make progress.
KEY_LIST_LIMIT_ERROR = "limit must be an integer between 1 and 500"

#: A caller whose patterns are not "*" must scan inside its own allowance. That
#: is what keeps a filtered-out key name out of the returned cursor.
PREFIX_NOT_COVERED_ERROR = "prefix must fall within your permitted key patterns"

#: Largest page GET /kv will serve.
KEY_LIST_MAX_LIMIT = 500
```

- [ ] **Step 2: Write the failing e2e module**

Create `tests/e2e/test_console.py`:

```python
"""End-to-end tests for the management console and its supporting routes.

These are the only tests that can prove three things a unit test cannot: that
the assets CMake embedded are really reachable over HTTP, that the real
``RaftNode.Status`` RPC answers, and that a key written on the leader shows up
in ``GET /kv`` on **every** node -- which is the whole propose -> replicate ->
apply -> local-index path.

Runs in the DEFAULT suite: it needs a cluster but never touches docker.
"""

from __future__ import annotations

import urllib.parse

import pytest
import requests

from contracts import (
    AUTH_USERS_PATH,
    CLUSTER_STATUS_PATH,
    CONSOLE_HTML_CONTENT_TYPE,
    CONSOLE_INDEX_PATH,
    CONSOLE_NOT_BUILT_ERROR,
    JSON_CONTENT_TYPE,
    KEY_LIST_LIMIT_ERROR,
    KV_LIST_PATH,
)

TIMEOUT = 5.0


def _console_is_embedded(base_url: str) -> bool:
    """False when this binary was built with -DKVDB_CONSOLE=OFF."""
    response = requests.get(f"{base_url}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)
    if response.status_code == 404:
        return CONSOLE_NOT_BUILT_ERROR not in response.text
    return True


# --- static assets ---------------------------------------------------------


def test_root_redirects_to_the_console(node_urls):
    for base_url in node_urls:
        response = requests.get(
            base_url + "/", timeout=TIMEOUT, allow_redirects=False
        )
        assert response.status_code == 302, base_url
        assert response.headers["Location"] == CONSOLE_INDEX_PATH


def test_console_index_is_served(node_urls):
    base_url = node_urls[0]
    if not _console_is_embedded(base_url):
        pytest.skip("binary was built with -DKVDB_CONSOLE=OFF")

    response = requests.get(
        f"{base_url}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT
    )
    assert response.status_code == 200
    assert response.headers["Content-Type"] == CONSOLE_HTML_CONTENT_TYPE
    assert "<div id=\"root\">" in response.text
    # index.html's URL never changes, so it must revalidate rather than be
    # cached -- otherwise a redeploy serves a stale app forever.
    assert response.headers["Cache-Control"] == "no-cache"
    assert response.headers["ETag"]


def test_console_index_revalidates_to_304(node_urls):
    base_url = node_urls[0]
    if not _console_is_embedded(base_url):
        pytest.skip("binary was built with -DKVDB_CONSOLE=OFF")

    first = requests.get(f"{base_url}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)
    etag = first.headers["ETag"]

    second = requests.get(
        f"{base_url}{CONSOLE_INDEX_PATH}",
        headers={"If-None-Match": etag},
        timeout=TIMEOUT,
    )
    assert second.status_code == 304
    assert second.content == b""


def test_console_hashed_assets_are_immutable(node_urls):
    base_url = node_urls[0]
    if not _console_is_embedded(base_url):
        pytest.skip("binary was built with -DKVDB_CONSOLE=OFF")

    index = requests.get(f"{base_url}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)
    # Pull one asset URL straight out of the page rather than guessing the hash.
    marker = 'src="/console/'
    start = index.text.find(marker)
    assert start != -1, "index.html references no script"
    start += len('src="')
    end = index.text.find('"', start)
    asset_path = index.text[start:end]

    response = requests.get(base_url + asset_path, timeout=TIMEOUT)
    assert response.status_code == 200
    assert "immutable" in response.headers["Cache-Control"]


def test_console_serves_gzip_when_offered(node_urls):
    base_url = node_urls[0]
    if not _console_is_embedded(base_url):
        pytest.skip("binary was built with -DKVDB_CONSOLE=OFF")

    session = requests.Session()
    response = session.get(
        f"{base_url}{CONSOLE_INDEX_PATH}",
        headers={"Accept-Encoding": "gzip"},
        timeout=TIMEOUT,
    )
    assert response.status_code == 200
    assert response.raw.headers.get("Content-Encoding") == "gzip"
    # requests decompresses transparently, so the text must still be the page.
    assert "<div id=\"root\">" in response.text


def test_console_unknown_asset_is_404_not_the_app(node_urls):
    # Hash routing means there is no catch-all: answering "here is the app" to
    # every typo makes a 404 unobservable.
    response = requests.get(
        f"{node_urls[0]}/console/definitely-not-a-real-asset.js",
        timeout=TIMEOUT,
    )
    assert response.status_code == 404


# --- /cluster/status -------------------------------------------------------


def test_cluster_status_agrees_on_the_leader(node_urls):
    payloads = []
    for base_url in node_urls:
        response = requests.get(
            f"{base_url}{CLUSTER_STATUS_PATH}", timeout=TIMEOUT
        )
        assert response.status_code == 200, base_url
        assert response.headers["Content-Type"] == JSON_CONTENT_TYPE
        payloads.append(response.json())

    # Every node answers for ITSELF, so node_id must differ...
    assert len({p["node_id"] for p in payloads}) == len(node_urls)
    # ...but a healthy cluster agrees on who leads.
    leaders = {p["leader_id"] for p in payloads}
    assert len(leaders) == 1, f"nodes disagree on the leader: {leaders}"
    assert leaders.pop() != "", "no leader elected"

    # Exactly one node reports itself Leader.
    assert sum(1 for p in payloads if p["state"] == "Leader") == 1

    for payload in payloads:
        assert len(payload["peers"]) == len(node_urls)
        assert payload["term"] >= 1
        assert payload["last_log_index"] >= payload["applied_index"]
        assert payload["key_count"] >= 0
        assert "auth_enabled" in payload
        # A partial-failure field must be absent on a healthy node.
        assert payload.get("error", "") == ""


def test_cluster_status_rejects_a_non_get(node_urls):
    response = requests.post(
        f"{node_urls[0]}{CLUSTER_STATUS_PATH}", timeout=TIMEOUT
    )
    assert response.status_code == 405


# --- GET /kv ---------------------------------------------------------------


def _leader_url(node_urls, mgmt_urls_for) -> str:
    for base_url, mgmt_url in zip(node_urls, mgmt_urls_for(node_urls)):
        status = requests.get(f"{mgmt_url}/status", timeout=TIMEOUT).json()
        if status["is_leader"]:
            return base_url
    pytest.fail("no leader found")


def test_a_written_key_is_listed_on_every_node(
    node_urls, mgmt_urls_for, wait_until
):
    """The load-bearing test: propose -> replicate -> apply -> local index.

    A unit test can prove scan_keys walks a std::set. Only this can prove the
    key reached every node's OWN index.
    """
    leader = _leader_url(node_urls, mgmt_urls_for)
    prefix = "console-e2e:"
    key = prefix + "listed"

    written = requests.put(
        f"{leader}/kv/{urllib.parse.quote(key, safe='')}",
        data=b"value",
        timeout=TIMEOUT,
    )
    assert written.status_code == 200

    encoded_prefix = urllib.parse.quote(prefix, safe="")
    encoded_key = urllib.parse.quote(key, safe="")

    for base_url in node_urls:
        # Polled against a deadline, never slept on: a follower applies the
        # entry asynchronously.
        wait_until(
            lambda url=base_url: encoded_key
            in requests.get(
                f"{url}{KV_LIST_PATH}?prefix={encoded_prefix}", timeout=TIMEOUT
            ).json()["keys"],
            timeout=10.0,
        )

    # Clean up so a re-run starts from the same state.
    requests.delete(f"{leader}/kv/{encoded_key}", timeout=TIMEOUT)


def test_kv_list_paginates_to_completion(node_urls, mgmt_urls_for, wait_until):
    leader = _leader_url(node_urls, mgmt_urls_for)
    prefix = "console-page:"
    keys = [f"{prefix}{index:02d}" for index in range(5)]

    for key in keys:
        assert (
            requests.put(
                f"{leader}/kv/{urllib.parse.quote(key, safe='')}",
                data=b"v",
                timeout=TIMEOUT,
            ).status_code
            == 200
        )

    encoded_prefix = urllib.parse.quote(prefix, safe="")
    wait_until(
        lambda: len(
            requests.get(
                f"{leader}{KV_LIST_PATH}?prefix={encoded_prefix}",
                timeout=TIMEOUT,
            ).json()["keys"]
        )
        == len(keys),
        timeout=10.0,
    )

    # Walk it two at a time, stopping on an ABSENT cursor rather than a short
    # page -- the documented rule, and the only correct one.
    collected: list[str] = []
    url = f"{leader}{KV_LIST_PATH}?prefix={encoded_prefix}&limit=2"
    for _ in range(10):
        page = requests.get(url, timeout=TIMEOUT).json()
        collected.extend(page["keys"])
        cursor = page.get("next_cursor")
        if cursor is None:
            break
        url = (
            f"{leader}{KV_LIST_PATH}?prefix={encoded_prefix}"
            f"&limit=2&cursor={cursor}"
        )
    else:
        pytest.fail("pagination did not terminate")

    assert collected == [urllib.parse.quote(key, safe="") for key in keys]

    for key in keys:
        requests.delete(
            f"{leader}/kv/{urllib.parse.quote(key, safe='')}", timeout=TIMEOUT
        )


def test_kv_list_round_trips_awkward_bytes(
    node_urls, mgmt_urls_for, wait_until
):
    """A key is arbitrary bytes; the listing must survive that."""
    leader = _leader_url(node_urls, mgmt_urls_for)
    prefix = "console-bytes:"
    # A space, a slash, a '=' and a byte that is not valid UTF-8.
    key = prefix + "a b/c=d\x80"
    encoded_key = urllib.parse.quote(key, safe="")
    encoded_prefix = urllib.parse.quote(prefix, safe="")

    assert (
        requests.put(
            f"{leader}/kv/{encoded_key}", data=b"v", timeout=TIMEOUT
        ).status_code
        == 200
    )

    wait_until(
        lambda: encoded_key.upper()
        in [
            listed.upper()
            for listed in requests.get(
                f"{leader}{KV_LIST_PATH}?prefix={encoded_prefix}",
                timeout=TIMEOUT,
            ).json()["keys"]
        ],
        timeout=10.0,
    )
    requests.delete(f"{leader}/kv/{encoded_key}", timeout=TIMEOUT)


def test_kv_list_never_reveals_reserved_keys(node_urls):
    # Holds whether or not auth is on: the reserved space is not addressable
    # through a data route in either direction.
    response = requests.get(
        f"{node_urls[0]}{KV_LIST_PATH}?limit=500", timeout=TIMEOUT
    )
    assert response.status_code == 200
    assert "__sys" not in response.text


def test_kv_list_refuses_a_reserved_prefix(node_urls):
    response = requests.get(
        f"{node_urls[0]}{KV_LIST_PATH}?prefix="
        + urllib.parse.quote("__sys:", safe=""),
        timeout=TIMEOUT,
    )
    assert response.status_code == 403


def test_kv_list_rejects_a_bad_limit(node_urls):
    for value in ("0", "-1", "abc"):
        response = requests.get(
            f"{node_urls[0]}{KV_LIST_PATH}?limit={value}", timeout=TIMEOUT
        )
        assert response.status_code == 400, value
        assert response.json()["error"] == KEY_LIST_LIMIT_ERROR


def test_kv_list_does_not_shadow_the_single_key_route(node_urls):
    # "/kv/" keeps its empty-key 400; "/kv" is the listing.
    assert requests.get(f"{node_urls[0]}/kv/", timeout=TIMEOUT).status_code == 400


def test_auth_users_is_closed_without_a_password(node_urls):
    # Default profile: user management is default-closed, so this is 403 rather
    # than an empty list. The auth-ON behaviour lives in test_auth.py.
    response = requests.get(
        f"{node_urls[0]}{AUTH_USERS_PATH}", timeout=TIMEOUT
    )
    assert response.status_code == 403
```

Match the fixture names to `tests/e2e/conftest.py`: it exposes `node_urls` and
`mgmt_urls_for` and a `wait_until` helper. If `mgmt_urls_for` and `wait_until`
are module-level functions rather than fixtures, import them from `conftest`'s
module the way the existing tests do and drop them from the signatures.

- [ ] **Step 3: Run to verify it fails**

```bash
docker build -t raftkv:latest . && docker compose up -d
# Wait for readiness the way the CI job does, then:
pytest tests/e2e/test_console.py -v
```

Expected: FAIL — before Tasks 3/5/6/8 land these 404; run this task *after* them
and it should pass. If it passes on the first run, confirm you are testing a
freshly built image rather than a stale `raftkv:latest`.

- [ ] **Step 4: Add the auth-profile cases**

Append to `tests/e2e/test_auth.py`, using that module's existing marker and
credential fixtures:

```python
def test_console_assets_need_no_credential(node_urls):
    """The static bytes are outside the authentication gate, by design.

    A page that required a credential to LOAD could not render a login form.
    The bytes are compile-time constants holding no cluster state.
    """
    response = requests.get(f"{node_urls[0]}/console/", timeout=TIMEOUT)
    assert response.status_code in (200, 404)  # 404 iff built with the console off
    assert response.status_code != 401

    root = requests.get(
        node_urls[0] + "/", timeout=TIMEOUT, allow_redirects=False
    )
    assert root.status_code == 302


def test_console_api_routes_require_a_credential(node_urls):
    for path in ("/cluster/status", "/kv?limit=10"):
        response = requests.get(node_urls[0] + path, timeout=TIMEOUT)
        assert response.status_code == 401, path
        assert "Basic" in response.headers.get("WWW-Authenticate", "")


def test_user_list_requires_admin_and_leaks_no_hash(node_urls, admin_auth):
    created = requests.put(
        f"{node_urls[0]}/auth/users/listed",
        json={
            "password": "listed-secret",
            "classes": ["read"],
            "patterns": ["app:*"],
            "enabled": True,
        },
        auth=admin_auth,
        timeout=TIMEOUT,
    )
    assert created.status_code in (200, 201)

    listing = requests.get(
        f"{node_urls[0]}/auth/users", auth=admin_auth, timeout=TIMEOUT
    )
    assert listing.status_code == 200
    assert "listed" in listing.json()["users"]
    # Names only. A record carries a salt and a password hash, and the entire
    # point of __sys: being unreadable is to keep those out of any body.
    assert "listed-secret" not in listing.text
    assert "salt" not in listing.text
    assert "hash" not in listing.text

    # A read-class user may not enumerate accounts.
    refused = requests.get(
        f"{node_urls[0]}/auth/users",
        auth=("listed", "listed-secret"),
        timeout=TIMEOUT,
    )
    assert refused.status_code == 403

    requests.delete(
        f"{node_urls[0]}/auth/users/listed", auth=admin_auth, timeout=TIMEOUT
    )


def test_key_listing_respects_key_patterns(node_urls, admin_auth, wait_until):
    """Read access to a key's NAME is read access.

    Without pattern filtering on the listing, enumeration would route around
    the check GET /kv/{key} enforces.
    """
    requests.put(
        f"{node_urls[0]}/kv/app:visible",
        data=b"v",
        auth=admin_auth,
        timeout=TIMEOUT,
    )
    requests.put(
        f"{node_urls[0]}/kv/secret:hidden",
        data=b"v",
        auth=admin_auth,
        timeout=TIMEOUT,
    )
    requests.put(
        f"{node_urls[0]}/auth/users/scoped",
        json={
            "password": "scoped-pw",
            "classes": ["read"],
            "patterns": ["app:*"],
            "enabled": True,
        },
        auth=admin_auth,
        timeout=TIMEOUT,
    )

    scoped = ("scoped", "scoped-pw")
    wait_until(
        lambda: requests.get(
            f"{node_urls[0]}/auth/whoami", auth=scoped, timeout=TIMEOUT
        ).status_code
        == 200,
        timeout=10.0,
    )

    allowed = requests.get(
        f"{node_urls[0]}/kv?prefix=app%3A", auth=scoped, timeout=TIMEOUT
    )
    assert allowed.status_code == 200
    assert allowed.json()["keys"] == ["app%3Avisible"]

    # Outside the allowance: refused rather than filtered, which is what keeps
    # a key name the caller cannot read out of the returned cursor.
    for query in ("?prefix=secret%3A", ""):
        refused = requests.get(
            f"{node_urls[0]}/kv{query}", auth=scoped, timeout=TIMEOUT
        )
        assert refused.status_code == 403, query
        assert refused.json()["error"] == PREFIX_NOT_COVERED_ERROR

    requests.delete(
        f"{node_urls[0]}/auth/users/scoped", auth=admin_auth, timeout=TIMEOUT
    )
    requests.delete(
        f"{node_urls[0]}/kv/app:visible", auth=admin_auth, timeout=TIMEOUT
    )
    requests.delete(
        f"{node_urls[0]}/kv/secret:hidden", auth=admin_auth, timeout=TIMEOUT
    )
```

Add `PREFIX_NOT_COVERED_ERROR` to that module's `contracts` import. If the module
has no `admin_auth` fixture, use whatever it calls the admin credential tuple.

- [ ] **Step 5: Add the secure-profile case**

Append to `tests/e2e/test_secure_profile.py`:

```python
def test_console_loads_through_the_proxy(secure_client_url, ca_bundle):
    """The proxy round-robins all three nodes, so all three serve the assets.

    Worth its own case: each node holds its own copy of the embedded bytes, and
    a build that embedded them into only one would pass every single-node test.
    """
    for _ in range(6):
        response = requests.get(
            f"{secure_client_url}/console/", verify=ca_bundle, timeout=10.0
        )
        assert response.status_code == 200
        assert "<div id=\"root\">" in response.text
```

Use that module's existing fixtures for the HTTPS base URL and the CA bundle
path; the names above are placeholders for whatever it already calls them.

- [ ] **Step 6: Run all three suites**

```bash
pytest tests/e2e -v
docker compose down

export RAFTKV_ADMIN_PASSWORD="$(openssl rand -hex 16)"
docker compose -f docker-compose.yml -f docker-compose.auth.yml up -d
pytest tests/e2e -m requires_auth -v -rs
docker compose -f docker-compose.yml -f docker-compose.auth.yml down

./scripts/gen-certs.sh
export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d
pytest tests/e2e -m requires_secure -v -rs
docker compose -f docker-compose.yml -f docker-compose.secure.yml down
```

Expected: PASS everywhere, and **no silent skips** — `-rs` prints skip reasons,
and a skip here is an unverified requirement.

- [ ] **Step 7: Document the new tests**

In `tests/e2e/README.md`, add `test_console.py` to the module list with one line
on what only it can prove: that the embedded assets are reachable, that
`RaftNode.Status` answers, and that a key written on the leader appears in every
node's own index.

- [ ] **Step 8: Commit**

```bash
git add tests/e2e
git commit -m "test: end-to-end coverage for the console and its routes

The load-bearing case is not a status code: it is that a key written on the
leader appears in GET /kv on ALL THREE nodes, polled against a deadline. That
is the only check covering propose -> replicate -> apply -> the follower's own
index reading it back.

Also pins what a status code alone would miss: assets load with no credential
while every API route demands one, a user listing carries no salt or hash, and
a pattern-scoped caller is REFUSED an uncovered prefix rather than served a
filtered page."
```

---

### Task 14: Documentation, contracts and CI

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `docs/architecture.md`
- Modify: `CHANGELOG.md`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: every route and behaviour from Tasks 3–13.
- Produces: no code.

- [ ] **Step 1: Add a `console` CI job**

In `.github/workflows/ci.yml`, add a job alongside the existing ones:

```yaml
  console:
    name: console
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with:
          node-version: '22'
          cache: npm
          cache-dependency-path: console/package-lock.json
      # `npm ci`, not `npm install`: it installs exactly the lockfile, which is
      # what makes the bytes embedded in kvdb_node reproducible.
      - run: npm ci
        working-directory: console
      - run: npm run typecheck
        working-directory: console
      - run: npm run build
        working-directory: console
      # The C++ generator refuses to embed a dist/ with no index.html, so
      # asserting it exists here turns a confusing cmake failure into an
      # obvious one.
      - name: Verify the build produced embeddable assets
        run: test -f console/dist/index.html
```

- [ ] **Step 2: Document the routes in the README**

In `README.md`'s API Reference, add three sections after "Delete a key" and
before "Percent-decoding".

````markdown
### List keys

```http
GET /kv?prefix=&cursor=&limit=
```

| Outcome | Status | Body |
|---|---|---|
| Success | `200 OK` | `{"keys":["app%3Aa"],"next_cursor":"app%3Aa%00"}` |
| `limit` not an integer in `1..500` | `400 Bad Request` | `{"error":"limit must be an integer between 1 and 500"}` |
| Malformed percent-encoding in `prefix` or `cursor` | `400 Bad Request` | `{"error":"malformed percent-encoding in the query string"}` |
| `prefix` under the reserved `__sys:` space | `403 Forbidden` | `{"error":"keys under \"__sys:\" are reserved; use /auth/users/{name}"}` |
| `prefix` outside the caller's key patterns | `403 Forbidden` | `{"error":"prefix must fall within your permitted key patterns"}` |

**Keys in the response are percent-encoded.** A key is arbitrary bytes and a
JSON string is Unicode text, so a key holding a raw `0x80` would produce a body
no parser accepts. The encoded form pastes straight back into `/kv/{key}`, which
percent-decodes.

**Page until `next_cursor` is absent — never until a page is short.** Reserved
keys and keys outside your ACL are filtered out, so a page can come back shorter
than `limit` while more keys remain. `limit` defaults to 100 and is *clamped* at
500 rather than rejected.

`next_cursor` is a *position*, not a key: normally the last returned key plus a
NUL byte, so it reveals only keys you have already been shown.

Values are **not** returned — fetch them with `GET /kv/{key}`. A page of 500
values could be hundreds of megabytes.

### Cluster status

```http
GET /cluster/status
```

Requires the `read` class and applies no key-pattern check, since it addresses no
key. **Every node answers for itself**, including its own belief about who leads
— it needs no leader and touches no log. Poll all three and a disagreement
between them is the information.

```json
{"node_id":"node1","state":"Leader","term":4,
 "leader_id":"node1","leader_addr":"node1:8088",
 "peers":[{"id":"node1","address":"node1:8088","suffrage":"Voter"}],
 "first_log_index":1,"last_log_index":118,"applied_index":118,
 "commit_index":118,"last_snapshot_index":0,
 "key_count":42,"wal_bytes":9310,"auth_enabled":false}
```

`error` appears only when one field could not be read and the rest still stands.
An **unreachable sidecar is a `502`**, never a `200` with zeroed fields — a
dashboard showing "term 0, no peers, not leader" is indistinguishable from a
cluster that has lost quorum.

### List users

```http
GET /auth/users
```

Requires the `admin` class; `403` when authentication is disabled, like the rest
of `/auth/*`. Returns `{"users":["alice","bob"]}` — **names only**, never a salt
or a password hash. The bootstrap administrator from `RAFTKV_ADMIN_PASSWORD` is
not a record and is not listed.
````

- [ ] **Step 3: Document the console in the README**

Add a section after the API Reference:

````markdown
## Management console

A browser console is served by the engine itself at
**<http://localhost:8080/console/>** (`/` redirects there). No extra process, no
extra port, no extra container: the built assets are embedded in `kvdb_node` as
read-only data at compile time.

Three pages — a cluster overview, a key browser with prefix listing and a
single-key console, and user/ACL management when authentication is on.

**What it costs the database.** Nothing while idle: no background thread, no
timer, and nothing computed until a browser asks. The cluster page polls once
every 3 s **only while its tab is visible**, and stops entirely when hidden. The
one standing cost is the ordered key index behind `GET /kv`, at roughly 48–64
bytes per key.

**Authentication.** The static assets are served without a credential — a page
that needed one to load could not render a login form — while every API call it
makes goes through the normal gate. With authentication off, the console shows a
banner saying so.

**Building without Node.** `KVDB_CONSOLE` defaults to `ON` and the Docker build
handles it. A local CMake build with no Node needs `-DKVDB_CONSOLE=OFF`; the
console routes then answer `404 {"error":"console not built into this binary"}`.

```bash
cd console && npm ci && npm run build   # produces console/dist
cmake -S cpp-app -B cpp-app/build       # embeds it
```
````

- [ ] **Step 4: Update `CLAUDE.md`**

Three edits.

Add to the Component Map's "Client-facing API" row, in the C++ column:
`network/static_assets.hpp` + the generated `console_assets_generated.hpp`
(embedded console served at `/console/`).

Add to the test-layer description for C++ unit tests: `static_assets_test`.

Add to the e2e description: `tests/e2e/test_console.py` runs in the default
suite; its load-bearing assertion is that a key written on the leader appears in
`GET /kv` on all three nodes, because that is the only check covering
propose → replicate → apply → the follower's own index.

Add a new paragraph to the Architecture section, after the read-path
consequences:

```markdown
**Console surface.** Three JSON routes back the browser console: `GET /kv`
(paginated key names, `read` class), `GET /cluster/status` (this node's own raft
view via the unary `RaftNode.Status` RPC, `read` class, no key-pattern check),
and `GET /auth/users` (names only, `admin` class). Four rules that are auth or
correctness boundaries rather than details:

- **Listed keys and cursors are percent-encoded.** Keys are arbitrary bytes; a
  JSON string is Unicode text. `json_escape()` does not save this — it passes
  bytes ≥ 0x20 through, which is right for UTF-8 and wrong for arbitrary bytes.
- **`scan_keys` takes an INCLUSIVE start position.** An exclusive cursor cannot
  express "resume past a whole range", which is how the reserved `__sys:` space
  is skipped in one `lower_bound` so a reserved key is never examined and can
  never surface in a listing or a cursor. Do not "simplify" it to an exclusive
  after-key.
- **The `/kv` page is cut AFTER filtering**, so the returned position always
  derives from an emitted key. A caller whose patterns are not `*` must scan
  inside its own allowance (403 otherwise) — that rule is what stops a
  filtered-out key name reaching the cursor.
- **`std::set<std::string_view> index_` holds views into `store_`'s own key
  strings.** Exactly three helpers — `put_unlocked`, `erase_unlocked`,
  `replace_all_unlocked` — may touch `store_`, and each maintains the index. An
  insert registers a view of the *map node's* key, never of the caller's
  argument; a view of the argument dangles and still appears to work. Nothing
  else in `kv_store.hpp` may mutate `store_` directly.

The console's static bytes are served **outside** the authenticate-once gate —
the only exception besides `/metrics`. Safe because they are compile-time
constants holding no cluster state, and necessary because a page needing a
credential to load cannot render a login form.
```

Amend the limitations list: the "No user listing" bullet was removed in Task 6
and the in-memory bullet amended there. Add one new bullet:

```markdown
- **The console shows one node's view, not a fanned-out cluster view**: it polls the node that served it, which answers `/cluster/status` locally and includes the committed peer list. It cannot poll sibling nodes directly, because a served page cannot know their browser-reachable URLs (published host ports are a compose detail, and the secure profile puts one proxy address in front of all three). Open the console on each node to compare views.
```

- [ ] **Step 5: Update `docs/architecture.md`**

Add `/console/` to the Ports table's `8080` row, and add to the Security model
table a row for the console: *Static console assets — served unauthenticated on
the client port in both profiles; compile-time constants, no cluster state.*

Add to the security design points:

```markdown
- **The console's static assets are outside the authentication gate, and its API
  calls are not.** The bytes are `constexpr` arrays holding no keys, values or
  configuration; a page that required a credential to load could not render a
  login form. This is the only such exception besides `/metrics`.
- **Key *names* are protected like key values.** `GET /kv` applies the caller's
  ACL patterns per key and refuses a prefix outside them, because enumeration
  would otherwise route around the check `GET /kv/{key}` enforces.
```

- [ ] **Step 6: Update `CHANGELOG.md`**

Add an entry in the file's existing style:

```markdown
### Added

- **Management console** at `/console/`, served by the storage engine from
  embedded assets — no extra process, port or container. Cluster overview, key
  browser with prefix listing, single-key console, and user/ACL management.
- `GET /kv` — paginated key listing with percent-encoded keys and an opaque
  cursor position (`read` class).
- `GET /cluster/status` — this node's own raft view, over a new unary
  `RaftNode.Status` RPC (`read` class).
- `GET /auth/users` — user names, admin only. Retires the "no user listing"
  limitation.
- `KVDB_CONSOLE` CMake option (default `ON`) and `KVDB_CONSOLE_DIST`. `OFF`
  builds without Node and answers `404 console not built into this binary`.

### Changed

- `PersistentKVStore` keeps an ordered index of its keys (~48–64 bytes per key)
  to serve prefix scans in `O(log n + page)` rather than `O(n)` per request.
```

- [ ] **Step 7: Verify every documented row against the running cluster**

Do not trust the tables — check them. Bring up a cluster and confirm each status
code and body in the three new README sections, then confirm the `contracts.py`
constants match what the engine actually emits:

```bash
docker build -t raftkv:latest . && docker compose up -d
curl -si 'http://localhost:8080/kv?limit=0' | head -1
curl -s  'http://localhost:8080/kv?limit=0'
curl -si 'http://localhost:8080/kv?prefix=__sys%3A' | head -1
curl -s  http://localhost:8080/cluster/status
curl -si http://localhost:8080/auth/users | head -1
pytest tests/e2e -v
docker compose down
```

- [ ] **Step 8: Commit**

```bash
git add README.md CLAUDE.md docs/architecture.md CHANGELOG.md \
  .github/workflows/ci.yml
git commit -m "docs: document the console, its three routes and the key index

Every row was checked against a running cluster, and contracts.py carries the
same constants -- the handler, the README tables and the test constants change
together, which is a standing rule here.

CLAUDE.md gains the four rules that are auth or correctness boundaries rather
than details: percent-encoded keys and cursors, the inclusive scan position
that makes the reserved-range skip possible, the page being cut after
filtering, and the three helpers that are the only code permitted to mutate
store_."
```

---

## Self-Review

Run against the spec after the plan was written.

**1. Spec coverage.** Every spec section maps to a task:

| Spec section | Task |
|---|---|
| Ordered key index, invariant, `scan_keys` | 1, 2 |
| `GET /kv` (encoding, filters, cursor, budget, covered prefix) | 3 |
| `GET /cluster/status` (502-not-200, `read` class) | 5 |
| `GET /auth/users` | 6 |
| Metrics route labels | 3, 5, 6, 8 |
| Proto + Go changes | 4 |
| Build, embedding, serving, caching, CSP | 7, 8, 9 |
| The three pages, credential handling, banner | 10, 11, 12 |
| Testing (C++ unit, e2e, auth, secure) | 1–8 inline, 13 |
| Limitations retired/amended | 6, 14 |
| Efficiency budget | held by 1 (index cost), 8 (`.rodata`), 10 (visibility gate) |
| Security posture | 3 (filters), 8 (gate exception), 14 (docs) |

No gap found.

**2. Placeholder scan.** No "TBD", "TODO", "similar to Task N", or bare "add
error handling". Three steps deliberately instruct the executor to *read the
existing code first* and match it — the `PUT /auth/users/{name}` request body
(Task 10 Step 2), the `state_machine_test.cpp` store double (Task 2 Step 5), and
the fixture/credential helper names in `test_auth.py` and `test_secure_profile.py`
(Task 13 Steps 4–5). Those are verification instructions with a named target, not
placeholders: the C++ handler and the existing fixtures are the contract, and
inventing names for them here would be worse than saying so.

**3. Type consistency.** Checked across tasks:

- `IKVStore::KeyPage { keys, reached_end }` — defined Task 2, consumed Tasks 3, 6.
- `scan_keys(prefix, start, limit)` — same argument order and inclusive-`start`
  meaning in Tasks 2, 3, 6, and in both test fakes.
- `IStoreStats::key_count()` / `wal_size_bytes()` — defined Task 5, matching the
  method names `PersistentKVStore` already has.
- `StatusResult` field names match `StatusResponse`'s proto fields one-to-one
  (Tasks 4, 5), and the JSON keys the handler emits (Task 5) match the
  `ClusterStatus` TypeScript interface (Task 10) and the e2e assertions (Task 13).
- `percent_encode` / `next_position` / `reserved_range_end` — defined Task 3,
  reused Tasks 5, 6.
- `find_static_asset` / `pick_static_asset` / `StaticAsset` — defined Task 7,
  consumed Task 8, and the generated table's field order matches the struct.
- `kConsoleAssets` / `kConsoleAssetCount` — emitted Task 8's generator, consumed
  Task 8's handler.
- `api.ts` exports — declared in Task 10's Interfaces block and used by exactly
  those names in Tasks 11 and 12.
- `KEY_LIST_LIMIT_ERROR` / `PREFIX_NOT_COVERED_ERROR` — defined Task 13 Step 1,
  used Steps 2 and 4, and the strings match the C++ literals in Task 3.

One inconsistency found and fixed while reviewing: Task 3's error string was
`"limit must be a non-negative integer"` in an early draft, which contradicted
rejecting `limit=0`. It is `"limit must be an integer between 1 and 500"`
everywhere now — handler, tests, `contracts.py` and README — and the spec's table
was corrected to match (recorded in [Deltas from the spec](#deltas-from-the-spec)).
