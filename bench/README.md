# kvbench

Throughput and latency for a running RaftKV cluster (R7.5).

```bash
docker compose up -d
cd bench
go run ./cmd/kvbench -workload all -clients 32 -duration 20s -label "3-node"
```

Results and methodology: [docs/benchmarks.md](../docs/benchmarks.md).

A standalone Go module with **no third-party dependencies** — net/http is enough —
so it builds from a clean checkout with no network access. It is not part of
`go-sidecar` on purpose: it is a client, shares no code with the sidecar, and
folding it in would put a benchmark tool in the path of `go test ./...` for the
binary that ships in the image.

## Flags

| Flag | Default | Notes |
|---|---|---|
| `-nodes` | all three localhost ports | Requests are spread across every node, one chosen per request |
| `-workload` | `all` | `write`, `read-local`, `read-linearizable`, `mixed`, `all` |
| `-clients` | 32 | Concurrent goroutines. Writes saturate around 32; see the results |
| `-duration` | 30s | Measurement window per workload |
| `-warmup` | 5s | Unmeasured, per workload |
| `-value-size` | 128 | Bytes |
| `-keyspace` | 10000 | Distinct keys, all seeded before measuring |
| `-mixed-read-ratio` | 0.9 | Read fraction in the mixed workload |
| `-label` | — | Free text for the results heading |

Markdown table on stdout, the same data as JSON on stderr:

```bash
go run ./cmd/kvbench -workload write 2>results.json >results.md
```

## Things it does that matter for the numbers to mean anything

- **Seeds the keyspace first.** Reading keys that do not exist measures the 404
  path, which is cheaper than a real read.
- **Warms up unrecorded.** Otherwise the p99 describes connection setup and
  lazily-initialised server state rather than steady state.
- **Keeps every sample** and takes exact quantiles. 8 bytes per request; the tail
  is the part worth knowing.
- **Does not time failed requests.** A connection refused in 200 µs would improve
  the p50, which is backwards.
- **Chooses a node per request, not per worker.** Pinning a worker to a node lets a
  slow node throttle only that worker while the others make up the throughput,
  hiding it in the average.
- **Opens a fresh connection per request** — forced, not chosen. The server has no
  keep-alive and closes the socket after each response, so Go's connection pool
  hands out dead connections and requests fail with "server closed idle
  connection". Every measurement therefore includes a TCP handshake, which is a
  real cost of this deployment and is not subtracted.
