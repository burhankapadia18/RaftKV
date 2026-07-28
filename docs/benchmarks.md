# Benchmarks

**These are local measurements on a laptop, not published performance figures.**

Everything below was produced on an Apple M2 running Docker Desktop, with all
three "nodes" as containers on one machine sharing 8 vCPUs and a 4 GiB VM. That
arrangement gets several things wrong at once compared with a real deployment:
replication has no network latency, every node competes for the same CPUs and the
same disk, and `fsync` goes through a virtualised filesystem. Use these numbers to
understand the *shape* of the system — writes cost roughly ten times a local read,
linearizable reads cost roughly a fifth of a write, throughput saturates at low
concurrency — and not as figures to quote.

Reproduce them yourself with one command; that is the point of checking the tool
in rather than the results.

## Method

```bash
docker build -t raftkv:latest .
export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
docker compose up -d                       # 3-node; `up -d node1` for 1-node

cd bench
go run ./cmd/kvbench -workload all -clients 32 -duration 20s -warmup 5s \
  -keyspace 5000 -value-size 128 -label "3-node cluster"
```

`kvbench` prints a markdown table on stdout and the same data as JSON on stderr,
so a run can be pasted here or diffed against another.

What it does, and why:

- **Seeds the whole keyspace first**, before any workload. A read benchmark
  against keys that do not exist measures the 404 path, which is cheaper than a
  real read and would flatter the read numbers considerably.
- **Warms up without recording.** The first requests pay connection setup and any
  lazily-initialised server state; including them produces a p99 that describes
  startup rather than steady state.
- **Records every sample** and takes exact quantiles rather than a streaming
  estimate. 8 bytes per request is nothing, and the tail is the interesting part.
- **Counts failed requests but does not time them into the distribution.** A
  connection refused in 200 µs would *improve* the p50, which is backwards.
- **Picks a node per request, not per worker.** Pinning a worker to a node lets a
  slow node throttle only its own worker while the others make up the throughput,
  which hides it in the average.
- **Opens a fresh connection for every request.** Not a choice: the server does
  not implement keep-alive, so a pooled connection is already closed by the time
  the next request gets it. A TCP handshake is therefore inside every measurement
  below. That is a real cost of this deployment, not benchmark overhead, and it is
  not subtracted.

### Reference machine

| | |
|---|---|
| CPU | Apple M2, 8 cores |
| RAM | 16 GiB host; Docker VM limited to 8 vCPU / ~4 GiB |
| Docker | 29.2.0 (Docker Desktop, arm64) |
| Storage | APFS on NVMe, via the Docker Desktop VM |
| Cluster | 3 containers on one host, `docker-compose.yml` defaults |
| Commit | `24374ae` |

### Run-to-run variance

The same 3-node write configuration produced **1523, 1930 and 1962 ops/s** on
three separate runs — roughly ±25%. Every figure here should be read with that
band around it, and a difference of less than about a third between two rows is
noise, not a finding. Latency percentiles moved similarly (p50 15.1–17.2 ms).

That variance is itself a property of the environment rather than of the store:
three nodes contending for eight shared vCPUs and one virtualised disk is not a
quiet system.

## Results

### 3-node cluster — 32 clients, 128-byte values, 5000 keys, 20s per workload

| Workload | Throughput (ops/s) | p50 | p95 | p99 | Errors |
|---|---:|---:|---:|---:|---:|
| write | 1930 | 15.5ms | 26.3ms | 31.6ms | 0 |
| read-local | 33289 | 0.33ms | 0.92ms | 1.8ms | 0 |
| read-linearizable | 4618 | 6.4ms | 11.4ms | 13.9ms | 0 |
| mixed (90% read) | 13865 | 0.74ms | 12.4ms | 15.7ms | 0 |

### 1 node, no replication — same parameters

| Workload | Throughput (ops/s) | p50 | p95 | p99 | Errors |
|---|---:|---:|---:|---:|---:|
| write | 3249 | 9.5ms | 12.2ms | 15.6ms | 0 |
| read-local | 30233 | 0.29ms | 0.65ms | 1.7ms | 0 |
| read-linearizable | 5281 | 5.8ms | 7.7ms | 9.0ms | 0 |
| mixed (90% read) | 16981 | 0.40ms | 3.6ms | 4.9ms | 0 |

### Value size — writes only, 3-node, 32 clients

| Value size | Throughput (ops/s) | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| 128 B | 1962 | 15.1ms | 26.1ms | 31.8ms |
| 1 KiB | 1727 | 17.5ms | 29.4ms | 36.3ms |
| 8 KiB | 714 | 40.0ms | 86.9ms | 112.1ms |

### Concurrency — writes only, 3-node, 128-byte values

| Clients | Throughput (ops/s) | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| 1 | 518 | 1.9ms | 2.4ms | 3.5ms |
| 8 | 1239 | 6.3ms | 8.6ms | 10.4ms |
| 32 | 1523 | 17.2ms | 29.7ms | 35.8ms |
| 128 | 1537 | 87.2ms | 159.1ms | 189.6ms |

## What the numbers say

**Writes saturate at low concurrency.** Throughput is flat from 32 clients to 128
— 1523 to 1537 ops/s — while p50 latency grows 5×, from 17 ms to 87 ms. The system
is already at capacity by 32 concurrent writers; everything past that is queueing.
Anyone tuning a client should note that raising concurrency beyond this point buys
nothing and costs latency proportionally.

**A local read is ~50× cheaper than a write, and a linearizable read sits between
them.** 33k local reads/s against 1.9k writes/s, with linearizable reads at 4.6k.
That ordering is exactly what the design predicts: a local read touches an
in-memory map, a linearizable read costs a `Barrier` plus a quorum check, and a
write costs a full raft round trip plus a WAL `fsync` on every replica. The
consistency choice Phase 4 exposed is worth roughly 7× on reads, which is the
number to weigh when deciding whether a given read needs to be fresh.

**Replication costs about 40% of write throughput** (3249 → 1930 ops/s), and *not*
because of network latency, which is nil here. The cost is the leader waiting for
a quorum of followers to fsync their own WALs, on the same contended disk. On real
hardware with a real network the ratio would differ; the direction would not.

**Large values hit the WAL, not the network.** 8 KiB values drop writes to 714
ops/s — a 2.7× fall for a 64× size increase, so the cost is sub-linear but real.
Each write appends the whole value to `kv.wal` and fsyncs before acknowledging, so
value size feeds directly into the durability path. Worth knowing before storing
large blobs in a store whose durability guarantee is per-write.

**Zero errors across every run**, including 128 concurrent writers, which is the
result the Phase 4 thread pool and the Phase 1 error contract were meant to
produce. It is also the least interesting number here: correctness under load is
the chaos harness's job, not this tool's.

## What is deliberately not measured

- **No cross-machine deployment.** Every number here has zero network latency
  between replicas, which is the single biggest difference from production.
- **No sustained/soak run.** Twenty seconds per workload says nothing about
  behaviour once the WAL has grown and compaction is running steadily.
- **No comparison against etcd, Consul or anything else.** A benchmark of this
  system on a laptop against someone else's tuned numbers would be worse than no
  comparison.
- **No latency under fault.** Throughput during an election or a snapshot
  transfer is not covered; [the chaos harness](../tests/chaos/README.md) exercises
  those paths for correctness, not for speed.
