# Phase 6 — Security

## Spec

### Goals
Close the open cluster-membership hole, offer TLS on every non-localhost surface, and
harden the input boundaries with fuzzing and static analysis. Security is opt-in by
flags (the zero-config docker-compose demo keeps working) but the compose file ships
with it **enabled** so the documented deployment is the secure one.

### Non-goals
- No multi-tenant authz / per-key ACLs — a single cluster-admin credential and a
  single client credential are enough for 1.0.
- No key management/rotation tooling beyond documenting cert/token generation.

### Requirements

**Management API (the critical hole — unauthenticated `/join`):**
- R6.1 Bearer-token auth middleware on all mutating management endpoints (`/join`,
  `/remove`): token from `-mgmt-token` flag / `RAFTKV_MGMT_TOKEN` env; missing token
  config = endpoints disabled (403 with explanatory error), not silently open.
  `/health`, `/ready`, `/metrics`, `/status` stay unauthenticated (read-only, needed
  by probes).
- R6.2 Join **forwarding** (Phase 4) carries the token through.
- R6.3 Optional TLS on the management listener (`-mgmt-tls-cert/-mgmt-tls-key`).

**Raft peer transport:**
- R6.4 Optional mTLS between raft peers: replace `raft.NewTCPTransport` with
  `raft.NewNetworkTransport` over a TLS `StreamLayer` (cert/key/CA flags in
  `internal/config`, one cluster CA). Off by default for local dev; on in compose.

**Client HTTP API (C++):**
- R6.5 Optional TLS on the client-facing HTTP server. Decision point: hand-rolling
  TLS over raw sockets is out — either link OpenSSL behind a small
  `network/tls_socket.hpp` seam, or document TLS-termination-via-reverse-proxy as the
  supported mode for 1.0 and ship a compose example with a Caddy/nginx sidecar.
  **Default recommendation: proxy termination** (keeps the C++ engine dependency-light;
  revisit post-1.0). The spec requires one of the two to be implemented and documented.

**Localhost surfaces:**
- R6.6 The intra-node gRPC pair (50051/50052) stays plaintext but binds **127.0.0.1**
  explicitly (C++ `Config::grpc_address()`, sidecar backend/rpc listeners) instead of
  `0.0.0.0`; docker compose stops publishing those ports.

**Input hardening:**
- R6.7 libFuzzer targets (`cpp-app/fuzz/`): `KVCommand::from_msgpack` and
  `HttpRequestParser::parse`. CI runs each for a bounded time (60s) per PR; corpora
  checked in. Build behind `KVDB_BUILD_FUZZERS`.
- R6.8 `gosec ./...` job in CI (Go), `-fsanitize=address,undefined` job already exists
  (Phase 0); triage findings, suppress only with justification comments.
- R6.9 Body/header caps from Phase 4 verified by fuzz + e2e oversized-request test
  (returns 413).

### Acceptance criteria
- E2E security tests: `/join` without token → 401/403 and the peer is **not** added;
  with token → joins. Oversized body → 413. With TLS compose profile up: plaintext
  connection to the raft port is refused; management over HTTPS works.
- Fuzzers run clean for the CI budget; any crash found becomes a regression test.
- `gosec` green; sanitizer jobs green.
- `docker compose ps` shows no published 50051/50052 ports.

## Plan

1. **Token auth**: middleware in `internal/management` (constructor takes token config;
   unit tests for missing/wrong/valid token, and the disabled-by-default 403); thread
   token through `cluster.Joiner` and Phase 4 join-forwarding.
2. **Bind hardening**: default listeners to 127.0.0.1 for the gRPC pair (R6.6) —
   `internal/config` defaults, `cpp-app/src/config/config.hpp`, compose port cleanup.
3. **Raft mTLS**: `internal/raftnode/tls_transport.go` implementing a TLS
   `raft.StreamLayer`; config flags; unit test with self-signed pair; compose profile
   `secure` with a `gen-certs.sh` helper script.
4. **Management TLS**: R6.3 flags + `ListenAndServeTLS`; joiner honors `https`.
5. **Client TLS decision (R6.5)**: implement the reverse-proxy compose example
   (`docker-compose.secure.yml` with Caddy terminating :8443 → node HTTP) + README
   guidance; leave a tracked issue for native TLS post-1.0.
6. **Fuzzing**: `cpp-app/fuzz/fuzz_kv_command.cpp`, `fuzz_http_parser.cpp`, CMake
   fuzzer targets, seed corpora, CI job (clang, 60s each).
7. **gosec**: add to the Go CI job; fix/annotate findings.
8. **Docs**: README Security section (what is/isn't protected, cert/token setup);
   CLAUDE.md known-limitation "No TLS/auth anywhere" removed and replaced with an
   accurate statement of the security model.

### Verification
- Unit + e2e security tests in CI; manual: `curl :6000/join?...` without token against
  the live cluster fails; `openssl s_client` against the raft port with the secure
  profile shows the TLS handshake.
- `/cluster-smoke-test` green in both default and `secure` compose profiles.
