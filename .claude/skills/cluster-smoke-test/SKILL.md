---
name: cluster-smoke-test
description: Build the RaftKV image, launch the 3-node docker-compose cluster, verify replication end-to-end with the MsgPack test client, and tear down. Use after any change to cpp-app, go-sidecar, proto, Dockerfile, or entrypoint.sh to confirm the system still works.
---

# RaftKV Cluster Smoke Test

Verify a change end-to-end against a real 3-node cluster. This is the project's primary verification method (there is no unit test suite).

## Steps

1. **Build** (from repo root):
   ```bash
   docker build -t raftkv:latest .
   ```
   Both the Go sidecar and the C++ app compile inside the image — a build failure here catches compile errors in either language.

2. **Start clean.** Stale raft state causes confusing membership errors:
   ```bash
   docker-compose down -v 2>/dev/null; rm -rf vol-node1 vol-node2 vol-node3
   docker-compose up -d
   ```

3. **Wait for the cluster to form** (leader election + joins take a few seconds). Confirm via logs:
   ```bash
   docker-compose logs | grep -E "entering leader state|Successfully joined"
   ```
   Expect node1 to become leader and node2/node3 to log a successful join.

4. **Run the write/read test**:
   ```bash
   python3 test_client.py    # needs: pip install requests msgpack
   ```
   Expect `Response Body: ok` and `GET Verification ... msgpack_optimization_active`.

5. **Verify replication on all nodes** — the critical check:
   - Every node's log must show `[StateMachine] Applied: SET user_123`:
     ```bash
     docker-compose logs | grep "Applied"
     ```
   - Followers must serve the read (node2/node3 map to host ports 8081/8082):
     ```bash
     curl "http://localhost:8081/get-val?key=user_123"
     curl "http://localhost:8082/get-val?key=user_123"
     ```

6. **Check write-to-follower behavior** (should return `error`, not crash):
   ```bash
   python3 -c "
   import requests, msgpack
   d = msgpack.packb({'op':'SET','key':'x','value':'y'})
   print(requests.post('http://localhost:8081/insert-val', data=d,
         headers={'Content-Type':'application/msgpack'}).text)"
   ```

7. **Tear down**:
   ```bash
   docker-compose down
   ```

## Interpreting failures

- `error` from the write on port 8080 → node1 is not leader or raft.Apply timed out; check `docker-compose logs node1` for election churn.
- Write ok but follower read misses → replication/apply problem; look for `ERROR: Failed to apply to C++ DB` in sidecar logs (the C++ gRPC server may not be up on that node).
- Nodes restart-looping → `entrypoint.sh` exits when either process dies; the first crashing process is the culprit — check the earliest log lines.
