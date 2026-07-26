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
   Four tests, all asserting (nothing to eyeball):

   | Test | Asserts |
   |---|---|
   | R0.1 | SET on the leader is readable on **all three** nodes, polled against a deadline |
   | R0.2 | DELETE round-trip: gone on all three nodes |
   | R0.3 | GET of a never-written key returns `Key Not Found` on every node |
   | R0.4 | SET sent to a **follower** is rejected (`error`) and reaches no node's store |

   The suite discovers the leader by probing (only the leader accepts a write),
   uses unique per-run keys, and never touches docker itself. Exit status is
   pytest's: non-zero on any failure. R0.4 replaces the old manual
   write-to-follower curl check — don't run that separately.

   Point the suite at a non-default topology with
   `RAFTKV_NODES=http://host:8080,...`; other knobs are listed in
   `tests/e2e/README.md`.

5. **Optional cross-check on the logs.** The suite already proves replication
   via HTTP reads on every node; this only helps when you are diagnosing a
   failure:
   ```bash
   docker compose logs | grep "Applied"
   ```
   Every node should show `[StateMachine] Applied:` lines for the e2e keys.

6. **Tear down**:
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
- **R0.4 fails with a follower answering `ok`** → leadership moved off the
  discovered leader mid-run (the message says so). Re-run; if it persists, the
  cluster is churning leaders.
- **R0.3 fails** → a stale key survived from a previous run, or the C++ handler's
  miss path changed. Keys are per-run unique, so this normally means real
  behavior drift.
- **Nodes restart-looping** → `entrypoint.sh` exits when either process dies; the
  first crashing process is the culprit — check the earliest log lines.
- After any failure, `docker compose logs --no-color --timestamps` is what CI
  dumps; do the same before tearing down.
