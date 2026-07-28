# RaftKV end-to-end suite

Asserting replacement for `test_client.py`. Four families of test:

- **R0.1–R0.5** (Phase 0) — a write on the leader replicates to **all** nodes,
  DELETE round-trips, a missing key is reported as missing, and a write to a
  follower is rejected.
- **R1.7/R1.8** (Phase 1) — the truthful-error HTTP contract: real status codes,
  correct reason phrases, JSON error bodies, and a regression test for the
  remote denial of service that a malformed `Content-Length` used to cause.
- **R2.4/R2.5/R2.7** (Phase 2, `test_crash.py`) — an acknowledged write survives
  `kill -9`. Opt-in; see [The `requires_docker` split](#the-requires_docker-split).
- **R3.x** (Phase 3, `test_snapshot.py`) — the raft log is bounded, a node whose
  volume is deleted rejoins via snapshot transfer, and a restart applies a
  snapshot plus the tail instead of all of history. Opt-in, and additionally
  needs a cluster started with `docker-compose.test.yml`; see
  [The snapshot scenarios](#the-snapshot-scenarios).

## The cluster must already be running

These tests never start, stop, or build anything with docker. The compose
lifecycle belongs to the CI `e2e` job and to the `cluster-smoke-test` skill.

```bash
docker build -t raftkv:latest .
docker compose up -d
```

## Run

```bash
pip install -r tests/e2e/requirements.txt
pytest tests/e2e -v                         # the default suite: never touches docker
pytest tests/e2e -m requires_docker -v -rs  # crash + snapshot: kills, wipes, restarts
pytest tests/e2e -m requires_secure -v -rs  # TLS profile: needs the secure cluster
```

`-rs` prints skip reasons. It matters for the opt-in run: the snapshot scenarios
skip unless the cluster was started with the test override, and a skip nobody
sees is an unverified requirement.

Exit status is pytest's: non-zero on any failure. If no node accepts a write
within the leader-discovery deadline, the suite fails with a message telling you
the cluster is not up and what each node answered instead.

## The Phase 6 security modules

`test_security.py` runs in the **default** suite — it needs nothing but a live
plaintext cluster. Two groups:

*   **Request caps (R6.9).** An oversized body is 413, oversized headers are 431,
    and — the part a unit test cannot show — the node is still serving
    afterwards, including after a burst of ten. Phase 1 shipped a node that a
    malformed `Content-Length` could kill; a cap that wedges the accept loop
    instead of answering is not a cap. There is also a just-under-the-limit case,
    without which "return 413 unconditionally" would pass.
*   **Cluster membership (R6.1/R6.2).** The status codes are checked, but the
    assertion that matters is `test_refused_join_does_not_add_the_peer`: after
    two rejected `/join` calls, `last_log_index` must not have moved. Middleware
    that returned 403 *after* calling `AddVoter` would satisfy every
    status-code check and leave the cluster compromised. The valid-token case
    sends no parameters on purpose, so it proves the credential was accepted
    (400, not 401/403) without adding a phantom voter that would poison every
    later test.

`test_secure_profile.py` carries the `requires_secure` marker and needs the
cluster brought up with `docker-compose.secure.yml` plus a CA in `./certs`:

```bash
./scripts/gen-certs.sh
export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d
pytest tests/e2e -m requires_secure -v -rs
```

It does **not** re-test TLS mechanics — Go unit tests in `internal/tlsconfig` and
`internal/raftnode` already drive real sockets. It tests what only a deployment
has: that the raft port refuses plaintext *and* refuses a peer with no client
certificate (one-way TLS would encrypt everything and still let anyone append
entries), that management HTTPS verifies against the CA and refuses TLS below
1.2, and that the plaintext client ports are **not** published. That last one is
a real mistake already made once here: compose *concatenates* `ports` across
files rather than replacing them, so the first version of the secure profile
left 8080 open right next to 8443.

## The `requires_docker` split

`pytest.ini` sets `addopts = -m "not requires_docker and not requires_secure"`,
so the default run excludes `test_crash.py`, `test_snapshot.py` and
`test_secure_profile.py`. That is not a convenience — it
is what keeps the rule above true.

The rule exists because it makes the suite portable: knowing nothing about *how*
the nodes are run, it works against a local compose cluster, a remote host, or a
k8s namespace, driven only by `RAFTKV_NODES`. Two acceptance criteria cannot be
expressed that way. Phase 2's ("`kill -9` at any instant loses no acknowledged
write") has to send a real SIGKILL to a real container, and specifically SIGKILL:
`docker compose stop` sends SIGTERM, which is a graceful shutdown and would prove
nothing. Phase 3's ("a wiped node rejoins and catches up from a snapshot") has to
delete a node's volume and start it again.

So the exceptions are quarantined behind a marker and opted into explicitly
rather than being smuggled into the default run. A command-line `-m` replaces the
one in `addopts`, which is what makes the opt-in work.

Both modules **skip** — never fail — when they cannot do their job: no `docker`
CLI, no `docker compose` plugin, no reachable daemon, no containers in the
compose project, or `RAFTKV_NODES` pointing at anything other than localhost
(killing a local container proves nothing about a remote cluster). A skip means
"not verified here".

### What the crash test proves

Write K keys through the leader, confirm they replicated,
`docker compose kill -s KILL` a follower, and — *while it is dead* — assert the
keys are already in that node's `kv.db`/`kv.wal` on disk. That assertion is the
point: the naive version of this test (kill, restart, read back) would pass with
an empty store, because a restart replays raft's log back into the state machine
and would refill it. Only the on-disk check distinguishes "durable" from
"refilled by replay". The read-back after the restart is then deliberately *not*
polled — recovery finishes before the HTTP listener opens — and a final write
proves the node rejoined the cluster rather than merely coming back to life. If
the data directory cannot be read from the test host (named volume, remote
daemon, permissions), the test still runs the kill/restart and emits a
`DurabilityNotVerified` warning rather than pretending it proved more.

The test always attempts to restart the node, including when an assertion
fails; a run that dies between the kill and the restart leaves the cluster a
node short, recoverable with `docker compose start <node>`.

**Run it against the ordinary cluster, not the snapshot one.** Its final
read-back is deliberately unpolled, and since Phase 3 a restarting node briefly
serves *less* than its disk held: raft restores the local snapshot over the
recovered store and only then replays the tail. With production snapshot
settings that window does not arise inside a test run; with
`docker-compose.test.yml` it can, and the unpolled read would be racing it. CI
runs the crash test against the base cluster for exactly this reason.

## The snapshot scenarios

`test_snapshot.py` needs one thing beyond docker: a cluster that snapshots
*inside a test run*. The shipped defaults are HashiCorp Raft's own (120s / 8192
entries / 10240 trailing logs), which no test can wait for, so the cluster has
to be started with the override:

```bash
docker compose down -v && rm -rf vol-node1 vol-node2 vol-node3
docker compose -f docker-compose.yml -f docker-compose.test.yml up -d
pytest tests/e2e -m requires_docker -v -rs
```

Each test reads the tunables back out of the sidecar's own startup log line
(`Starting sidecar with config: Config{… SnapshotThreshold: 20, TrailingLogs: 10}`)
and **skips** when they are production values — so running the opt-in suite
against the ordinary cluster reports "not verified here" instead of timing out.
That is also why adding this module did not change what CI's existing
crash-test step does.

Three scenarios, in file order, sharing one write burst:

1. **A snapshot is taken and the log is truncated.** Both are asserted, and they
   are different claims. `last_snapshot_index` rising says a snapshot happened;
   `first_log_index` rising says the log was actually shortened. Raft truncates
   to `snapshot_index - TrailingLogs`, so a cluster can snapshot forever and
   still keep every entry. A complete snapshot directory (`meta.json` +
   `state.bin`, not `*.tmp`) must exist in `<data dir>/snapshots/` as well.
2. **A wiped follower rejoins.** Stop it, delete *everything* in its data
   directory, start it, and require it to serve every key. Two independent facts
   establish that it caught up by **snapshot transfer** rather than log replay:
   the leader's `first_log_index` was > 1 immediately before the wipe (so the
   beginning of history is gone and `InstallSnapshot` is raft's only option),
   and the wiped node reports `last_snapshot_index` > 0 afterwards having had a
   verified-empty snapshot store when it started.
3. **A normal restart applies a snapshot plus the tail.** Measured as a delta on
   the C++ engine's per-entry log lines: the number of entries replayed must not
   exceed `last_log_index - last_snapshot_index` (plus a small slack), where
   replaying all of history would be `last_log_index`. Restart *timing* is
   deliberately not asserted — wall-clock on a shared runner is not evidence.

Some of the evidence is log lines this repository emits itself
(`fsm: restored a …`, `[StateMachine] Restored snapshot: …`, named in
`snapshot_support.py`). That is a real coupling — renaming one breaks the test —
and it is deliberate: no index distinguishes "raft installed a snapshot" from
"the C++ engine actually took it", and R3.5's whole point is that the second
must follow from the first. Log evidence is always counted before and after an
action and compared, never grepped absolutely, because a restarted container's
logs still contain everything it printed before the restart.

The wiped node is always started again, including when an assertion fails.

## Configuration

All optional; every value has a working default for `docker-compose.yml`.

| Env var | Default | Meaning |
|---|---|---|
| `RAFTKV_NODES` | `http://localhost:8080,http://localhost:8081,http://localhost:8082` | Comma-separated node base URLs |
| `RAFTKV_HTTP_TIMEOUT` | `5.0` | Per-request socket timeout, seconds |
| `RAFTKV_REPLICATION_TIMEOUT` | `5.0` | Deadline for a write to become readable on a node |
| `RAFTKV_LEADER_TIMEOUT` | `30.0` | Deadline for leader discovery (tolerates an election in progress) |
| `RAFTKV_POLL_INTERVAL` | `0.1` | Poll interval, seconds |

`test_crash.py` only (all ignored by the default run):

| Env var | Default | Meaning |
|---|---|---|
| `RAFTKV_COMPOSE_FILE` | `<repo>/docker-compose.yml` | Compose file identifying the project whose container gets killed |
| `RAFTKV_CRASH_KEYS` | `25` | How many keys to write before the kill |
| `RAFTKV_RESTART_TIMEOUT` | `90.0` | Deadline for the restarted node to answer a read again |
| `RAFTKV_DOCKER_TIMEOUT` | `60.0` | Budget for one `docker` CLI invocation |

`test_snapshot.py` only (also ignored by the default run):

| Env var | Default | Meaning |
|---|---|---|
| `RAFTKV_COMPOSE_FILES` | `<repo>/docker-compose.yml,<repo>/docker-compose.test.yml` | Comma-separated compose files, in `-f` order. Must match what the cluster was started with, or a restarted service loses the override |
| `RAFTKV_SNAPSHOT_KEYS` | `60` | Keys written to push the cluster past the snapshot threshold |
| `RAFTKV_COMPACTION_TIMEOUT` | `120.0` | Deadline for every node to snapshot *and* truncate |
| `RAFTKV_CATCHUP_TIMEOUT` | `180.0` | Deadline for a wiped or restarted node to serve every key again |
| `RAFTKV_DOCKER_POLL_INTERVAL` | `1.0` | Poll interval for `/status` and log probes (each costs a `docker compose exec`) |
| `RAFTKV_MAX_SNAPSHOT_THRESHOLD` | `512` | Above this, the cluster counts as "not configured for these tests" and they skip |
| `RAFTKV_MAX_TRAILING_LOGS` | `512` | Same, for `TrailingLogs` |
| `RAFTKV_RESTART_TAIL_SLACK` | `8` | Extra applies tolerated above the expected tail on restart |

```bash
RAFTKV_NODES=http://node1:8080,http://node2:8080,http://node3:8080 pytest tests/e2e -v
```

## The contract under test

`contracts.py` is the single source of truth for these values, copied from
`cpp-app/src/network/http_server.hpp`. Changing the handler means changing that
file in the same commit.

**`POST /insert-val`**

| Outcome | Status | Body (`application/json`) |
|---|---|---|
| Committed | `200 OK` | `{"ok":true}` |
| This node is not the leader | `503 Service Unavailable` | `{"error":"not leader","leader":"node1:8088"}` |
| Propose failed some other way | `502 Bad Gateway` | `{"error":"<reason>"}` |
| `Content-Type` is not `application/msgpack` | `415 Unsupported Media Type` | `{"error":"unsupported media type","expected":"application/msgpack"}` |
| Empty body | `400 Bad Request` | `{"error":"empty request body"}` |
| Unparseable `Content-Length` | `400 Bad Request` | `{"error":"malformed Content-Length"}` |

**`GET /get-val?key=...`**

| Outcome | Status | Body |
|---|---|---|
| Key present | `200 OK` | the stored value verbatim, `text/plain; charset=utf-8` |
| Key absent | `404 Not Found` | `{"error":"key not found"}` (JSON) |
| No `key` query parameter | `400 Bad Request` | `{"error":"missing required query parameter: key"}` (JSON) |

Anything else: `404 Not Found`, `{"error":"not found"}`.

The `leader` field is the peer's **Raft** address (`<node-id>:8088` under
`docker-compose.yml`), not an HTTP endpoint — the tests assert its shape, not a
literal value.

## How it works

- **No fixed sleeps.** Every wait goes through `wait_until(predicate, timeout, interval)`
  in `conftest.py`, replacing the `time.sleep(0.3)` in `test_client.py`.
- **Leader discovery by probing.** The management API (`:6000/status`) is not
  published to the host in `docker-compose.yml`, so the suite cannot ask who
  leads. It POSTs a harmless SET to each node instead: the leader answers
  `200 {"ok":true}` and a follower answers `503 {"error":"not leader",...}`.
  The probe keys off the *success* shape rather than "not a 503", so a node
  whose sidecar is down (502) is never mistaken for a leader. The result is
  cached for the session and the probe is retried until `RAFTKV_LEADER_TIMEOUT`.
- **Everything hangs off `ClusterClient`.** `test_cluster.py` imports only
  `contracts` and pytest fixtures — never `conftest` by name, which would be
  fragile once a second `conftest.py` exists in the tree. Request helpers,
  JSON decoding, polling and the raw-socket escape hatch are all methods on the
  `cluster` fixture. `test_crash.py` follows the same rule, and additionally
  imports `assert_write_accepted` from `test_cluster` so the committed-write
  contract (200 + JSON + `{"ok":true}`) is asserted in exactly one place.
  `test_snapshot.py` does the same and additionally imports the docker plumbing
  from `test_crash.py` rather than growing a second copy of it.
- **`conftest.py` stays docker-free.** The compose wrapper, the skip
  conditions and the on-disk format checks live in `test_crash.py`; the pieces
  the snapshot scenarios need on top — compose across two `-f` files, reading
  `:6000/status` through `docker compose exec`, listing and emptying a data
  directory from inside a container — live in `snapshot_support.py`, a plain
  module next to the tests in the same way as `contracts.py`. Neither is
  imported by anything that runs in the default suite.
- **Raw sockets where `requests` cannot go.** `requests` derives
  `Content-Length` from the body, so the R1.8 regression test hand-builds the
  request bytes and reads the response off the socket itself.
- **Unique keys.** Every key carries a per-run and per-test `uuid4` suffix, so
  the suite is idempotent against a long-lived cluster and repeated runs never
  collide. Nothing is cleaned up afterwards — expect a handful of
  `e2e-*` keys to accumulate on a cluster you keep alive across many runs.

## The R1.8 regression test

`test_r1_8_malformed_content_length_is_rejected_without_killing_the_node` is the
highest-value test here: it is the only one whose failure means an
unauthenticated stranger can take the cluster down.

`HttpRequestParser::parse` used to call `std::stoi` on the `Content-Length`
value. On garbage it threw, the exception unwound out of
`HttpServer::handle_connection` and `run()` into `main.cpp`'s catch-all, which
printed `Fatal error: stoi` and exited 1; `entrypoint.sh` exits when either
process dies and `docker-compose.yml` declares no `restart:` policy, so the
container stayed down.

The test sends `Content-Length: abc` to **every** node, asserts a
`400 {"error":"malformed Content-Length"}` — the message, not just the code,
because dropping the flag would fall through to the empty-body branch and still
produce a 400 — then asserts the node still serves reads, and finally that a
write on the leader still replicates everywhere.

## Still-pinned behavior

Phase 1 flipped the pins on error reporting. These remain deliberately pinned;
do not "fix" the tests without changing the product in the same phase:

| Test | Pinned contract | Changed in |
|---|---|---|
| R0.4 | a write to a follower is **rejected** (503 + a leader hint the client must act on) rather than forwarded | Phase 4 (leader forwarding) |
| R0.1 / R0.2 | reads are polled because they are served from the local store with no read-index check — a follower may legitimately be stale, and there is no linearizable read mode to ask for | Phase 4 (consistency modes) |
| R0.2 | a deleted key and a never-written key are the same 404 — the store has no tombstones | — |

## Flipped in Phase 1

For the record, so a reader of the git history knows these were intentional:

| Test | Was (Phase 0 pin) | Is now |
|---|---|---|
| R0.3 | `200` + body `Key Not Found` | `404` + `{"error":"key not found"}` |
| R0.4 | `200` + body `error` | `503` + `{"error":"not leader","leader":"..."}` |
| R0.1 / R0.2 | `200` + body `ok` | `200` + `{"ok":true}` |
