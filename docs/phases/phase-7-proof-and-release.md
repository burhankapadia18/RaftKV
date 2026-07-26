# Phase 7 — Proof and Release (v1.0)

## Spec

### Goals
Prove the guarantees the previous phases built — with chaos testing and published
benchmarks — and cut a releasable v1.0: versioned images, complete docs, changelog.

### Non-goals
- No performance optimization work beyond what benchmarking reveals as outright broken
  (fixes discovered here get their own scoped tasks).
- No Jepsen-grade formal verification — the chaos harness is docker-level fault
  injection, honestly documented as such.

### Requirements

**Chaos harness (`tests/chaos/`):**
- R7.1 A load generator (`load.py`): N concurrent clients issuing PUT/GET/DELETE with
  a deterministic keyspace, recording every **acknowledged** write (key, value, ack time)
  to a journal file.
- R7.2 A fault injector (`chaos.py`) running scenarios against the compose cluster:
  - kill -9 the leader (repeatedly, random intervals)
  - kill -9 a random follower
  - `docker pause`/`unpause` a node (simulated freeze/partition)
  - wipe a follower volume and rejoin (exercises Phase 3 restore under load)
- R7.3 Invariant checker: after each scenario and at the end — every journaled
  acknowledged write is readable (linearizable read) on the cluster, and all three
  nodes converge to identical state (dump + compare). Any violation fails the run
  with the offending key and timeline.
- R7.4 CI: a 10-minute chaos run as a nightly/scheduled workflow (not per-PR);
  per-PR keeps the fast e2e suite.

**Benchmarks (`bench/`):**
- R7.5 A benchmark client (Go, `bench/cmd/kvbench`) measuring: write throughput and
  p50/p95/p99 latency; local-read and linearizable-read throughput/latency; mixed
  workloads; against 1-node and 3-node clusters. Parameters (clients, duration,
  value size) as flags; output as a markdown table.
- R7.6 Results published in `docs/benchmarks.md` with exact methodology (hardware,
  versions, commands) — honest numbers, no cherry-picking; README links to it with
  headline figures.

**Docs & release:**
- R7.7 `docs/architecture.md`: the final design — both data paths, snapshot lifecycle,
  consistency modes, security model, port map. README slimmed to quickstart + API +
  links; API reference verified against the Phase 4 surface.
- R7.8 `CHANGELOG.md` (Keep-a-Changelog format) reconstructing phases 0–7 as the
  v1.0.0 entry.
- R7.9 Release workflow (`.github/workflows/release.yml`): on tag `v*` — run full CI,
  build multi-arch images (linux/amd64, linux/arm64 via buildx), push to GHCR
  (`ghcr.io/<owner>/raftkv`), create a GitHub release with notes. Compose file
  switches to the published image with a local-build override for development.
- R7.10 CLAUDE.md fully reconciled: the "Known Limitations" section lists only what
  genuinely remains (e.g., no client SDK, proxy-terminated TLS, single-raft-group
  scalability ceiling).

### Acceptance criteria
- Three consecutive nightly chaos runs pass with zero invariant violations.
- `docs/benchmarks.md` exists with reproducible numbers; `kvbench` runs from a clean
  checkout with one command.
- `git tag v1.0.0 && git push --tags` produces green CI, pushed multi-arch images, and
  a GitHub release; `docker compose up` with the published image passes the e2e suite.
- A newcomer can go from README to a verified running cluster without reading source.

## Plan

1. **Chaos**: build `tests/chaos/load.py` (journaling load generator) and `chaos.py`
   (scenario runner + invariant checker) on top of the e2e helpers from Phase 0;
   run each scenario locally until stable; wire the nightly workflow (R7.4).
2. **Triage**: fix whatever chaos finds (expected: shutdown races, forwarding retry
   gaps). Each finding = failing regression test first, then fix, per repo TDD rules.
3. **Bench**: implement `bench/cmd/kvbench` (Go module inside `go-sidecar` workspace
   or standalone `bench/go.mod`); run the matrix on a documented machine; write
   `docs/benchmarks.md` (R7.5–R7.6).
4. **Docs**: write `docs/architecture.md`; restructure README; write `CHANGELOG.md`
   (R7.7–R7.8).
5. **Release automation**: `release.yml` with buildx + GHCR push + release notes;
   dry-run with a `v1.0.0-rc1` tag; fix, then tag `v1.0.0` (R7.9).
6. **Final reconciliation**: CLAUDE.md, `.claude/rules/*.md`, and skills
   (`cluster-smoke-test`) updated to the final commands and truthful limitations (R7.10).

### Verification
- Nightly chaos workflow green three nights running.
- `kvbench` reproduces published numbers within noise on the reference machine.
- rc-tag dry run end-to-end; final `v1.0.0` release artifacts pulled and smoke-tested
  from GHCR on both architectures.
