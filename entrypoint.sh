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
    GO_ARGS="$GO_ARGS -bootstrap true"
fi

if [ ! -z "$JOIN_ADDR" ]; then
    GO_ARGS="$GO_ARGS -join $JOIN_ADDR"
fi

# 4. Start Go Sidecar (Foreground)
echo "Starting Go Sidecar with args: $GO_ARGS"
./sidecar $GO_ARGS &
GO_PID=$!

# 5. Wait for any process to exit
wait -n

# Exit with status of process that exited first
exit $?
