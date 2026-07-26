"""Shared fixtures and helpers for the RaftKV end-to-end suite.

The suite runs against an **already-running** cluster. It never starts, stops
or builds anything with docker: the compose lifecycle belongs to the CI job and
to the ``cluster-smoke-test`` skill, not to the tests.

Environment variables:
    RAFTKV_NODES                 Comma-separated node base URLs. Default
                                 ``http://localhost:8080,http://localhost:8081,http://localhost:8082``
                                 (the host port mapping in docker-compose.yml).
    RAFTKV_HTTP_TIMEOUT          Per-request socket timeout, seconds (default 5.0).
    RAFTKV_REPLICATION_TIMEOUT   Deadline for a committed write to become
                                 readable on a node, seconds (default 5.0 --
                                 the R0.1 deadline).
    RAFTKV_LEADER_TIMEOUT        Deadline for leader discovery, seconds
                                 (default 30.0) -- tolerates an election in
                                 progress on a freshly started cluster.
    RAFTKV_POLL_INTERVAL         Poll interval, seconds (default 0.1).
"""

from __future__ import annotations

import os
import re
import time
import uuid

import msgpack
import pytest
import requests

# The response bodies and content type the C++ engine actually emits live in
# contracts.py, imported by the tests as well so the two cannot drift.
from contracts import BODY_OK, MSGPACK_CONTENT_TYPE

# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

DEFAULT_NODES = "http://localhost:8080,http://localhost:8081,http://localhost:8082"


def _env_float(name: str, default: float) -> float:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        return float(raw)
    except ValueError as exc:  # pragma: no cover - operator error
        raise ValueError(f"{name} must be a number, got {raw!r}") from exc


HTTP_TIMEOUT = _env_float("RAFTKV_HTTP_TIMEOUT", 5.0)
REPLICATION_TIMEOUT = _env_float("RAFTKV_REPLICATION_TIMEOUT", 5.0)
LEADER_TIMEOUT = _env_float("RAFTKV_LEADER_TIMEOUT", 30.0)
POLL_INTERVAL = _env_float("RAFTKV_POLL_INTERVAL", 0.1)


def node_urls() -> list[str]:
    """Base URLs of the cluster nodes, from ``RAFTKV_NODES`` or the default."""
    raw = os.environ.get("RAFTKV_NODES", "").strip() or DEFAULT_NODES
    return [url.strip().rstrip("/") for url in raw.split(",") if url.strip()]


# --------------------------------------------------------------------------
# Polling
# --------------------------------------------------------------------------


def wait_until(predicate, timeout: float = 5.0, interval: float = 0.1):
    """Poll ``predicate`` until it returns something truthy or ``timeout`` elapses.

    Returns the last value the predicate produced, so callers get both the
    "did it happen" answer and whatever the predicate computed.

    This is the *only* place the suite sleeps. Every wait in these tests goes
    through here instead of the fixed ``time.sleep(0.3)`` that the old
    ``test_client.py`` used (R0.1).
    """
    deadline = time.monotonic() + timeout
    result = predicate()
    while not result:
        if time.monotonic() >= deadline:
            return result
        time.sleep(interval)
        result = predicate()
    return result


# --------------------------------------------------------------------------
# Wire helpers
# --------------------------------------------------------------------------


def pack_command(op: str, key: str, value: str = "") -> bytes:
    """MsgPack-encode the ``{op, key, value}`` map the C++ side expects.

    Matches ``MSGPACK_DEFINE_MAP(op, key, value)`` in
    cpp-app/src/commands/kv_command.hpp -- a map, not an array, and all three
    fields always present.
    """
    return msgpack.packb({"op": op, "key": key, "value": value}, use_bin_type=True)


def decode_body(response: requests.Response) -> str:
    """Decode a response body without relying on charset sniffing.

    The C++ server sends no ``Content-Type`` on responses, so
    ``Response.text`` would fall back to encoding *detection*. The bodies we
    assert on are ASCII; decode them explicitly.
    """
    return response.content.decode("utf-8")


def post_command(base_url: str, op: str, key: str, value: str = "") -> requests.Response:
    """POST a MsgPack command to ``/insert-val`` on one node.

    Deliberately uses the module-level ``requests`` API (a fresh connection per
    call) rather than a ``requests.Session``: the C++ HTTP server closes the
    socket after every response without sending ``Connection: close``, so a
    pooled keep-alive connection would be reused after the peer hung up.
    """
    return requests.post(
        f"{base_url}/insert-val",
        data=pack_command(op, key, value),
        headers={"Content-Type": MSGPACK_CONTENT_TYPE},
        timeout=HTTP_TIMEOUT,
    )


def get_value(base_url: str, key: str) -> requests.Response:
    """GET ``/get-val?key=...`` from one node.

    Keys must be URL-safe: the C++ query parser does no percent-decoding
    (cpp-app/src/network/http_request.hpp). The ``unique_key`` fixture only
    produces ``[A-Za-z0-9-]``.
    """
    return requests.get(
        f"{base_url}/get-val",
        params={"key": key},
        timeout=HTTP_TIMEOUT,
    )


# --------------------------------------------------------------------------
# Cluster client
# --------------------------------------------------------------------------


class ClusterClient:
    """Thin HTTP client bound to a discovered cluster topology."""

    def __init__(self, nodes, leader: str) -> None:
        self.nodes = tuple(nodes)
        self.leader = leader
        self.followers = tuple(url for url in nodes if url != leader)

    @staticmethod
    def body(response: requests.Response) -> str:
        """Response body as text (see :func:`decode_body`)."""
        return decode_body(response)

    # -- writes ------------------------------------------------------------

    def set(self, base_url: str, key: str, value: str) -> requests.Response:
        return post_command(base_url, "SET", key, value)

    def delete(self, base_url: str, key: str) -> requests.Response:
        return post_command(base_url, "DELETE", key, "")

    # -- reads -------------------------------------------------------------

    def get(self, base_url: str, key: str) -> requests.Response:
        return get_value(base_url, key)

    def read(self, base_url: str, key: str):
        """Body of ``GET /get-val``, or ``None`` if the node is unreachable.

        Transport errors are swallowed only so that polling survives a node
        that is momentarily busy; the caller's deadline still expires and the
        assertion still fails, reporting ``None`` as the last body seen.
        """
        try:
            return decode_body(self.get(base_url, key))
        except requests.RequestException:
            return None

    def wait_for_body(self, base_url: str, key: str, expected: str, timeout=None):
        """Poll one node until ``GET /get-val`` returns ``expected``.

        Returns ``(matched, last_body_seen)``.
        """
        deadline = REPLICATION_TIMEOUT if timeout is None else timeout
        seen = [None]

        def matches() -> bool:
            seen[0] = self.read(base_url, key)
            return seen[0] == expected

        matched = wait_until(matches, timeout=deadline, interval=POLL_INTERVAL)
        return bool(matched), seen[0]

    def wait_for_body_on_all(self, key: str, expected: str, timeout=None):
        """Poll every node; returns ``{base_url: last_body_seen}`` for mismatches."""
        mismatches = {}
        for url in self.nodes:
            matched, last = self.wait_for_body(url, key, expected, timeout=timeout)
            if not matched:
                mismatches[url] = last
        return mismatches


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------


@pytest.fixture(scope="session")
def nodes() -> list[str]:
    """Base URLs of every node under test."""
    urls = node_urls()
    if not urls:
        pytest.fail(
            "RAFTKV_NODES is set but empty; expected a comma-separated list of "
            "base URLs, e.g. RAFTKV_NODES=http://localhost:8080,http://localhost:8081",
            pytrace=False,
        )
    return urls


@pytest.fixture(scope="session")
def run_id() -> str:
    """Unique suffix for this run, so repeated runs never collide on keys."""
    return uuid.uuid4().hex[:12]


@pytest.fixture
def unique_key(request, run_id: str) -> str:
    """A key unique to this test *and* this run (URL-safe characters only)."""
    slug = re.sub(r"[^A-Za-z0-9]+", "-", request.node.name).strip("-")
    return f"e2e-{slug}-{run_id}-{uuid.uuid4().hex[:8]}"


@pytest.fixture
def unique_value() -> str:
    """A value that cannot be confused with any of the sentinel bodies."""
    return f"value-{uuid.uuid4().hex[:12]}"


@pytest.fixture(scope="session")
def leader(nodes: list[str], run_id: str) -> str:
    """Base URL of the current Raft leader, discovered by probing.

    The management API (:6000 ``/status``) is **not** published to the host by
    docker-compose.yml, so we cannot ask the cluster who leads. Instead we
    exploit the current no-forwarding contract: a write only succeeds on the
    leader. Followers answer ``error`` because ``raft.Apply`` returns
    ``ErrNotLeader`` (go-sidecar/internal/rpc/server.go) and the C++ handler
    turns a failed propose into the body ``error``.

    The probe is retried until ``RAFTKV_LEADER_TIMEOUT`` so a cluster still
    holding an election is tolerated.
    """
    last_round: list[str] = []

    def probe():
        last_round.clear()
        for url in nodes:
            probe_key = f"e2e-leader-probe-{run_id}-{uuid.uuid4().hex[:8]}"
            try:
                response = post_command(url, "SET", probe_key, "1")
            except requests.RequestException as exc:
                last_round.append(f"{url} -> unreachable ({type(exc).__name__}: {exc})")
                continue
            body = decode_body(response)
            last_round.append(f"{url} -> HTTP {response.status_code} body={body!r}")
            if response.status_code == 200 and body == BODY_OK:
                return url
        return None

    found = wait_until(probe, timeout=LEADER_TIMEOUT, interval=POLL_INTERVAL)
    if not found:
        detail = "\n  ".join(last_round) or "  (no nodes probed)"
        pytest.fail(
            "No node accepted a write within "
            f"{LEADER_TIMEOUT:g}s -- the RaftKV cluster does not look ready.\n"
            f"Probed nodes: {', '.join(nodes)}\n"
            f"Last probe round:\n  {detail}\n"
            "These tests do not manage docker. Start the cluster first:\n"
            "  docker build -t raftkv:latest . && docker compose up -d\n"
            "or point the suite elsewhere with RAFTKV_NODES=<comma-separated URLs>.",
            pytrace=False,
        )
    return found


@pytest.fixture(scope="session")
def cluster(nodes: list[str], leader: str) -> ClusterClient:
    """HTTP client for the cluster, with the leader already discovered."""
    return ClusterClient(nodes, leader)
