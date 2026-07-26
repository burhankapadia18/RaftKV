# Phase 5 — Operability

## Spec

### Goals
Make the cluster observable and its lifecycle graceful: structured logs, Prometheus
metrics from both processes, health endpoints that tell the truth, and clean shutdown
with leadership handoff.

### Non-goals
- No distributed tracing (out of scope for 1.0).
- No Grafana dashboards shipped in-repo (metrics surface only; a sample dashboard JSON
  may land in Phase 7 docs).

### Requirements

**Structured logging:**
- R5.1 Go: replace stdlib `log` with `log/slog` JSON handler; every logger carries
  `node_id` and `component` (fsm, rpc, mgmt, joiner, backend) attributes. `log.Fatalf`
  remains only in `cmd/sidecar/main.go` per repo rules.
- R5.2 C++: new `cpp-app/src/common/log.hpp` — minimal leveled JSON-lines logger to
  stdout (`{"ts","level","component","msg",...}`), no external deps. All existing
  `std::cout`/`std::cerr` call sites migrate. Log level from `Config`.

**Metrics:**
- R5.3 Go: `promhttp` on the management server at `/metrics`. Exported: HashiCorp
  `raft.Stats()` gauges (state, term, last/first log index, last snapshot index),
  apply latency histogram + apply error counter (instrumented in `CppFSM.Apply`),
  propose counter by outcome (`ok|not_leader|forwarded|error`) in `rpc.Server`.
- R5.4 C++: `GET /metrics` on the HTTP server serving Prometheus text exposition from
  a tiny hand-rolled registry (`common/metrics.hpp` — atomic counters + fixed-bucket
  histograms; no dependency). Exported: request count/latency by route and status class,
  store key count, WAL size bytes, apply count/latency.

**Health:**
- R5.5 Management `/health` stays pure liveness (process up). New `/ready` returns 200
  only when: raft state is Leader or Follower (not Candidate/Shutdown), a leader is
  known, and the C++ backend answered a probe within the last 10s. Body is JSON listing
  each check.
- R5.6 `/status` rewritten with `encoding/json` (struct + `json.Marshal`), extended
  with term, indexes, and snapshot info (already needed by Phase 3 tests).

**Lifecycle:**
- R5.7 `entrypoint.sh`: `trap` SIGTERM/SIGINT, forward the signal to both children,
  `wait` for both; container stop is graceful, not a kill.
- R5.8 Go shutdown path in `cmd/sidecar/main.go`: on SIGTERM — if leader, attempt
  `raft.LeadershipTransfer()` (bounded 5s); then stop gRPC (`GracefulStop`), management
  server (`Shutdown(ctx)`), and `raft.Shutdown()`.
- R5.9 C++ shutdown: SIGTERM handler sets the HTTP stop flag (Phase 4's graceful stop)
  and calls `StateMachineServer::shutdown()`; main exits cleanly after both join.
- R5.10 `docker-compose.yml`: drop obsolete `version:` key; per node
  `healthcheck` (curl `http://localhost:6000/ready`), `restart: unless-stopped`;
  node2/node3 `depends_on: node1: condition: service_healthy`.

### Acceptance criteria
- `docker compose stop node-with-leadership` → logs show leadership transfer, another
  node is leader **before** the old one exits, and in-flight e2e writes continue with
  only transient errors (asserted by extending `test_failover.py`).
- `curl :6000/metrics` (Go) and `curl :8080/metrics` (C++) both scrape cleanly with
  `promtool check metrics` (run in CI e2e job).
- `/ready` returns 503 during startup and during a lost-quorum scenario (stop two
  nodes; the survivor's `/ready` must fail) — e2e asserted.
- All logs from a running cluster parse as JSON lines (e2e spot check).

## Plan

1. **Go logging**: introduce a `newLogger(nodeID, component)` helper (small
   `internal/logging` package); migrate packages one by one; silence-check with
   `go vet` + tests.
2. **C++ logging**: `common/log.hpp` + registration in `KVDB_HEADERS`; migrate call
   sites in `http_server.hpp`, `state_machine.hpp`, `raft_client.hpp`, `main.cpp`.
3. **Go metrics**: add `prometheus/client_golang`; `internal/metrics` package with the
   collectors; instrument `fsm`, `rpc`; mount `promhttp` in `management`. Unit test:
   scrape the handler, assert key metric names present.
4. **C++ metrics**: `common/metrics.hpp` (unit-tested formatting) + `/metrics` route
   in the HTTP handler + instrumentation of request/apply paths.
5. **Health**: `/ready` checks + JSON `/status` in `internal/management` (backend
   probe via the existing `StateMachineClient`); handler unit tests for each failure
   combination.
6. **Lifecycle**: signal handling in `entrypoint.sh`, `cmd/sidecar/main.go`, and
   `cpp-app/src/main.cpp` (R5.7–R5.9); compose healthchecks/restart/depends_on (R5.10).
7. **Tests/docs**: extend `test_failover.py` (graceful-stop scenario), add
   `test_observability.py` (metrics scrape + ready-state transitions); README gains an
   Operations section (endpoints, metrics list, shutdown semantics); CLAUDE.md port
   table gains `/metrics`,`/ready`.

### Verification
- Unit suites green; `promtool check metrics` in CI.
- Manual: `docker compose stop` the leader while `watch curl /status` on a follower —
  observe transfer; `docker compose logs | jq .` parses.
- `/cluster-smoke-test` green.
