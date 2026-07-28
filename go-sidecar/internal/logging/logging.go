// Package logging builds the structured loggers every other package uses.
//
// Every line the sidecar emits carries node_id and component (R5.1). That is the
// point: with three nodes writing to one `docker compose logs` stream, a message
// that does not say which node produced it and which subsystem it came from is
// nearly useless during an incident — and "nearly useless during an incident" is
// the only time anyone reads logs.
//
// JSON lines rather than human-prose, for the same reason: `docker compose logs |
// jq 'select(.component=="fsm")'` has to work.
package logging

import (
	"io"
	"log/slog"
	"os"
	"strings"
)

// Component names. Kept as constants rather than free strings so a typo shows up
// at compile time instead of as a log stream nobody can filter.
const (
	ComponentMain    = "main"
	ComponentFSM     = "fsm"
	ComponentRPC     = "rpc"
	ComponentMgmt    = "mgmt"
	ComponentJoiner  = "joiner"
	ComponentBackend = "backend"
	ComponentRaft    = "raft"
)

// ParseLevel maps a configured level name onto an slog level.
//
// Unknown values fall back to info rather than failing startup: a typo in a log
// level is a bad reason to refuse to boot, and the fallback is visible because
// the first thing New logs is the level it settled on.
func ParseLevel(name string) slog.Level {
	switch strings.ToLower(strings.TrimSpace(name)) {
	case "debug":
		return slog.LevelDebug
	case "warn", "warning":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

// New returns the process's root logger, tagged with node_id.
//
// Writes to stdout, not stderr: these are ordinary application logs and mixing
// them into stderr makes `docker compose logs` interleave them unpredictably with
// the C++ side's output.
func New(nodeID, level string) *slog.Logger {
	return NewWithWriter(os.Stdout, nodeID, level)
}

// NewWithWriter is New with an injectable sink, so tests can read what was
// logged instead of polluting the test output.
func NewWithWriter(w io.Writer, nodeID, level string) *slog.Logger {
	handler := slog.NewJSONHandler(w, &slog.HandlerOptions{
		Level: ParseLevel(level),
	})
	return slog.New(handler).With(slog.String("node_id", nodeID))
}

// For derives a component-scoped logger from a parent.
//
// Callers keep the returned logger rather than calling this per message; the
// attribute is fixed for the lifetime of the subsystem.
func For(parent *slog.Logger, component string) *slog.Logger {
	if parent == nil {
		// A nil parent means a caller forgot to wire one through. Returning a
		// working logger that says so beats a nil dereference in the middle of an
		// error path, which is exactly where that bug would surface.
		return slog.Default().With(
			slog.String("component", component),
			slog.Bool("logger_unset", true),
		)
	}
	return parent.With(slog.String("component", component))
}
