---
name: cluster-smoke-test
description: Build the RaftKV image, launch the 3-node docker compose cluster, run the asserting pytest end-to-end suite against it, and tear down. Use after any change to cpp-app, go-sidecar, proto, Dockerfile, or entrypoint.sh to confirm the system still works.
---

# RaftKV Cluster Smoke Test

Verify a change end-to-end against a real 3-node cluster. This is the project's
system-level verification; the unit layers (`go test -race ./...` and
`ctest --test-dir cpp-app/build`) are separate and should already be green
before you get here.

Use `docker compose` (the CLI plugin) throughout — the standalone
`docker-compose` binary is not guaranteed to be installed.

## Steps

1. **Build** (from repo root):
   ```bash
   docker build -t raftkv:latest .
   ```
   Both the Go sidecar and the C++ app compile inside the image — a build failure here catches compile errors in either language.

2. **Start clean.** Stale raft state causes confusing membership errors:
   ```bash
   docker compose down -v 2>/dev/null; rm -rf vol-node1 vol-node2 vol-node3
   docker compose up -d
   ```

3. **Wait for the cluster to form** (leader election + joins take a few seconds). Confirm via logs:
   ```bash
   docker compose logs | grep -E "entering leader state|Successfully joined"
   ```
   Expect node1 to become leader and node2/node3 to log a successful join.

4. **Run the end-to-end suite** — this is the whole verification:
   ```bash
   pip install -r tests/e2e/requirements.txt   # once
   pytest tests/e2e -v
   ```
   ~28 tests, all asserting (nothing to eyeball). The headline ones:

   | Area | Asserts |
   |---|---|
   | Replication | A write is readable on **all three** nodes, polled against a deadline |
   | DELETE | Round-trip: gone on all three nodes |
   | Miss | A never-written key returns 404 with `{"error":"key not found"}` |
   | Forwarding | A write to a **follower** is accepted (200) and replicated — it is relayed to the leader, not refused |
   | Status contract | The full table: 400/404/405/413/415/431/502/503, bodies included |
   | Hardening | A malformed `Content-Length` (including `-1`) no longer kills a node |
   | Security | Request caps return 413/431 and the node survives a burst; `/join` is closed without a token, and a refused join does not move `last_log_index` |

   **Note the forwarding row**: before Phase 4 a follower write was *rejected*, and
   this skill said so. It is now accepted. If you see a 503 from a follower, that
   means no leader is available — not that forwarding is missing.

   The suite discovers the leader by probing `/status`, uses unique per-run keys,
   and never touches docker itself. Exit status is pytest's: non-zero on any
   failure.

   Point the suite at a non-default topology with
   `RAFTKV_NODES=http://host:8080,...`; other knobs are listed in
   `tests/e2e/README.md`.

5. **Run the crash-recovery test** (Phase 2) — deselected from the run above by
   `pytest.ini`, because it is the one test that touches docker: it `SIGKILL`s a
   follower, asserts the acknowledged writes are already in that node's
   `kv.db`/`kv.wal` *while it is dead*, then restarts it and reads them back.

   ```bash
   pytest tests/e2e -m requires_docker -v -rs
   ```

   Run it after the suite above; it disturbs the cluster (one node is briefly
   down, then rejoins). `-rs` matters: the test **skips** rather than fails when
   it cannot do its job — no docker CLI, no compose project, or `RAFTKV_NODES`
   pointing somewhere other than localhost — and a silent skip would look like a
   pass. It always attempts the restart even when an assertion fails; if a run
   is interrupted between the kill and the restart, bring the node back with
   `docker compose start node2`.

6. **The snapshot scenarios need the test override**, which lowers the snapshot
   tunables far enough that one happens inside a test run. Without it they skip:
   ```bash
   docker compose down -v && rm -rf vol-node1 vol-node2 vol-node3
   docker compose -f docker-compose.yml -f docker-compose.test.yml up -d
   pytest tests/e2e -m requires_docker -v -rs
   ```
   Read the `-rs` output. A skip here means Phase 3's headline requirement went
   unverified, and that has already happened silently for two phases.

7. **The TLS profile** (Phase 6), if the change touches security, transport or
   compose:
   ```bash
   ./scripts/gen-certs.sh
   export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
   docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d
   pytest tests/e2e -m requires_secure -v -rs
   ```

8. **Optional cross-check on the logs.** The suite already proves replication via
   HTTP reads on every node; this only helps when diagnosing a failure. Both
   processes emit JSON lines with a shared schema, so filter rather than grep:
   ```bash
   docker compose logs --no-color | grep '"level":"error"'
   docker compose logs --no-color node1 | grep '"component":"state_machine"'
   ```
   Successful applies are logged at **debug**, which the containers do not emit —
   use `curl localhost:8080/metrics | grep raftkv_apply_total` instead of looking
   for an "Applied" line. There is no longer any `[StateMachine] Applied:` text;
   Phase 5 replaced the bracketed prefixes with JSON.

9. **Tear down**:
   ```bash
   docker compose down -v
   ```

## Interpreting failures

- **`No node accepted a write within 30s -- the RaftKV cluster does not look ready`**
  (suite exits 1 during leader discovery) → no cluster is running, it is still
  electing, or the published ports differ from
  `localhost:8080/8081/8082`. The message lists what each node answered. Check
  `docker compose ps` and `docker compose logs node1` for election churn.
- **R0.1/R0.2 fail with "did not replicate to every node"** → the write was
  accepted by the leader but some node never applied it. The failure message
  names the node and the last body it served. Look for
  `ERROR: Failed to apply to C++ DB` in that node's sidecar logs (its C++ gRPC
  server on :50051 may not be up).
- **A follower write returns 503** → no leader is available (an election, or quorum
  lost). Forwarding exists; 503 is the retryable answer, 502 is not. Re-run; if it
  persists, the cluster is churning leaders.
- **A miss test fails** → a stale key survived from a previous run, or the handler's
  miss path changed. Keys are per-run unique, so this normally means real drift.
- **A read returns 404 right after a 200 write** → that is correct for a default
  read on a follower, and every test that cares uses
  `?consistency=linearizable`. If a *test* trips on it, the test is missing that
  parameter.
- **Nodes restart-looping** → `entrypoint.sh` exits when either process dies; the
  first crashing process is the culprit — check the earliest log lines.
- After any failure, `docker compose logs --no-color --timestamps` is what CI
  dumps; do the same before tearing down.

## Beyond the smoke test

This skill covers correctness of a change. Two heavier tools exist and are not
part of it:

- `python tests/chaos/chaos.py --duration 600` — fault injection under load
  (kill the leader, freeze a node, wipe a disk) with an invariant checker. Reach
  for it when a change touches replication, recovery, snapshots or shutdown.
- `cd bench && go run ./cmd/kvbench -workload all` — throughput and latency. Note
  that run-to-run variance on a laptop is about ±25%, so a single run cannot
  demonstrate a performance change.
