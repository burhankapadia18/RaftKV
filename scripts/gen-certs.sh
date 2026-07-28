#!/usr/bin/env bash
#
# Generate a throwaway cluster CA and one certificate per node (R6.3/R6.4).
#
# THIS IS A DEVELOPMENT HELPER, NOT A CA. It writes unencrypted private keys to a
# local directory and has no revocation, rotation or audit story. A real
# deployment should get certificates from whatever already issues them there
# (Vault, cert-manager, an internal PKI) — the sidecar only needs three file
# paths and does not care where they came from.
#
# Usage:
#   ./scripts/gen-certs.sh [output-dir] [node-id ...]
#
# Defaults to ./certs for node1 node2 node3 and proxy, which is what
# docker-compose.secure.yml mounts.

set -euo pipefail

OUT_DIR=${1:-certs}
shift || true
NODES=("$@")
if [ ${#NODES[@]} -eq 0 ]; then
    # `proxy` is the TLS-terminating reverse proxy from docker-compose.secure.yml
    # (R6.5). It is not a raft peer, but it needs a certificate signed by the same
    # CA so a client can verify it with the one file it already has.
    NODES=(node1 node2 node3 proxy)
fi

# Days, not years. These are dev certificates; a short life is a feature, because
# it makes "we shipped the test CA to production" fail fast and loudly.
DAYS=90

# The management listener and the raft transport both need a certificate that is
# valid for however the peer is dialled. Inside the compose network that is the
# service name (node1); from the host during a smoke test it is localhost or
# 127.0.0.1. All three go in the SAN list, because a name that is not in the SAN
# list is a name that fails verification — and CN is ignored by modern clients.
extra_sans() {
    local node=$1
    printf 'DNS:%s,DNS:localhost,IP:127.0.0.1,IP:::1' "$node"
}

if ! command -v openssl >/dev/null 2>&1; then
    echo "openssl is required but was not found on PATH" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
# The directory holds private keys. 0700 before anything is written into it, not
# after: a chmod that happens later leaves a window where the keys are readable.
chmod 700 "$OUT_DIR"

echo "==> Cluster CA"
if [ -f "$OUT_DIR/ca.pem" ]; then
    echo "    $OUT_DIR/ca.pem exists; reusing it (delete the directory to start over)"
else
    openssl ecparam -name prime256v1 -genkey -noout -out "$OUT_DIR/ca-key.pem" 2>/dev/null
    chmod 600 "$OUT_DIR/ca-key.pem"
    openssl req -x509 -new -key "$OUT_DIR/ca-key.pem" \
        -sha256 -days $((DAYS * 2)) \
        -subj "/CN=raftkv-dev-ca" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -out "$OUT_DIR/ca.pem" 2>/dev/null
    echo "    wrote $OUT_DIR/ca.pem"
fi

for node in "${NODES[@]}"; do
    echo "==> $node"
    key="$OUT_DIR/$node-key.pem"
    crt="$OUT_DIR/$node.pem"
    csr="$OUT_DIR/$node.csr"

    openssl ecparam -name prime256v1 -genkey -noout -out "$key" 2>/dev/null
    chmod 600 "$key"

    openssl req -new -key "$key" -subj "/CN=$node" -out "$csr" 2>/dev/null

    # extendedKeyUsage carries BOTH serverAuth and clientAuth. A raft peer is a
    # server for inbound append-entries and a client for outbound ones, using the
    # same identity in both directions; a serverAuth-only certificate produces a
    # cluster where every node listens happily and none can dial out.
    openssl x509 -req -in "$csr" \
        -CA "$OUT_DIR/ca.pem" -CAkey "$OUT_DIR/ca-key.pem" \
        -CAcreateserial -days "$DAYS" -sha256 \
        -extfile <(printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth,clientAuth\nkeyUsage=critical,digitalSignature,keyEncipherment\n' "$(extra_sans "$node")") \
        -out "$crt" 2>/dev/null

    rm -f "$csr"
    echo "    wrote $crt (SANs: $(extra_sans "$node"))"
done

# Permissions, stated honestly rather than aspirationally.
#
# The certificates and the NODE KEYS end up world-readable (0644), because the
# containers bind-mount this directory and the process inside runs as a uid that
# does not exist on the host — a 0600 key would simply be unreadable there, and
# the cluster would fail to start with a permission error instead of a TLS one.
# That is a real weakening, and it is why the banner above says this script is
# not for anything real: 0644 private keys are acceptable only for certificates
# that are thrown away with the directory.
#
# The CA private key is the exception and stays 0600. It is never mounted into a
# container, so nothing needs to read it but this script — and it is the one file
# that could mint a new cluster member.
chmod 644 "$OUT_DIR/ca.pem"
for node in "${NODES[@]}"; do
    chmod 644 "$OUT_DIR/$node.pem" "$OUT_DIR/$node-key.pem"
done
chmod 600 "$OUT_DIR/ca-key.pem"

cat <<EOF

Done. $OUT_DIR now contains:
  ca.pem         cluster CA        -> RAFT_TLS_CA / MGMT_TLS_CA
  ca-key.pem     CA private key    -> never mounted into a container
  <node>.pem     node certificate  -> RAFT_TLS_CERT / MGMT_TLS_CERT
  <node>-key.pem node private key  -> RAFT_TLS_KEY / MGMT_TLS_KEY

Bring the secure cluster up with:
  export RAFTKV_MGMT_TOKEN="\$(openssl rand -hex 32)"
  docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d

Node keys are 0644 so the container's user can read them through the bind mount.
That is a genuine weakening, acceptable only because these are throwaway
development certificates. Do not reuse this directory, or this script, for
anything real. ca-key.pem stays 0600 and is never mounted.
EOF
