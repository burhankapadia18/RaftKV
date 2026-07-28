#!/bin/bash

# Defaults
ID=${NODE_ID:-node1}
RAFT_PORT=8088
MGMT_PORT=6000
SIDE_PORT=50052
APP_PORT=50051
HTTP_PORT=8080
DATA_DIR=/app/data

# How long to wait for the C++ gRPC state machine to start accepting
# connections, and how long to pause between probes (seconds).
APP_WAIT_TIMEOUT=${APP_WAIT_TIMEOUT:-30}
APP_PROBE_INTERVAL=${APP_PROBE_INTERVAL:-0.2}

echo "--- Starting Node $ID ---"

# 1. Start C++ App (Background)
# Usage: ./kvdb_node <http_port> <my_grpc_port> <sidecar_port> <db_file>
echo "Starting C++ KVDB..."
./kvdb_node $HTTP_PORT $APP_PORT $SIDE_PORT $DATA_DIR/kv.db &
CPP_PID=$!

# 2. Wait for the C++ StateMachine gRPC port to accept connections.
# This replaces a flat `sleep 2`, which was simultaneously too long on a fast
# boot and too short on a slow one, and which happily started the sidecar
# against a C++ process that had already died.
probe_app_port() {
    # Bash's /dev/tcp redirection performs a real TCP connect. The subshell keeps
    # the descriptor out of this shell and its exit status is the connect result;
    # a loopback port that is not listening is refused immediately.
    (exec 3<>"/dev/tcp/127.0.0.1/$APP_PORT") 2>/dev/null
}

echo "Waiting for C++ KVDB gRPC on 127.0.0.1:$APP_PORT (timeout ${APP_WAIT_TIMEOUT}s)..."
APP_DEADLINE=$((SECONDS + APP_WAIT_TIMEOUT))

until probe_app_port; do
    # Do not spend the whole timeout probing a port that will never open: if the
    # C++ app is gone, its exit status is the diagnostic worth reporting.
    if ! kill -0 "$CPP_PID" 2>/dev/null; then
        wait "$CPP_PID"
        CPP_STATUS=$?
        echo "C++ KVDB (pid $CPP_PID) exited with status $CPP_STATUS before its gRPC port came up" >&2
        if [ "$CPP_STATUS" -eq 0 ]; then
            CPP_STATUS=1
        fi
        exit $CPP_STATUS
    fi

    if ((SECONDS >= APP_DEADLINE)); then
        echo "C++ KVDB gRPC port $APP_PORT did not open within ${APP_WAIT_TIMEOUT}s; giving up" >&2
        kill "$CPP_PID" 2>/dev/null
        exit 1
    fi

    sleep "$APP_PROBE_INTERVAL"
done

echo "C++ KVDB gRPC is accepting connections on 127.0.0.1:$APP_PORT"

# 3. Build Go Arguments
GO_ARGS="-id $ID -raft $RAFT_PORT -srv $SIDE_PORT -app localhost:$APP_PORT -mgmt $MGMT_PORT -data $DATA_DIR"

# Docker specific: We must advertise our hostname so other containers can find us
GO_ARGS="$GO_ARGS -advertise $ID"

if [ "$BOOTSTRAP" = "true" ]; then
    # `-bootstrap=true`, NOT `-bootstrap true`. Go's flag package treats a bare
    # `true` as the first POSITIONAL argument and stops parsing flags there, so
    # the space-separated form silently discards every flag that follows it.
    # That was harmless while nothing came after it; Phase 3 appends the
    # snapshot tunables, and on the bootstrap node they were being dropped —
    # the node ran production snapshot settings while its command line said
    # otherwise. Caught by test_snapshot.py refusing to run against a cluster
    # whose reported settings did not match the override.
    GO_ARGS="$GO_ARGS -bootstrap=true"
fi

if [ ! -z "$JOIN_ADDR" ]; then
    GO_ARGS="$GO_ARGS -join $JOIN_ADDR"
fi

# Snapshot tunables (Phase 3). Each is forwarded only when set, so an unset
# variable means "use the sidecar's own default" (HashiCorp Raft's: 120s /
# 8192 entries / 10240 trailing logs) rather than a value picked here -- there
# is exactly one place these defaults live, internal/config/config.go.
#
# docker-compose.test.yml sets all three far below production values so that
# tests/e2e/test_snapshot.py can observe a snapshot and a log truncation inside
# a test run. TRAILING_LOGS is not optional for that: Raft truncates to
# snapshot_index - TrailingLogs, so with the default the log never shrinks
# however often the node snapshots.
if [ ! -z "$SNAPSHOT_INTERVAL" ]; then
    GO_ARGS="$GO_ARGS -snapshot-interval $SNAPSHOT_INTERVAL"
fi

if [ ! -z "$SNAPSHOT_THRESHOLD" ]; then
    GO_ARGS="$GO_ARGS -snapshot-threshold $SNAPSHOT_THRESHOLD"
fi

if [ ! -z "$TRAILING_LOGS" ]; then
    GO_ARGS="$GO_ARGS -trailing-logs $TRAILING_LOGS"
fi

# TLS material (R6.3/R6.4). Forwarded only when set, so a node started without
# these behaves exactly as it did before Phase 6 -- the zero-config demo keeps
# working and security is something a deployment opts into.
#
# Paths, unlike the token, are safe in argv: they are not secrets, and the
# sidecar needs to report which surfaces are protected. The KEYS they point at
# must be mounted read-only; docker-compose.secure.yml does that.
#
# Deliberately NOT defaulted to a conventional path like /certs/$ID.pem. A
# default would mean a missing mount silently produces "TLS off" instead of a
# startup failure, which is the exact confusion this phase exists to remove:
# either the operator asks for TLS and gets it, or they do not ask.
add_flag_if_set() {
    # $1 = flag name, $2 = value
    if [ -n "$2" ]; then
        GO_ARGS="$GO_ARGS $1 $2"
    fi
}

add_flag_if_set -raft-tls-cert "$RAFT_TLS_CERT"
add_flag_if_set -raft-tls-key "$RAFT_TLS_KEY"
add_flag_if_set -raft-tls-ca "$RAFT_TLS_CA"
add_flag_if_set -mgmt-tls-cert "$MGMT_TLS_CERT"
add_flag_if_set -mgmt-tls-key "$MGMT_TLS_KEY"
add_flag_if_set -mgmt-tls-ca "$MGMT_TLS_CA"

if [ -n "$RAFT_TLS_CERT" ]; then
    echo "Raft peer transport: mutual TLS ($RAFT_TLS_CERT)"
else
    echo "WARNING: RAFT_TLS_CERT is not set; the raft peer port ($RAFT_PORT) is" >&2
    echo "         PLAINTEXT and unauthenticated. Anything that can reach it can" >&2
    echo "         append entries to the log. See docker-compose.secure.yml." >&2
fi

# The cluster-admin token (R6.1) is passed via the ENVIRONMENT, never as a flag.
# `-mgmt-token <secret>` would put it in the process's command line, where any
# local user can read it out of `ps`. internal/config already defaults the flag
# from RAFTKV_MGMT_TOKEN, so simply not passing it is what keeps it out of argv.
if [ -z "$RAFTKV_MGMT_TOKEN" ]; then
    echo "WARNING: RAFTKV_MGMT_TOKEN is not set; /join and /remove are DISABLED" >&2
    echo "         on this node. A node cannot join a cluster without it." >&2
fi

# 4. Start Go Sidecar (Foreground)
echo "Starting Go Sidecar with args: $GO_ARGS"
./sidecar $GO_ARGS &
GO_PID=$!

# 5. Forward SIGTERM/SIGINT to both children (R5.7).
#
# Without this, `docker compose stop` signalled this shell only. The children were
# never told, the grace period expired, and docker SIGKILLed everything — so the
# C++ side never drained its in-flight requests and the sidecar never got to hand
# leadership away. A planned stop cost the cluster a full election, and
# `docker compose stop` took the entire timeout every time.
forward_shutdown() {
    echo "--- Node $ID received a shutdown signal, forwarding to children ---"
    SHUTTING_DOWN=1
    # `|| true`: a child that has already exited is not an error here.
    [ -n "$CPP_PID" ] && kill -TERM "$CPP_PID" 2>/dev/null || true
    [ -n "$GO_PID" ] && kill -TERM "$GO_PID" 2>/dev/null || true
}

SHUTTING_DOWN=
trap forward_shutdown TERM INT

# Block until a child exits on its own (a crash) or a signal interrupts us.
# `wait` is interruptible by a trapped signal; that is what lets the handler run.
wait -n
FIRST_EXIT=$?

# On a signal, `wait -n` returns as soon as the handler has run, with the children
# still shutting down. Wait for them properly rather than exiting and letting
# docker SIGKILL them mid-drain — the whole point of the graceful path.
if [ -n "$SHUTTING_DOWN" ]; then
    wait "$CPP_PID" 2>/dev/null || true
    wait "$GO_PID" 2>/dev/null || true
    echo "--- Node $ID shut down cleanly ---"
    exit 0
fi

# Otherwise a child died unexpectedly: exit with its status so the container
# reports the failure.
exit $FIRST_EXIT
