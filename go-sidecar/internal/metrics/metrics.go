// Package metrics holds the sidecar's Prometheus collectors (R5.3).
//
// One package rather than metrics scattered through the packages that emit them,
// so the exported surface — the thing an operator builds alerts on — is
// reviewable in one file. Renaming a metric breaks dashboards, so it should be a
// visible diff here rather than an incidental edit somewhere else.
package metrics

import (
	"strconv"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
)

// Propose outcome labels. Constants because they are a closed set and a typo
// would silently create a new time series rather than fail.
const (
	OutcomeOK        = "ok"
	OutcomeNotLeader = "not_leader"
	OutcomeForwarded = "forwarded"
	OutcomeError     = "error"
)

var (
	// ApplyLatency measures the local FSM apply, i.e. the round trip to the C++
	// state machine. Buckets start at 100µs because a healthy local gRPC apply is
	// sub-millisecond, and end at 5s because the C++ client's deadline is 5s —
	// anything slower is already a failure, so finer resolution up there buys
	// nothing.
	ApplyLatency = promauto.NewHistogram(prometheus.HistogramOpts{
		Name: "raftkv_fsm_apply_duration_seconds",
		Help: "Time to apply one committed Raft log entry to the C++ state machine.",
		Buckets: []float64{
			0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025,
			0.05, 0.1, 0.25, 0.5, 1, 2.5, 5,
		},
	})

	// ApplyErrors counts applies that did NOT succeed, split by the distinction
	// Phase 1 established: a deterministic rejection leaves the cluster
	// consistent, a transport failure can diverge this replica. Alerting on the
	// two together would page someone every time a client sends a bad command.
	ApplyErrors = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "raftkv_fsm_apply_errors_total",
		Help: "Failed applies, by kind: rejected (deterministic) or transport (may diverge).",
	}, []string{"kind"})

	// ProposeTotal counts proposals by outcome, which is what makes "is this node
	// serving writes or just forwarding them" answerable.
	ProposeTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "raftkv_propose_total",
		Help: "Proposals handled, by outcome (ok, not_leader, forwarded, error).",
	}, []string{"outcome"})

	// ReadTotal does the same for linearizable reads.
	ReadTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "raftkv_read_total",
		Help: "Linearizable reads handled, by outcome (ok, not_leader, forwarded, error).",
	}, []string{"outcome"})
)

// Apply error kinds (see ApplyErrors).
const (
	ApplyErrorRejected  = "rejected"
	ApplyErrorTransport = "transport"
)

// RaftStatsSource is the consumer-side view of the Raft node the collector needs.
// *raftnode.Node satisfies it; declared here so this package does not depend on
// raftnode, matching the convention used by management.RaftControl.
type RaftStatsSource interface {
	Stats() map[string]string
	FirstLogIndex() (uint64, error)
}

// raftCollector exports raft.Stats() as gauges.
//
// Implemented as a custom Collector rather than gauges updated on a ticker
// because these values are only meaningful at scrape time. A background updater
// would report whatever the last tick saw, which is how a dashboard ends up
// showing a stale leader during the exact minute someone is looking at it.
type raftCollector struct {
	source RaftStatsSource

	lastLogIndex      *prometheus.Desc
	firstLogIndex     *prometheus.Desc
	appliedIndex      *prometheus.Desc
	commitIndex       *prometheus.Desc
	lastSnapshotIndex *prometheus.Desc
	term              *prometheus.Desc
	isLeader          *prometheus.Desc
	numPeers          *prometheus.Desc
}

// NewRaftCollector returns a collector reading from source.
func NewRaftCollector(source RaftStatsSource) prometheus.Collector {
	g := func(name, help string) *prometheus.Desc {
		return prometheus.NewDesc("raftkv_raft_"+name, help, nil, nil)
	}
	return &raftCollector{
		source:            source,
		lastLogIndex:      g("last_log_index", "Index of the last entry in the Raft log."),
		firstLogIndex:     g("first_log_index", "Index of the first entry retained in the Raft log; rises only when a snapshot truncates it."),
		appliedIndex:      g("applied_index", "Index of the last entry applied to the state machine."),
		commitIndex:       g("commit_index", "Index of the last committed entry."),
		lastSnapshotIndex: g("last_snapshot_index", "Index covered by the most recent snapshot."),
		term:              g("term", "Current Raft term."),
		isLeader:          g("is_leader", "1 when this node is the Raft leader, else 0."),
		numPeers:          g("num_peers", "Number of peers this node knows about."),
	}
}

func (c *raftCollector) Describe(ch chan<- *prometheus.Desc) {
	for _, d := range c.descs() {
		ch <- d
	}
}

func (c *raftCollector) descs() []*prometheus.Desc {
	return []*prometheus.Desc{
		c.lastLogIndex, c.firstLogIndex, c.appliedIndex, c.commitIndex,
		c.lastSnapshotIndex, c.term, c.isLeader, c.numPeers,
	}
}

func (c *raftCollector) Collect(ch chan<- prometheus.Metric) {
	stats := c.source.Stats()

	emit := func(desc *prometheus.Desc, key string) {
		// A key raft stopped reporting is skipped rather than exported as 0. A
		// zero here is indistinguishable from a real zero index, and a missing
		// series is the honest signal that something changed under us.
		raw, ok := stats[key]
		if !ok {
			return
		}
		value, err := strconv.ParseFloat(raw, 64)
		if err != nil {
			return
		}
		ch <- prometheus.MustNewConstMetric(desc, prometheus.GaugeValue, value)
	}

	emit(c.lastLogIndex, "last_log_index")
	emit(c.appliedIndex, "applied_index")
	emit(c.commitIndex, "commit_index")
	emit(c.lastSnapshotIndex, "last_snapshot_index")
	emit(c.term, "term")
	emit(c.numPeers, "num_peers")

	// state is a string ("Leader"/"Follower"/"Candidate"/"Shutdown"); a gauge is
	// more useful to alert on than a label whose value keeps changing.
	leader := 0.0
	if stats["state"] == "Leader" {
		leader = 1.0
	}
	ch <- prometheus.MustNewConstMetric(c.isLeader, prometheus.GaugeValue, leader)

	// FirstLogIndex comes from the log store, not Stats(), and can fail. A failed
	// read is a skipped series for the same reason as above.
	if first, err := c.source.FirstLogIndex(); err == nil {
		ch <- prometheus.MustNewConstMetric(
			c.firstLogIndex, prometheus.GaugeValue, float64(first))
	}
}
