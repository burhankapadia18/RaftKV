"""Phase 6 acceptance for the TLS deployment profile.

    ./scripts/gen-certs.sh
    export RAFTKV_MGMT_TOKEN="$(openssl rand -hex 32)"
    docker compose -f docker-compose.yml -f docker-compose.secure.yml up -d
    pytest tests/e2e -m requires_secure -v

Marked `requires_secure` and deselected by default, for the same reason
test_crash.py is: the normal suite must keep running against the plain compose
profile with nothing but pip installed. Every test here SKIPS rather than fails
when the secure cluster is not up, so an accidental run reports "not verified
here" instead of a wall of red.

What it does NOT re-check: the TLS handshake mechanics, which are covered by
Go unit tests over real sockets in internal/tlsconfig and internal/raftnode.
This module is here for the properties only a deployment has — that the ports
are wired the way the compose file claims, and that plaintext is actually
refused rather than merely discouraged.
"""

from __future__ import annotations

import os
import socket
import ssl
import subprocess
from pathlib import Path

import pytest
import requests

pytestmark = pytest.mark.requires_secure


REPO_ROOT = Path(__file__).resolve().parents[2]
CA_FILE = REPO_ROOT / "certs" / "ca.pem"

MGMT_URL = "https://localhost:6000"
PROXY_URL = "https://localhost:8443"

# Raft peers listen on 8088 but that port is not published to the host, so the
# plaintext check runs from inside the compose network.
RAFT_PORT = 8088

TIMEOUT = 10.0


@pytest.fixture(scope="module")
def ca_file() -> str:
    if not CA_FILE.exists():
        pytest.skip(f"{CA_FILE} not found; run ./scripts/gen-certs.sh first")
    return str(CA_FILE)


@pytest.fixture(scope="module")
def secure_cluster(ca_file: str) -> str:
    """Skip unless the secure profile is actually up and answering."""
    try:
        response = requests.get(f"{MGMT_URL}/ready", verify=ca_file, timeout=TIMEOUT)
    except requests.exceptions.RequestException as exc:
        pytest.skip(f"secure cluster is not reachable on {MGMT_URL}: {exc}")
    if response.status_code != 200:
        pytest.skip(f"secure cluster is not ready: {response.status_code} {response.text[:200]}")
    return ca_file


@pytest.fixture(scope="module")
def compose_network() -> str:
    """The docker network name, for the checks that must run peer-side."""
    if not _docker_available():
        pytest.skip("docker is not available")
    result = subprocess.run(
        ["docker", "network", "ls", "--format", "{{.Name}}"],
        capture_output=True, text=True, timeout=TIMEOUT, check=False,
    )
    for name in result.stdout.split():
        if "raftkv" in name:
            return name
    pytest.skip("no raftkv docker network found")


# ---------------------------------------------------------------------------
# R6.3 — management over TLS
# ---------------------------------------------------------------------------


def test_management_over_https_works(secure_cluster):
    response = requests.get(f"{MGMT_URL}/status", verify=secure_cluster, timeout=TIMEOUT)

    assert response.status_code == 200
    assert "is_leader" in response.json()


def test_management_certificate_must_verify(secure_cluster):
    """A certificate from the wrong CA must be refused.

    Without this the suite would pass with `verify=False` everywhere, which is
    the most common way a TLS deployment turns out to be verifying nothing.
    """
    wrong_ca = REPO_ROOT / "certs" / "proxy.pem"  # a leaf, not the CA

    with pytest.raises(requests.exceptions.SSLError):
        requests.get(f"{MGMT_URL}/status", verify=str(wrong_ca), timeout=TIMEOUT)


def test_management_plaintext_is_not_served(secure_cluster):
    """HTTP to the HTTPS management port must not be answered by a handler.

    Go's http.Server replies 400 with an explanatory line rather than dropping
    the connection, so the property is 'not served', not 'connection refused'.
    """
    try:
        response = requests.get("http://localhost:6000/status", timeout=TIMEOUT)
    except requests.exceptions.RequestException:
        return  # Refused outright; also correct.

    assert response.status_code != 200, (
        "the management port served a plaintext request; the bearer token would "
        "travel in clear"
    )


def test_management_rejects_tls_below_1_2(secure_cluster):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    try:
        context.maximum_version = ssl.TLSVersion.TLSv1_1
        context.minimum_version = ssl.TLSVersion.TLSv1
    except (AttributeError, ValueError) as exc:
        pytest.skip(f"this OpenSSL build cannot offer TLS < 1.2: {exc}")

    with pytest.raises((ssl.SSLError, OSError)):
        with socket.create_connection(("localhost", 6000), timeout=TIMEOUT) as sock:
            with context.wrap_socket(sock, server_hostname="localhost"):
                pass


# ---------------------------------------------------------------------------
# R6.4 — raft peer transport
# ---------------------------------------------------------------------------


def test_raft_port_refuses_plaintext(compose_network, secure_cluster):
    """The R6.4 acceptance criterion, from a peer's vantage point."""
    result = _in_network(
        compose_network,
        "alpine",
        ["sh", "-c",
         f'printf "GET / HTTP/1.0\\r\\n\\r\\n" | timeout 5 nc node1 {RAFT_PORT} | wc -c'],
    )
    byte_count = result.stdout.strip()

    assert byte_count == "0", (
        f"the raft port answered a plaintext request with {byte_count} bytes; "
        "it should complete no exchange at all"
    )


def test_raft_port_requires_a_client_certificate(compose_network, secure_cluster):
    """One-way TLS would encrypt the traffic and still let anyone append entries.

    Connecting with the CA but no client identity must fail the handshake.
    """
    result = _in_network(
        compose_network,
        "alpine/openssl",
        ["s_client", "-connect", f"node1:{RAFT_PORT}", "-CAfile", "/certs/ca.pem",
         "-servername", "node1", "-brief"],
        mount_certs=True,
    )
    combined = result.stdout + result.stderr

    assert "certificate required" in combined.lower() or result.returncode != 0, (
        "a peer with no client certificate completed the raft handshake:\n" + combined[:800]
    )


def test_raft_port_accepts_a_cluster_peer(compose_network, secure_cluster):
    """The positive case, so the test above cannot pass because nothing works."""
    result = _in_network(
        compose_network,
        "alpine/openssl",
        ["s_client", "-connect", f"node1:{RAFT_PORT}", "-CAfile", "/certs/ca.pem",
         "-cert", "/certs/node2.pem", "-key", "/certs/node2-key.pem",
         "-servername", "node1", "-brief"],
        mount_certs=True,
    )
    combined = result.stdout + result.stderr

    assert "Verification: OK" in combined, (
        "a legitimate cluster peer could not complete the raft handshake:\n" + combined[:800]
    )


# ---------------------------------------------------------------------------
# R6.5 — client API through the TLS proxy
# ---------------------------------------------------------------------------


def test_client_api_over_https_round_trips(secure_cluster):
    key = f"secure-{os.getpid()}"

    put = requests.put(
        f"{PROXY_URL}/kv/{key}", data=b"tls-value", verify=secure_cluster, timeout=TIMEOUT
    )
    assert put.status_code == 200, f"PUT over HTTPS returned {put.status_code}: {put.text[:200]}"

    # Linearizable, so a round-robin hop onto a lagging follower is a real
    # failure rather than the documented staleness of a default read.
    got = requests.get(
        f"{PROXY_URL}/kv/{key}",
        params={"consistency": "linearizable"},
        verify=secure_cluster,
        timeout=TIMEOUT,
    )
    assert got.status_code == 200
    assert got.text == "tls-value"


def test_plaintext_client_port_is_not_published(secure_cluster):
    """The secure profile must not leave a plaintext door open next to the locked one.

    `ports` concatenates across compose files rather than replacing, so this is
    a real mistake that has already been made once in this repo: the first
    version of docker-compose.secure.yml still published 8080.
    """
    for port in (8080, 8081, 8082):
        with pytest.raises((requests.exceptions.RequestException, OSError)):
            requests.get(f"http://localhost:{port}/metrics", timeout=2.0)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def _docker_available() -> bool:
    try:
        return subprocess.run(
            ["docker", "info"], capture_output=True, timeout=TIMEOUT, check=False
        ).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _in_network(network: str, image: str, argv: list, mount_certs: bool = False):
    command = ["docker", "run", "--rm", "--network", network]
    if mount_certs:
        command += ["-v", f"{REPO_ROOT / 'certs'}:/certs:ro"]
    command += [image] + argv

    return subprocess.run(
        command, capture_output=True, text=True, timeout=120, check=False, stdin=subprocess.DEVNULL
    )
