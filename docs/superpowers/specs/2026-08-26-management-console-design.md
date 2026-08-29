# Management Console — Design

**Date:** 2026-08-26
**Status:** approved, not yet implemented

A browser console for cluster inspection and key management, served by the
existing C++ engine on port 8080 out of the same container. Comparable in intent
to the Qdrant or Milvus dashboards: one page an operator opens to see who leads,
what is stored, and who may read it.

## Contents

- [Goals and non-goals](#goals-and-non-goals)
- [Architecture](#architecture)
- [Workstream 1 — the ordered key index](#workstream-1--the-ordered-key-index)
- [Workstream 1 — new HTTP routes](#workstream-1--new-http-routes)
- [Workstream 1 — proto and Go changes](#workstream-1--proto-and-go-changes)
- [Workstream 2 — build, embedding and serving](#workstream-2--build-embedding-and-serving)
- [Workstream 3 — the console app](#workstream-3--the-console-app)
- [Workstream 4 — testing](#workstream-4--testing)
- [Efficiency budget](#efficiency-budget)
- [Security posture](#security-posture)
- [Limitations this changes](#limitations-this-changes)
- [Rejected alternatives](#rejected-alternatives)

## Goals and non-goals

**Goals.** Four pages' worth of function: a cluster overview, a key browser with
prefix listing and pagination, a single-key console (get/put/delete with a
consistency toggle), and user/ACL management when authentication is on. Served
from the database container with no new process and no new port. Zero server-side
cost when nobody has the page open.

**Non-goals for this design.** Charts or time-series history (that is what
`/metrics` and Prometheus are for; duplicating it in-process would mean retaining
samples in the engine). Log tailing. A write path that bypasses raft. Any change
to how a write is proposed, replicated or applied. Response content negotiation
(`Accept: application/msgpack`) remains the separate open item it is today.

**The constraint that shapes everything below:** the database is the primary
tenant of this container. The console may not add a background thread, a timer, a
retained buffer, or a data-structure cost that scales with anything but key
count — and the one structure it does add has to be justified against the write
path it sits on.

## Architecture

One new surface on the existing hand-rolled HTTP server (port 8080). No new
process, no new port, no new container, no new runtime dependency in the image.

```
browser ──GET /console/*───────▶ kvdb_node   static bytes from .rodata, no auth gate
        ──GET /cluster/status──▶ kvdb_node ──gRPC RaftNode.Status──▶ sidecar :50052
        ──GET /kv?prefix=…─────▶ kvdb_node ──▶ PersistentKVStore (local ordered index)
        ──PUT/GET/DELETE /kv/{key}────▶ existing routes, unchanged
        ──GET /auth/users[/{name}]────▶ existing routes + one new list route
```

The console is a Preact single-page app using **hash routing** (`#/cluster`,
`#/keys`, `#/users`). Hash routing rather than the History API is a deliberate
simplification: the server then needs no catch-all rewrite, and `/console/`
plus `/console/assets/<hashed>` are the only static paths that exist. A
catch-all would also have to decide what a genuinely unknown path under
`/console/` means, and answering "here is the app" to every typo is how a 404
becomes unobservable.

Four workstreams. Each is independently verifiable and leaves the tree green, so
this is also the phasing for the implementation plan:

| # | Workstream | Verifiable by |
|---|---|---|
| 1 | Backend APIs: ordered key index, `scan_keys`, `Status` RPC, three routes | `ctest` and `curl`, no UI involved |
| 2 | Build and serve: Vite → CMake embed → static routes | `curl http://localhost:8080/console/` returns a placeholder page |
| 3 | The Preact app | a browser |
| 4 | End-to-end tests, contracts, documentation | `pytest tests/e2e` |

## Workstream 1 — the ordered key index

`GET /kv?prefix=` needs keys in lexicographic order with a cursor. `store_` is a
`std::unordered_map<std::string, std::string>` (`StateMap`, `kv_store.hpp:35`),
which has no order at all.

### The structure

```cpp
std::set<std::string_view> index_;   // views into store_'s own key strings
```

`unordered_map` node addresses are stable across rehash, so a view of
`it->first` stays valid for that node's lifetime. No key bytes are copied.

**Cost, stated honestly.** An RB-tree node is three pointers plus a colour word
(32 bytes) plus the `string_view` (16 bytes) plus allocator overhead — call it
**48–64 bytes per key**, independent of key length. On the write path it adds one
`std::set::insert` on *new* keys only, next to a WAL `fsync` that already
dominates by orders of magnitude; the measured write ceiling of ~1.9k writes/s is
set by consensus and durability, not by this.

**The index is byte-transparent, like the store.** `std::string_view` carries a
length, and `std::less<std::string_view>` compares via `char_traits::compare`, so
keys containing `=`, newlines or NUL bytes order correctly and exactly. That
property was won in Phase 2 and must not be quietly given back.

### The invariant, and how it is enforced

A dangling `string_view` is a use-after-free that reads as a corrupted key. There
are **six** places `store_` is mutated today:

| Site | Line (at time of writing) | Shape |
|---|---|---|
| `set()` | `store_[key] = value` (275) | per-key insert-or-update |
| `remove()` | `store_.erase(key)` (307) | per-key erase |
| `restore_state()` | `store_ = std::move(state)` (399) | whole-map replace |
| base-file load | `store_ = deserialize_state(...)` (458) | whole-map replace |
| legacy line parser | `store_[...] = ...` (494) | per-key insert |
| WAL replay | `store_[cmd.key] = ...` / `erase` (557, 560) | per-key insert / erase |

Auditing six sites forever is the failure mode. So the design **funnels all six
through three new private helpers**, which become the only code in the file
permitted to touch `store_` directly:

```cpp
void put_unlocked(const std::string &key, const std::string &value);
bool erase_unlocked(const std::string &key);
void replace_all_unlocked(StateMap state);   // rebuilds index_ from scratch
```

Each maintains `index_` alongside `store_`. The rules they encode:

- **An insert registers a view of the map node's key, never of the caller's
  argument.** `put_unlocked` inserts `std::string_view(it->first)` from the
  iterator `store_.emplace`/`operator[]` yields. A view of the parameter would
  dangle the moment the caller's string went out of scope — and would *appear* to
  work, because the bytes are usually still there.
- **An erase removes the view before erasing the node**, in that order.
- **A whole-map replace clears and rebuilds**, because every old view is dead and
  every new node is unregistered.

This extends the existing `_unlocked` discipline documented in
`.claude/rules/cpp.md` — these helpers assume `mutex_` is held and must never
re-acquire it, exactly like `maybe_compact_unlocked()`. All six call sites are
already inside the lock (the constructor's are outside it only because nothing can
observe the object yet, which the rules file already carves out).

Compaction needs no change: `maybe_compact_unlocked()` serializes `store_` and
rewrites the base file without mutating the map, so no view moves. That is worth a
test anyway — a no-op today is a regression tomorrow.

### The scan API

```cpp
// IKVStore
struct KeyPage {
  std::vector<std::string> keys;   // raw key bytes, lexicographic order
  bool reached_end = false;        // no further key carries this prefix
};

[[nodiscard]] virtual KeyPage
scan_keys(std::string_view prefix, std::string_view start, size_t limit) const = 0;
```

Returns up to `limit` keys that start with `prefix`, beginning at `start`
**inclusive**, in lexicographic order. Empty `start` means "from the beginning of
the prefix range".

Implementation: `it = start.empty() ? index_.lower_bound(prefix) :
index_.lower_bound(start)`, then walk while the prefix still matches, copying keys
out. C++17 has no `starts_with`, so the match is `k.size() >= p.size() &&
k.compare(0, p.size(), p) == 0`.

**`start` is inclusive, not exclusive, and that choice is load-bearing.** An
exclusive "after the key K" cursor cannot express "resume past an entire range of
keys", because strings have no immediate predecessor. An inclusive position can
express both: `K + "\0"` is the first position strictly after `K` (any key greater
than `K` either extends `K`, and so is `>= K + "\0"`, or diverges later), and
`"__sys;"` is the first position past every `__sys:`-prefixed key, since `';'` is
the byte after `':'`. The reserved range is therefore skipped as a *range* in one
`lower_bound`, not filtered key by key — which is what keeps a reserved key from
ever surfacing as a cursor.

`reached_end` distinguishes "this page filled up" from "the prefix is exhausted",
which the handler needs because it may call `scan_keys` more than once per
request (see below) and a short page does not mean the last page.

The predicate that decides which keys a *caller* may see is deliberately **not** a
parameter here. Passing a callback would mean running caller-supplied logic while
the store mutex is held; instead the handler loops over short, self-contained
`scan_keys` calls, each taking and releasing the lock. The store stays ignorant of
authorization, and no lock is held across a filter.

**Keys are copied out under the lock; the lock is released before anything is
filtered or serialized.** One `scan_keys` call is O(log n + limit) and `limit` is
capped, so each critical section is short and a request that needs several of them
releases the mutex between each. That bound is the entire reason this is an index
and not a full pass over the map: a full pass would hold the store lock against
`Apply` for O(n) on *every page request*, which is the opposite of "the database
is the main thing running".

The `__sys:` range skip is performed by the **handler**, by choosing `start`. The
store knows nothing about reserved prefixes or authorization — `storage/` does not
depend on `auth/`, and this design does not change that.

`scan_keys` goes on `IKVStore` (unlike `key_count()`, which deliberately did not)
because the HTTP handler is a consumer of it and the handler is tested against a
fake. `FakeKVStore` in `http_handler_test.cpp` is already backed by a
`std::map`, so its implementation is a short loop.

## Workstream 1 — new HTTP routes

All three sit **inside** the existing authenticate-once gate in
`route_request()`. None of them is a new authentication surface.

### `GET /kv?prefix=&cursor=&limit=` — list keys

Requires the `read` class. Response:

```json
{"keys":["app%3Aa","app%3Ab"],"next_cursor":"app%3Ab%00"}
```

**`next_cursor` is a position, not a key**, and it is present if and only if more
keys may follow. A client pages by passing it back as `cursor` and stops when the
field is absent — never by comparing the returned count against `limit`, which can
be short while more keys remain (see the filtering note below). There is no
separate `truncated` flag: two fields carrying one fact drift apart.

The position is normally the last emitted key plus a NUL byte, which is why the
example ends in `%00`. It therefore reveals only keys the caller has already been
shown.

**Every key in the response is percent-encoded, and this is load-bearing.** Keys
are arbitrary bytes; a JSON string is Unicode text. A key containing a raw `0x80`
would produce a body no JSON parser accepts, and `json_escape()` would not save it
— it escapes the delimiters and C0 controls and passes bytes ≥ 0x20 through
untouched, which is correct for UTF-8 payloads and wrong for arbitrary ones.
Percent-encoding also means `next_cursor` can be pasted straight back into the
query string, and a listed key can be pasted straight into `/kv/{key}`, which
percent-decodes.

`prefix` and `cursor` arrive percent-encoded and are decoded with the existing
`url_decode(raw, /*decode_plus=*/true)`. `decode_plus` is true here because these
are query-string parameters, matching the convention documented at
`http_request.hpp:22` — and note `query_params()` returns values **undecoded**, so
the handler must do this explicitly rather than assume.

**This route decodes while `/get-val?key=` does not, and that is not a new
inconsistency.** `/kv` belongs to the REST surface, which decodes; the legacy
routes are frozen precisely so decoding cannot silently move which key an existing
client reaches. Do not "harmonise" them.

| Outcome | Status | Body |
|---|---|---|
| Success | `200` | `{"keys":[…],"next_cursor":"…"}`, cursor absent at end of range |
| `limit` absent | `200` | default 100 |
| `limit` above 500 | `200` | clamped to 500, not rejected — a cap is not a client error |
| `limit` not an integer in `1..500` | `400` | `{"error":"limit must be an integer between 1 and 500"}` |
| malformed percent-encoding in `prefix` or `cursor` | `400` | `{"error":"malformed percent-encoding in the query string"}` |
| `prefix` under the reserved space | `403` | the existing `__sys:` reserved-prefix body |
| `prefix` outside the caller's key patterns | `403` | `{"error":"prefix must fall within your permitted key patterns"}` |
| authenticated but lacking `read` | `403` | `{"error":"permission denied"}` |

Two filters apply to every key before it is emitted:

- **`__sys:` keys are excluded unconditionally**, including for admins — the
  existing rule that `__sys:` is refused on data routes *in the read direction
  too*. A listing that revealed `__sys:user:alice` would leak the account roster
  through the one route whose whole purpose is enumeration. As described above this
  is a range skip, not a per-key filter, so a reserved key is never even examined.
- **The caller's ACL key patterns are applied per key.** A user scoped to `app:*`
  sees only matching keys. Without this, listing would route around the pattern
  check that `GET /kv/{key}` enforces — read access to a key's *name* is read
  access.

**The page is cut after filtering, not before.** The handler loops over short
`scan_keys` calls, filters each batch, and accumulates until it has `limit`
emitted keys, the store reports `reached_end`, or it has examined an overall
budget of keys (proposed: `max(1000, 10 × limit)`). The loop is what makes the
cursor safe: because a page ends on an *emitted* key, the returned position is
always derived from a key the caller was allowed to see.

The one case that escapes that is the examine budget running out with fewer than
`limit` keys emitted — then the position must name the last key examined, which
the caller may not be permitted to read. That is why a caller whose patterns are
not simply `*` **must supply a `prefix` covered by one of their patterns**, and
gets a 403 otherwise (the row above). With that rule, a restricted caller only
ever scans inside its own allowance, nothing is filtered out, and the budget
cannot be exhausted by filtering. Non-prefix patterns (`a*b`, `?ab*`) can still
reach the residual case; the disclosure there is bounded at one key name per
request and is recorded in [Security posture](#security-posture) rather than
being papered over. Every pattern the console itself creates is a prefix glob.

`/kv` has no trailing slash and therefore cannot collide with `/kv/{key}`, whose
prefix constant is `"/kv/"`. `GET /kv/` keeps its current answer, `400 key must
not be empty`.

### `GET /cluster/status` — cluster overview

Requires the `read` class, with **no key-pattern check**, because it addresses no
key. That is a judgment call and is recorded as one: cluster topology is not user
data, the management API on port 6000 is separately bearer-token gated, and
requiring `admin` here would make the overview page invisible to exactly the
users most likely to open it. If that trade is ever revisited, this paragraph is
the thing to change.

```json
{"node_id":"node1","state":"Leader","term":4,
 "leader_id":"node1","leader_addr":"node1:8088",
 "peers":[{"id":"node1","address":"node1:8088","suffrage":"Voter"}],
 "first_log_index":1,"last_log_index":118,"applied_index":118,
 "commit_index":118,"last_snapshot_index":0,
 "key_count":42,"wal_bytes":9310,"auth_enabled":false}
```

`key_count`, `wal_bytes` and `auth_enabled` are local to the C++ process and cost
no gRPC hop. Everything else comes from the sidecar.

**A failed sidecar call answers 502, not 200 with zeros.** A dashboard rendering
"term 0, no peers, not leader" for a node whose sidecar is merely unreachable is
worse than an error: it looks like a cluster that has lost quorum. The existing
`unavailable:`/`not_leader:` prefix contract does not apply here — `Status` needs
no leader — so this is a plain transport failure mapped to 502.

`Status` is read-only, touches no log, and needs no leader: **every node answers
for itself**, including its own belief about who leads. That is what makes the
overview page honest. The console polls all three nodes and shows three
independent views; when they disagree, that disagreement is the information.

### `GET /auth/users` — list users

Requires `admin`. Returns `{"users":["alice","bob"]}`.

Same scan primitive, pointed at `__sys:user:` internally, with the prefix
stripped. Usernames are charset-restricted to `[A-Za-z0-9_.-]` and ≤128 bytes by
`auth::username_error()`, so unlike data keys they are safe to emit raw in JSON.

The handler returns **names only** and never deserializes a `UserRecord` into the
response, so no salt and no password hash can reach a client through it. The
bootstrap admin from `RAFTKV_ADMIN_PASSWORD` is not a record and does not appear;
the response documents that by including it in neither direction — consistent
with `PUT /auth/users/admin` already being refused.

### Metrics

`route_label()` gains `GET /kv`, `GET /cluster/status` and `GET /auth/users`, and
collapses every static asset into a single **`GET /console/*`** label. Per-asset
labels would create one time series per file, which is the same unbounded-cardinality
mistake that already keeps the key out of `/kv/{key}` and the user name out of
`/auth/users/{name}`.

## Workstream 1 — proto and Go changes

```proto
service RaftNode {
  rpc Status(StatusRequest) returns (StatusResponse);   // new, unary
}

message Peer {
  string id = 1;
  string address = 2;
  string suffrage = 3;
}
```

Fresh tags only; nothing renumbered or reused. `StatusResponse` carries the raft
fields listed above plus `repeated Peer peers`. Go stubs are regenerated and
committed per `.claude/rules/protobuf.md`; `cpp-app/pb/` stays untouched (it is a
stale, unreferenced copy and the C++ build generates into `build/proto/`).

Go changes, all additive:

- **`raftnode.Node.Configuration()`** wrapping `Raft.GetConfiguration()` — the
  only genuinely new capability. `Stats()`, `IsLeader()` and `LeaderAddr()`
  already exist and are reused as-is.
- **`rpc.Server.Status`** reading a new consumer-side interface
  (`RaftStatusReporter`) supplied the same way `LocalReader` is, so `internal/rpc`
  still does not import `internal/raftnode` and the dependency direction the
  existing interfaces exist to break stays broken.
- **`IRaftClient::status()`** on the C++ side returning a `StatusResult{success,
  error, …}`, matching the shape of the existing `ProposeResult` and `ReadResult`
  rather than inventing a third error convention.

No change to `Propose`, `Apply`, `Read`, the FSM, snapshots, or membership.

## Workstream 2 — build, embedding and serving

New top-level `console/`: `package.json`, `vite.config.ts`, `src/`, and
`scripts/gzip-dist.mjs` — a post-build step that gzips each text asset using
Node's own `zlib`, because CMake has no portable raw-gzip primitive
(`file(ARCHIVE_CREATE)` is tar-based).

CMake, when `KVDB_CONSOLE=ON` (the default), reads `${KVDB_CONSOLE_DIST}` and
generates `build/console/console_assets.hpp`: per asset a `constexpr unsigned
char[]` of raw bytes, a second array of gzip bytes, a content type, and a
content-hash ETag, plus a `constexpr` table. Lookup is a linear scan — there are
fewer than twenty assets, and the comment will say that is why.

- **`dist/` missing while `KVDB_CONSOLE=ON` is a fatal CMake error, never a
  silent skip.** A node that quietly ships without a console is the same class of
  failure as a TLS surface that quietly falls back to plaintext: the operator
  asked for a thing and got something else. A local build without Node opts out
  explicitly with `-DKVDB_CONSOLE=OFF`, and the routes then answer `404
  {"error":"console not built into this binary"}` — an answer, not a mystery.
- **Both raw and gzip bytes are embedded** (roughly +250 KB of `.rodata` on a
  multi-megabyte binary) and selected by `Accept-Encoding`. The cost buys two
  things: `curl http://localhost:8080/console/` stays readable for debugging, and
  no decompressor has to be linked into the engine to serve a client that does not
  advertise gzip.
- The Dockerfile gains a `node:22-alpine` stage ahead of `cpp_builder`, which
  copies `dist/` in. **The runtime image is unchanged** — no Node, no new shared
  library.
- Caching: `index.html` is `Cache-Control: no-cache` plus an `ETag`, so a revisit
  costs a 304 with no body. Hashed assets under `/console/assets/` are
  `immutable, max-age=31536000`. `GET /` answers `302` to `/console/`.

**Static routes are checked before the authentication gate**, alongside
`/metrics`. That is a deliberate exception to the rule in CLAUDE.md that
authentication happens once before any route runs, and it will carry a comment
saying so. It is safe for a narrow reason: these bytes are compile-time constants
containing no cluster state, no keys, no values and no configuration. Every API
call the page subsequently makes goes through the gate normally. A page that
required a credential to load could not render a login form.

Security headers on the console HTML — all compile-time constants, so the
`extra_headers` rule that values must not be attacker-influenced holds:

```
Content-Security-Policy: default-src 'self'; object-src 'none';
                         base-uri 'none'; frame-ancestors 'none'
X-Content-Type-Options: nosniff
Referrer-Policy: no-referrer
```

## Workstream 3 — the console app

Preact plus Vite. Three routes, and the app must work with no credentials because
the default profile has no authentication.

- **Cluster.** The nodes as cards: state, term, leader, `first`/`last`/`applied`/
  `commit`/`snapshot` indices, key count, WAL size. Replication lag shown as
  `last_log_index − applied_index`. Polls `/cluster/status` every 3 s **only while
  `document.visibilityState === 'visible'`**, with a visible pause control. A
  backgrounded tab polls nothing.
- **Keys.** A prefix box and a paginated list from `/kv`; clicking a key fetches
  its value and offers edit (`PUT`), delete, and a `consistency=linearizable`
  toggle. **Values are fetched on click only, never with the list** — a page of
  500 values could be 500 MiB, which is the opposite of what was asked for.
- **Users.** Rendered only when `auth_enabled`. List, create, update, delete via
  `/auth/users`. The UI states the real constraints rather than implying more:
  three command classes, glob patterns with no escapes, no quotas, no audit log.

**Credential handling.** The app renders its own login form and sends
`Authorization: Basic`, holding the credential in `sessionStorage`. The trade-off,
stated rather than buried: that is weaker than an httpOnly cookie session, because
a cross-site scripting bug could read it. The mitigations are the CSP above and a
no-`innerHTML` discipline in the app. The stronger alternative — a real session
token endpoint — needs a new committed record type and a token store, which is a
larger change than the console itself and is out of scope here. Native browser
Basic auth was also considered and rejected: it pops a modal dialog on every 401
and offers no way to log out.

When authentication is off, the page shows a persistent banner saying the API is
unauthenticated and pointing at `docker-compose.auth.yml`.

**The console adds no exposure.** It is served from the port that, in the default
profile, already accepts unauthenticated reads and writes of every key. It makes
existing exposure legible, which is an argument for it rather than against it.

## Workstream 4 — testing

**C++ unit tests.** New test files are listed explicitly in `CMakeLists.txt`,
never globbed, so a new test file is a visible diff.

- `kv_store_test`: prefix boundaries (a prefix that is itself a key; keys
  differing only after the prefix); inclusive-`start` semantics, including that
  `K + "\0"` resumes strictly after `K` and that `"__sys;"` lands past the whole
  reserved range; `reached_end` set only at the true end of a prefix; pagination
  across an insert and an erase between pages; `limit` clamping; index consistency
  after `remove`, `restore_state`, a base-file load, a legacy-format load and a WAL
  replay; that a compaction rewrite strands no view; and that keys containing `=`,
  newlines and NUL bytes order and round-trip through `scan_keys` exactly. A NUL in
  a key is the case that would break a naive exclusive cursor, so it is pinned
  deliberately.
- `http_handler_test`: the full status, body and content-type contract for all
  three new routes, auth-on and auth-off; percent-encoding of emitted keys and
  cursors, including a key with invalid UTF-8 bytes; that a reserved key never
  appears in a listing or a cursor; ACL-pattern filtering of listed keys and the
  403 for a prefix outside the caller's patterns; that a filtered page still
  paginates to completion rather than stopping short; the 502-not-200 behaviour
  when the fake raft client fails `status()`; and the console-not-built 404.
- New `console_assets_test`: ETag stability across builds of identical input,
  content types, and `Accept-Encoding` selection between the raw and gzip
  variants.

**End-to-end, default suite** — new `tests/e2e/test_console.py`: `/` redirects;
`/console/` is `200 text/html` and answers `304` to a conditional re-request;
`/cluster/status` on all three nodes agrees on `leader_id`; and a key written
through the leader appears in `GET /kv?prefix=` on **all three** nodes, polled
against a deadline rather than slept on. That last one is the load-bearing
assertion, for the same reason the auth suite's is: it is the only check that
covers propose → replicate → apply → a follower's own index reading it back.

**`requires_secure`** gains one case proving the console loads through the Caddy
proxy, since the round-robin stanza means three different nodes serve the assets.

**`requires_auth`** additions to `test_auth.py`: console assets load with **no**
credential; `/cluster/status` and `/kv?prefix=` are refused without one;
`/auth/users` requires `admin` and its body contains neither a salt nor a hash; a
`read`-class user scoped to `app:*` listing `/kv?prefix=` sees no key outside that
pattern.

**Documentation changed in the same commits as the handler**, per the existing
rule that the handler, the README table and `tests/e2e/contracts.py` move
together: README API reference, `contracts.py`, CLAUDE.md (component map, ports,
and the limitations edits below), `docs/architecture.md`, and CHANGELOG.

## Efficiency budget

What the console costs the database, in full:

| Resource | Cost |
|---|---|
| Processes, ports, containers | none added |
| Background threads or timers | none — nothing runs until a request arrives |
| Resident memory, idle | ~250 KB of `.rodata` (embedded assets, never copied) |
| Resident memory, per key | 48–64 bytes for the ordered index |
| Write path | one `std::set::insert` on new keys only, beside an existing `fsync` |
| Read path | unchanged |
| Store lock held per `scan_keys` call | O(log n + limit), limit ≤ 500, released between calls |
| Keys examined per list request | bounded by `max(1000, 10 × limit)` |
| Network, per open tab | one `/cluster/status` per node per 3 s while visible; nothing while hidden |
| Runtime image size | unchanged (Node lives only in a build stage) |

## Security posture

- The console's static bytes are outside the authentication gate; every API call
  it makes is inside it.
- `__sys:` stays unreadable through data routes in both directions, listing
  included, and is skipped as a range so a reserved key is never examined and can
  never appear in a cursor.
- ACL key patterns are enforced on names, not just values.
- **Known, bounded disclosure in key listing.** A caller with non-prefix glob
  patterns (`a*b`, `?ab*`) who exhausts the examine budget without filling a page
  receives a cursor derived from the last key examined, which may be a key they
  cannot read — one key name per request. Callers with prefix patterns, which is
  every pattern the console creates, are required to scan inside their own
  allowance and cannot reach this case. Recorded rather than hidden; closing it
  properly means pattern-aware range decomposition, which is not worth it for the
  first version.
- Cluster topology is `read`-class; user administration stays `admin`-class.
- No new unauthenticated surface is created. The peer-reachable, unauthenticated
  sidecar port 50052 remains the known gap it is documented as; `Status` is
  read-only and adds no write capability to it, but it does let anyone who can
  reach that port read cluster topology, which they could already infer by
  proposing.

## Limitations this changes

Retired outright:

- **"No user listing."** `GET /auth/users` exists. The underlying cause — one flat
  map with no prefix scan — is what the index fixes, so the entry goes away rather
  than being narrowed.

Amended:

- **"The whole dataset lives in memory."** Still true; the per-key cost rises by
  48–64 bytes and the entry should say so.

Not touched, and the console must not imply otherwise: at-least-once writes under
failure, unbounded local-read staleness, one raft group, proxy-terminated client
TLS, salted-SHA-256 passwords, and the unauthenticated sidecar port.

## Rejected alternatives

**Key listing by full map pass.** O(n) under the store lock on every page, which
blocks `Apply`. Acceptable at ten thousand keys, hostile at a million, and
directly contrary to the requirement that the database stays the priority.

**Key listing by switching `StateMap` to `std::map`.** No extra memory, but it
makes every hot-path `get` and `set` O(log n) with pointer chasing, and `StateMap`
is the type on the snapshot and restore seam — the widest possible blast radius
for a feature that only needs ordering on one read path.

**Cluster status via CORS on the sidecar's `:6000/status`.** No proto change, but
the browser would have to reach `:6000`, `:6001`, `:6002` per node — addresses the
page cannot know — and it does not work behind the proxy at all.

**Cluster status by the C++ engine calling `localhost:6000` over HTTP.** Requires
a hand-rolled HTTP client inside the engine when a gRPC channel to the sidecar
already exists.

**A separate console container.** Simplest to build and explicitly against the
requirement; it also puts cross-origin configuration between the page and every
node.

**Vanilla JS with no build step.** Genuinely cheaper in toolchain and supply
chain, and it was the recommendation. Preact plus Vite was chosen for developer
experience on a three-page app with real state; the cost is npm in the build and a
lockfile to maintain, and it is paid in a build stage that never reaches the
runtime image.

**React instead of Preact.** Roughly three times the bundle for a page that polls
a few JSON endpoints.

**Server-side session tokens instead of `sessionStorage`.** Stronger against XSS,
but needs a new committed record type and a token store — a larger change than
the console. Recorded here as the upgrade path if the console ever holds anything
more sensitive than a Basic credential the user just typed.
