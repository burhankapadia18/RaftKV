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

import json
import os
import re
import socket
import time
import urllib.parse
import uuid
from typing import NamedTuple

import msgpack
import pytest
import requests

# The status codes, media types and message strings the C++ engine actually
# emits live in contracts.py, imported by the tests as well so the two cannot
# drift.
from contracts import (
    ERROR_KEY_NOT_FOUND,
    HTTP_NOT_FOUND,
    HTTP_OK,
    MSGPACK_CONTENT_TYPE,
    WRITE_OK_BODY,
)

# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

DEFAULT_NODES = "http://localhost:8080,http://localhost:8081,http://localhost:8082"

#: Read chunk size for the raw-socket helper. Nothing this suite sends produces
#: a response anywhere near this large; the loop runs until the peer closes.
RECV_CHUNK = 4096


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

    The server does declare a ``Content-Type`` now, but ``requests`` only reads
    a charset out of it for ``text/*``; for ``application/json`` it falls back
    to encoding *detection*. Every body this suite asserts on is UTF-8, so
    decode it explicitly rather than letting a heuristic decide.
    """
    return response.content.decode("utf-8")


def try_json(text: str):
    """Parse ``text`` as JSON, or return ``None`` if it is not valid JSON."""
    try:
        return json.loads(text)
    except ValueError:
        return None


def post_command(base_url: str, op: str, key: str, value: str = "") -> requests.Response:
    """POST a MsgPack command to ``/insert-val`` on one node.

    Deliberately uses the module-level ``requests`` API (a fresh connection per
    call) rather than a ``requests.Session``: the C++ HTTP server closes the
    socket after every response without sending ``Connection: close``, so a
    pooled keep-alive connection would be reused after the peer hung up.
    """
    return post_payload(base_url, pack_command(op, key, value))


def post_payload(
    base_url: str, payload: bytes, content_type: str = MSGPACK_CONTENT_TYPE
) -> requests.Response:
    """POST arbitrary bytes to ``/insert-val`` with an explicit media type.

    Used by the error-path tests, which need a body or a ``Content-Type`` that
    ``post_command`` would never produce.
    """
    return requests.post(
        f"{base_url}/insert-val",
        data=payload,
        headers={"Content-Type": content_type},
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
# Raw socket access
# --------------------------------------------------------------------------


class RawResponse(NamedTuple):
    """An HTTP response read straight off a socket."""

    status: int
    reason: str
    headers: dict
    body: str

    def json(self):
        """Body parsed as JSON, or ``None`` if it is not JSON."""
        return try_json(self.body)

    def __str__(self) -> str:
        return f"HTTP {self.status} {self.reason} body={self.body!r}"


def parse_raw_response(raw: bytes) -> RawResponse:
    """Parse the bytes of a complete HTTP response.

    Raises ``ValueError`` when the peer sent nothing or something that is not a
    status line -- both of which mean "the node did not answer", which is
    exactly what the R1.8 regression test needs to distinguish from a 400.
    """
    if not raw:
        raise ValueError("peer closed the connection without sending a response")

    head, separator, body = raw.partition(b"\r\n\r\n")
    if not separator:
        raise ValueError(f"response has no header terminator: {raw!r}")

    lines = head.split(b"\r\n")
    status_line = lines[0].decode("latin-1")
    parts = status_line.split(" ", 2)
    if len(parts) < 2 or not parts[1].isdigit():
        raise ValueError(f"not an HTTP status line: {status_line!r}")

    headers = {}
    for line in lines[1:]:
        name, colon, value = line.decode("latin-1").partition(":")
        if colon:
            headers[name.strip().lower()] = value.strip()

    reason = parts[2] if len(parts) > 2 else ""
    return RawResponse(int(parts[1]), reason, headers, body.decode("utf-8", "replace"))


def raw_http_request(base_url: str, request: bytes, timeout=None) -> RawResponse:
    """Send raw bytes to a node's HTTP port and read the whole response.

    ``requests`` cannot express a malformed request -- it builds
    ``Content-Length`` itself from the body -- so the R1.8 regression test has
    to speak the wire directly. The server closes the connection after every
    response, so reading to EOF is the framing.
    """
    parsed = urllib.parse.urlsplit(base_url)
    deadline = HTTP_TIMEOUT if timeout is None else timeout

    chunks = []
    with socket.create_connection(
        (parsed.hostname, parsed.port or 80), timeout=deadline
    ) as sock:
        sock.sendall(request)
        while True:
            chunk = sock.recv(RECV_CHUNK)
            if not chunk:
                break
            chunks.append(chunk)

    return parse_raw_response(b"".join(chunks))


# --------------------------------------------------------------------------
# Cluster client
# --------------------------------------------------------------------------


class ReadResult(NamedTuple):
    """What one ``GET /get-val`` attempt observed.

    ``status`` is ``None`` when the request never completed; ``body`` then
    carries the transport error instead of a response body.
    """

    status: int | None
    body: str

    @property
    def reachable(self) -> bool:
        return self.status is not None

    def __str__(self) -> str:
        if self.status is None:
            return f"unreachable ({self.body})"
        return f"HTTP {self.status} body={self.body!r}"


def _is_value(expected: str):
    """Predicate: the node serves ``expected`` as a 200 plain-text body."""

    def matches(result: ReadResult) -> bool:
        return result.status == HTTP_OK and result.body == expected

    return matches


def _is_missing(result: ReadResult) -> bool:
    """Predicate: the node reports the key as absent (404 + the JSON envelope)."""
    if result.status != HTTP_NOT_FOUND:
        return False
    payload = try_json(result.body)
    return isinstance(payload, dict) and payload.get("error") == ERROR_KEY_NOT_FOUND


class ClusterClient:
    """Thin HTTP client bound to a discovered cluster topology.

    Everything the tests need to talk to the cluster hangs off this object, so
    ``test_cluster.py`` imports only ``contracts`` and never ``conftest`` by
    name (see the note at the top of contracts.py).
    """

    def __init__(self, nodes, leader: str) -> None:
        self.nodes = tuple(nodes)
        self.leader = leader
        self.followers = tuple(url for url in nodes if url != leader)

    # -- decoding ----------------------------------------------------------

    @staticmethod
    def body(response: requests.Response) -> str:
        """Response body as text (see :func:`decode_body`)."""
        return decode_body(response)

    @staticmethod
    def json(response: requests.Response) -> dict:
        """Response body parsed as a JSON object, or a clear failure.

        Failing here rather than raising ``JSONDecodeError`` keeps the raw body
        in the report, which is what you need when the handler answered with
        something unexpected.
        """
        text = decode_body(response)
        payload = try_json(text)
        if not isinstance(payload, dict):
            pytest.fail(
                f"expected a JSON object body from {response.url}, got "
                f"HTTP {response.status_code} "
                f"Content-Type={response.headers.get('Content-Type')!r} "
                f"body={text!r}",
                pytrace=False,
            )
        return payload

    # -- writes ------------------------------------------------------------

    def set(self, base_url: str, key: str, value: str) -> requests.Response:
        return post_command(base_url, "SET", key, value)

    def delete(self, base_url: str, key: str) -> requests.Response:
        return post_command(base_url, "DELETE", key, "")

    def post_payload(
        self, base_url: str, payload: bytes, content_type: str = MSGPACK_CONTENT_TYPE
    ) -> requests.Response:
        """POST arbitrary bytes to ``/insert-val`` (error-path tests)."""
        return post_payload(base_url, payload, content_type)

    # -- reads -------------------------------------------------------------

    def get(self, base_url: str, key: str) -> requests.Response:
        return get_value(base_url, key)

    @staticmethod
    def visit(base_url: str, path: str) -> requests.Response:
        """GET an arbitrary path, for the routes with no happy path at all."""
        return requests.get(f"{base_url}{path}", timeout=HTTP_TIMEOUT)

    def probe_read(self, base_url: str, key: str) -> ReadResult:
        """One ``GET /get-val``, with transport errors folded into the result.

        Transport errors are swallowed only so that polling survives a node
        that is momentarily busy; the caller's deadline still expires and the
        assertion still fails, reporting the last observation.
        """
        try:
            response = self.get(base_url, key)
        except requests.RequestException as exc:
            return ReadResult(None, f"{type(exc).__name__}: {exc}")
        return ReadResult(response.status_code, decode_body(response))

    # -- raw wire ----------------------------------------------------------

    @staticmethod
    def send_raw(base_url: str, request: bytes) -> RawResponse:
        """Send handcrafted request bytes to a node (see :func:`raw_http_request`)."""
        return raw_http_request(base_url, request)

    @staticmethod
    def malformed_content_length_request(base_url: str, value: str = "abc") -> bytes:
        """A ``POST /insert-val`` whose ``Content-Length`` is not a number.

        ``requests`` will not emit this: it computes ``Content-Length`` from the
        body. Byte-for-byte construction is the only way to reproduce the
        Phase 0 crash.
        """
        host = urllib.parse.urlsplit(base_url).netloc
        return (
            "POST /insert-val HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Content-Type: {MSGPACK_CONTENT_TYPE}\r\n"
            f"Content-Length: {value}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).encode("ascii")

    # -- polling -----------------------------------------------------------

    def wait_for(self, base_url: str, key: str, matches, timeout=None):
        """Poll one node's ``GET /get-val`` until ``matches(ReadResult)`` holds.

        Returns ``(matched, last_observation)``.
        """
        deadline = REPLICATION_TIMEOUT if timeout is None else timeout
        seen = [ReadResult(None, "never probed")]

        def check() -> bool:
            seen[0] = self.probe_read(base_url, key)
            return matches(seen[0])

        matched = wait_until(check, timeout=deadline, interval=POLL_INTERVAL)
        return bool(matched), seen[0]

    def wait_for_value_on_all(self, key: str, expected: str, timeout=None) -> dict:
        """Poll every node until it serves ``expected``.

        Returns ``{base_url: last_observation}`` for the nodes that never did --
        empty means every node converged.
        """
        return self._wait_on_all(key, _is_value(expected), timeout)

    def wait_for_missing_on_all(self, key: str, timeout=None) -> dict:
        """Poll every node until it reports ``key`` as absent (404)."""
        return self._wait_on_all(key, _is_missing, timeout)

    def _wait_on_all(self, key: str, matches, timeout) -> dict:
        mismatches = {}
        for url in self.nodes:
            matched, last = self.wait_for(url, key, matches, timeout=timeout)
            if not matched:
                mismatches[url] = str(last)
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
    leader. Since Phase 1 that is unambiguous on the wire -- the leader answers
    ``200 {"ok":true}`` and a follower answers ``503`` with
    ``{"error":"not leader","leader":"<raft address>"}``, because
    ``raft.Apply`` returns ``ErrNotLeader``, the sidecar tags it
    ``not_leader:<addr>`` (go-sidecar/internal/rpc/server.go) and the C++
    handler maps that prefix to 503.

    The probe deliberately keys off the success shape rather than "not a 503",
    so a node answering 502 (sidecar unreachable) is never mistaken for a
    leader. It is retried until ``RAFTKV_LEADER_TIMEOUT`` so a cluster still
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
            if response.status_code == HTTP_OK and try_json(body) == WRITE_OK_BODY:
                return url
        return None

    found = wait_until(probe, timeout=LEADER_TIMEOUT, interval=POLL_INTERVAL)
    if not found:
        detail = "\n  ".join(last_round) or "  (no nodes probed)"
        pytest.fail(
            "No node accepted a write within "
            f"{LEADER_TIMEOUT:g}s -- the RaftKV cluster does not look ready.\n"
            f"Expected exactly one node to answer HTTP 200 {WRITE_OK_BODY!r}; "
            "a follower answers 503 {'error': 'not leader', ...} and a node "
            "whose sidecar is down answers 502.\n"
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
