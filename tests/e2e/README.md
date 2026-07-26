# RaftKV end-to-end suite

Asserting replacement for `test_client.py`. Two families of test:

- **R0.1–R0.5** (Phase 0) — a write on the leader replicates to **all** nodes,
  DELETE round-trips, a missing key is reported as missing, and a write to a
  follower is rejected.
- **R1.7/R1.8** (Phase 1) — the truthful-error HTTP contract: real status codes,
  correct reason phrases, JSON error bodies, and a regression test for the
  remote denial of service that a malformed `Content-Length` used to cause.

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
pytest tests/e2e -v
```

Exit status is pytest's: non-zero on any failure. If no node accepts a write
within the leader-discovery deadline, the suite fails with a message telling you
the cluster is not up and what each node answered instead.

## Configuration

All optional; every value has a working default for `docker-compose.yml`.

| Env var | Default | Meaning |
|---|---|---|
| `RAFTKV_NODES` | `http://localhost:8080,http://localhost:8081,http://localhost:8082` | Comma-separated node base URLs |
| `RAFTKV_HTTP_TIMEOUT` | `5.0` | Per-request socket timeout, seconds |
| `RAFTKV_REPLICATION_TIMEOUT` | `5.0` | Deadline for a write to become readable on a node |
| `RAFTKV_LEADER_TIMEOUT` | `30.0` | Deadline for leader discovery (tolerates an election in progress) |
| `RAFTKV_POLL_INTERVAL` | `0.1` | Poll interval, seconds |

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
  `cluster` fixture.
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
