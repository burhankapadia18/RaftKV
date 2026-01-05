# --- Stage 1: Build Go Sidecar ---
FROM golang:1.24.5 AS go_builder
WORKDIR /app

COPY go-sidecar/go.mod go-sidecar/go.sum ./go-sidecar/
WORKDIR /app/go-sidecar
RUN go mod download

COPY proto /app/proto
COPY go-sidecar /app/go-sidecar

ENV CGO_ENABLED=0
RUN go build -o /sidecar cmd/sidecar/main.go

# --- Stage 2: Build C++ App ---
FROM debian:bookworm-slim AS cpp_builder
WORKDIR /app

RUN apt-get update && apt-get install -y \
    build-essential cmake \
    libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc \
    pkg-config \
    libmsgpack-dev

COPY proto /app/proto
COPY cpp-app /app/cpp-app

WORKDIR /app/cpp-app/build

RUN rm -rf * && cmake -DCMAKE_PREFIX_PATH=/app .. && make -j4

# --- Stage 3: Runtime Image ---
FROM debian:bookworm-slim
WORKDIR /app

RUN apt-get update && apt-get install -y \
    libgrpc++1.51 libprotobuf32 \
    libmsgpackc2 \
    curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=go_builder /sidecar /app/sidecar
COPY --from=cpp_builder /app/cpp-app/build/kvdb_node /app/kvdb_node
COPY entrypoint.sh /app/entrypoint.sh

RUN chmod +x /app/entrypoint.sh

EXPOSE 8080 8088 6000 50051 50052

ENTRYPOINT ["/app/entrypoint.sh"]