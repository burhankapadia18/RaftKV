// Command kvbench measures RaftKV throughput and latency against a running
// cluster (R7.5).
//
//	go run ./cmd/kvbench -workload write -clients 32 -duration 30s
//	go run ./cmd/kvbench -workload all -nodes http://localhost:8080,http://localhost:8081,http://localhost:8082
//
// Output is a markdown table, so a run can be pasted into docs/benchmarks.md
// without reformatting.
//
// # What this measures, and what it does not
//
// This is a CLIENT-side measurement over HTTP, from one process, usually on the
// same machine as the cluster. It therefore includes HTTP parsing, the localhost
// network, and the client's own scheduling — all of which are real costs a real
// client pays, but which mean these numbers are not "how fast raft is". They are
// "how fast this deployment answers a client".
//
// Latency is reported at p50/p95/p99 from a full record of every sample, not a
// streaming estimate. At these sample counts the memory is trivial (8 bytes per
// request) and an exact quantile is worth more than an approximate one when the
// interesting question is the tail.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"os"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	workloadWrite            = "write"
	workloadReadLocal        = "read-local"
	workloadReadLinearizable = "read-linearizable"
	workloadMixed            = "mixed"
	workloadAll              = "all"
)

var workloadOrder = []string{
	workloadWrite, workloadReadLocal, workloadReadLinearizable, workloadMixed,
}

type config struct {
	nodes     []string
	clients   int
	duration  time.Duration
	valueSize int
	keyspace  int
	workload  string
	warmup    time.Duration
	timeout   time.Duration
	mixedRead float64
	label     string
}

func main() {
	var (
		nodesFlag = flag.String("nodes", "http://localhost:8080,http://localhost:8081,http://localhost:8082",
			"comma-separated client HTTP endpoints; requests are spread across all of them")
		clients   = flag.Int("clients", 32, "concurrent client goroutines")
		duration  = flag.Duration("duration", 30*time.Second, "measurement window per workload")
		valueSize = flag.Int("value-size", 128, "value size in bytes")
		keyspace  = flag.Int("keyspace", 10000, "distinct keys")
		workload  = flag.String("workload", workloadAll,
			"write | read-local | read-linearizable | mixed | all")
		warmup    = flag.Duration("warmup", 5*time.Second, "unmeasured warm-up before each workload")
		timeout   = flag.Duration("timeout", 10*time.Second, "per-request timeout")
		mixedRead = flag.Float64("mixed-read-ratio", 0.9, "read fraction in the mixed workload")
		label     = flag.String("label", "", "free-text label for the results table (e.g. \"3-node\")")
	)
	flag.Parse()

	cfg := config{
		nodes:     splitNodes(*nodesFlag),
		clients:   *clients,
		duration:  *duration,
		valueSize: *valueSize,
		keyspace:  *keyspace,
		workload:  *workload,
		warmup:    *warmup,
		timeout:   *timeout,
		mixedRead: *mixedRead,
		label:     *label,
	}

	if err := run(cfg); err != nil {
		fmt.Fprintf(os.Stderr, "kvbench: %v\n", err)
		os.Exit(1)
	}
}

func splitNodes(raw string) []string {
	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, part := range parts {
		trimmed := strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(part), "/"))
		if trimmed != "" {
			out = append(out, trimmed)
		}
	}
	return out
}

func run(cfg config) error {
	if len(cfg.nodes) == 0 {
		return errors.New("no nodes given")
	}
	if cfg.clients < 1 {
		return errors.New("-clients must be at least 1")
	}

	client := newHTTPClient(cfg)

	if err := checkReachable(client, cfg); err != nil {
		return err
	}

	workloads := []string{cfg.workload}
	if cfg.workload == workloadAll {
		workloads = workloadOrder
	}

	// Seed the keyspace before ANY workload, including the write one. A read
	// benchmark against keys that do not exist measures the 404 path, which is
	// cheaper than a real read and would flatter the numbers considerably.
	fmt.Fprintf(os.Stderr, "seeding %d keys...\n", cfg.keyspace)
	if err := seed(client, cfg); err != nil {
		return fmt.Errorf("seeding the keyspace: %w", err)
	}

	results := make([]result, 0, len(workloads))
	for _, name := range workloads {
		if !validWorkload(name) {
			return fmt.Errorf("unknown workload %q", name)
		}
		fmt.Fprintf(os.Stderr, "running %s (warmup %s, measure %s)...\n",
			name, cfg.warmup, cfg.duration)
		results = append(results, measure(client, cfg, name))
	}

	printMarkdown(cfg, results)
	return nil
}

func validWorkload(name string) bool {
	for _, known := range workloadOrder {
		if name == known {
			return true
		}
	}
	return false
}

func newHTTPClient(cfg config) *http.Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()

	// Connection reuse is DISABLED, and not as a tuning choice.
	//
	// The server does not implement keep-alive: it closes the socket after every
	// response. Go's transport does not know that, so it returns the connection to
	// the idle pool, hands it to the next request, and that request fails with
	// "http: server closed idle connection". The first version of this tool pooled
	// connections and lost 38 of 500 seed writes to exactly that — a client-side
	// artefact that looks like server errors.
	//
	// So every request opens a fresh connection, which is what any client of this
	// server must do. That makes a TCP handshake part of every measurement here.
	// It is a real cost of the deployment rather than benchmark overhead, and
	// docs/benchmarks.md says so instead of quietly subtracting it.
	transport.DisableKeepAlives = true
	transport.MaxConnsPerHost = 0
	return &http.Client{Transport: transport, Timeout: cfg.timeout}
}

func checkReachable(client *http.Client, cfg config) error {
	for _, node := range cfg.nodes {
		resp, err := client.Get(node + "/metrics")
		if err != nil {
			return fmt.Errorf("%s is not reachable: %w (is the cluster up?)", node, err)
		}
		_, _ = io.Copy(io.Discard, resp.Body)
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("%s answered %d to /metrics", node, resp.StatusCode)
		}
	}
	return nil
}

func keyFor(index int) string {
	return fmt.Sprintf("bench-k%08d", index)
}

func makeValue(size int) []byte {
	value := make([]byte, size)
	for i := range value {
		value[i] = byte('a' + i%26)
	}
	return value
}

// seed writes every key once so reads have something to find.
func seed(client *http.Client, cfg config) error {
	value := makeValue(cfg.valueSize)
	var failed atomic.Int64
	var firstErr atomic.Value

	var wg sync.WaitGroup
	work := make(chan int, cfg.clients*2)

	for w := 0; w < cfg.clients; w++ {
		wg.Add(1)
		go func(worker int) {
			defer wg.Done()
			node := cfg.nodes[worker%len(cfg.nodes)]
			for index := range work {
				if err := put(client, node, keyFor(index), value, cfg.timeout); err != nil {
					if failed.Add(1) == 1 {
						firstErr.Store(err.Error())
					}
				}
			}
		}(w)
	}

	for index := 0; index < cfg.keyspace; index++ {
		work <- index
	}
	close(work)
	wg.Wait()

	if n := failed.Load(); n > 0 {
		return fmt.Errorf("%d of %d seed writes failed; first: %v",
			n, cfg.keyspace, firstErr.Load())
	}
	return nil
}

type result struct {
	workload  string
	ops       int64
	errors    int64
	elapsed   time.Duration
	latencies []time.Duration
}

func (r result) throughput() float64 {
	if r.elapsed <= 0 {
		return 0
	}
	return float64(r.ops) / r.elapsed.Seconds()
}

func (r result) percentile(p float64) time.Duration {
	if len(r.latencies) == 0 {
		return 0
	}
	// Nearest-rank on the sorted samples. Exact, because every sample is kept.
	index := int(float64(len(r.latencies)-1) * p)
	return r.latencies[index]
}

func measure(client *http.Client, cfg config, workload string) result {
	// Warm up without recording. The first requests pay connection setup and any
	// lazily-initialised state on the server, and including them would show up as
	// a p99 that says more about startup than about the system.
	if cfg.warmup > 0 {
		runFor(client, cfg, workload, cfg.warmup, false)
	}
	return runFor(client, cfg, workload, cfg.duration, true)
}

func runFor(
	client *http.Client, cfg config, workload string,
	window time.Duration, record bool,
) result {
	value := makeValue(cfg.valueSize)
	ctx, cancel := context.WithTimeout(context.Background(), window)
	defer cancel()

	var (
		mu        sync.Mutex
		latencies []time.Duration
		ops       atomic.Int64
		errs      atomic.Int64
	)
	if record {
		// Pre-size generously; a resize mid-run would be measured as latency.
		latencies = make([]time.Duration, 0, 1<<16)
	}

	started := time.Now()
	var wg sync.WaitGroup

	for w := 0; w < cfg.clients; w++ {
		wg.Add(1)
		go func(worker int) {
			defer wg.Done()
			rng := rand.New(rand.NewSource(int64(worker) + 1))
			local := make([]time.Duration, 0, 4096)

			for ctx.Err() == nil {
				// A node per REQUEST, not per worker: pinning a worker to a node
				// would let a slow node throttle only its own worker while the
				// others make up the throughput, hiding it in the average.
				node := cfg.nodes[rng.Intn(len(cfg.nodes))]
				key := keyFor(rng.Intn(cfg.keyspace))

				begin := time.Now()
				var err error
				switch workload {
				case workloadWrite:
					err = put(client, node, key, value, cfg.timeout)
				case workloadReadLocal:
					err = get(client, node, key, false, cfg.timeout)
				case workloadReadLinearizable:
					err = get(client, node, key, true, cfg.timeout)
				case workloadMixed:
					if rng.Float64() < cfg.mixedRead {
						err = get(client, node, key, false, cfg.timeout)
					} else {
						err = put(client, node, key, value, cfg.timeout)
					}
				}
				elapsed := time.Since(begin)

				if err != nil {
					errs.Add(1)
					// Failed requests are counted but NOT timed into the
					// distribution: a connection refused in 200µs would improve
					// the p50, which is exactly backwards.
					continue
				}
				ops.Add(1)
				if record {
					local = append(local, elapsed)
				}
			}

			if record && len(local) > 0 {
				mu.Lock()
				latencies = append(latencies, local...)
				mu.Unlock()
			}
		}(w)
	}

	wg.Wait()
	elapsed := time.Since(started)

	sort.Slice(latencies, func(i, j int) bool { return latencies[i] < latencies[j] })
	return result{
		workload:  workload,
		ops:       ops.Load(),
		errors:    errs.Load(),
		elapsed:   elapsed,
		latencies: latencies,
	}
}

func put(client *http.Client, node, key string, value []byte, timeout time.Duration) error {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodPut,
		node+"/kv/"+key, bytes.NewReader(value))
	if err != nil {
		return err
	}
	return do(client, req, http.StatusOK)
}

func get(client *http.Client, node, key string, linearizable bool, timeout time.Duration) error {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	url := node + "/kv/" + key
	if linearizable {
		url += "?consistency=linearizable"
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	return do(client, req, http.StatusOK)
}

func do(client *http.Client, req *http.Request, want int) error {
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	// Drain before closing, or the connection cannot be reused and every request
	// pays a new handshake.
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
	if resp.StatusCode != want {
		return fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}
	return nil
}

func printMarkdown(cfg config, results []result) {
	label := cfg.label
	if label == "" {
		label = fmt.Sprintf("%d node(s)", len(cfg.nodes))
	}

	fmt.Printf("### %s — %d clients, %d-byte values, %d keys, %s per workload\n\n",
		label, cfg.clients, cfg.valueSize, cfg.keyspace, cfg.duration)
	fmt.Println("| Workload | Throughput (ops/s) | p50 | p95 | p99 | Errors |")
	fmt.Println("|---|---:|---:|---:|---:|---:|")
	for _, r := range results {
		fmt.Printf("| %s | %.0f | %s | %s | %s | %d |\n",
			r.workload,
			r.throughput(),
			fmtDuration(r.percentile(0.50)),
			fmtDuration(r.percentile(0.95)),
			fmtDuration(r.percentile(0.99)),
			r.errors,
		)
	}
	fmt.Println()

	// Machine-readable too, so a run can be diffed against another without
	// re-parsing a table.
	summary := map[string]any{
		"label":      label,
		"nodes":      cfg.nodes,
		"clients":    cfg.clients,
		"value_size": cfg.valueSize,
		"keyspace":   cfg.keyspace,
		"duration_s": cfg.duration.Seconds(),
		"results":    make([]map[string]any, 0, len(results)),
	}
	for _, r := range results {
		summary["results"] = append(summary["results"].([]map[string]any), map[string]any{
			"workload":   r.workload,
			"ops":        r.ops,
			"errors":     r.errors,
			"throughput": r.throughput(),
			"p50_us":     r.percentile(0.50).Microseconds(),
			"p95_us":     r.percentile(0.95).Microseconds(),
			"p99_us":     r.percentile(0.99).Microseconds(),
		})
	}
	encoded, err := json.MarshalIndent(summary, "", "  ")
	if err == nil {
		fmt.Fprintln(os.Stderr, string(encoded))
	}
}

func fmtDuration(d time.Duration) string {
	switch {
	case d == 0:
		return "-"
	case d < time.Millisecond:
		return fmt.Sprintf("%.2fms", float64(d.Microseconds())/1000)
	default:
		return fmt.Sprintf("%.1fms", float64(d.Microseconds())/1000)
	}
}
