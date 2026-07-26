# RaftKV end-to-end suite

Asserting replacement for `test_client.py`. Covers spec R0.1–R0.5: a write on the
leader replicates to **all** nodes, DELETE round-trips, a missing key reads back
as `Key Not Found`, and a write to a follower is rejected.

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
the cluster is not up.

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

## How it works

- **No fixed sleeps.** Every wait goes through `wait_until(predicate, timeout, interval)`
  in `conftest.py`, replacing the `time.sleep(0.3)` in `test_client.py`.
- **Leader discovery by probing.** The management API (`:6000/status`) is not
  published to the host in `docker-compose.yml`, so the suite cannot ask who
  leads. It POSTs a harmless SET to each node instead: only the leader answers
  `ok`, followers answer `error` because there is no leader forwarding. The
  result is cached for the session and the probe is retried until
  `RAFTKV_LEADER_TIMEOUT`.
- **Unique keys.** Every key carries a per-run and per-test `uuid4` suffix, so
  the suite is idempotent against a long-lived cluster and repeated runs never
  collide. Nothing is cleaned up afterwards — expect a handful of
  `e2e-*` keys to accumulate on a cluster you keep alive across many runs.

## Pinned-buggy assertions

Phase 0 is a safety net, not a fix. These assertions deliberately encode current
misbehavior, each commented with the phase that will change it:

| Test | Pinned contract | Changed in |
|---|---|---|
| R0.3 | missing key ⇒ HTTP **200** + body `Key Not Found` | Phase 1 (real 404) |
| R0.4 | write to follower ⇒ HTTP **200** + body `error` | Phase 1 (status code), Phase 4 (forwarding) |

Do not "fix" these tests without changing the product in the same phase.
