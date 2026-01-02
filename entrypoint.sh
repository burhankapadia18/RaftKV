#!/bin/bash

# Defaults
ID=${NODE_ID:-node1}
RAFT_PORT=8088
MGMT_PORT=6000
SIDE_PORT=50052
APP_PORT=50051
HTTP_PORT=8080
DATA_DIR=/app/data

echo "--- Starting Node $ID ---"

# 1. Start C++ App (Background)
# Usage: ./kvdb_node <http_port> <my_grpc_port> <sidecar_port> <db_file>
echo "Starting C++ KVDB..."
./kvdb_node $HTTP_PORT $APP_PORT $SIDE_PORT $DATA_DIR/kv.db &
CPP_PID=$!

# Wait for C++ to warm up
sleep 2

# 2. Build Go Arguments
GO_ARGS="-id $ID -raft $RAFT_PORT -srv $SIDE_PORT -app localhost:$APP_PORT -mgmt $MGMT_PORT -data $DATA_DIR"

# Docker specific: We must advertise our hostname so other containers can find us
GO_ARGS="$GO_ARGS -advertise $ID"

if [ "$BOOTSTRAP" = "true" ]; then
    GO_ARGS="$GO_ARGS -bootstrap true"
fi

if [ ! -z "$JOIN_ADDR" ]; then
    GO_ARGS="$GO_ARGS -join $JOIN_ADDR"
fi

# 3. Start Go Sidecar (Foreground)
echo "Starting Go Sidecar with args: $GO_ARGS"
./sidecar $GO_ARGS &
GO_PID=$!

# 4. Wait for any process to exit
wait -n

# Exit with status of process that exited first
exit $?