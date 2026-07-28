"""Phase 6 security contract, against a running cluster.

Two things are checked here, and neither is provable from a unit test:

*   R6.9 — the Phase 4 request caps. A unit test can show that the parser
    rejects an oversized body it was handed; only a real socket can show that a
    node stays up while someone tries to hand it one.
*   R6.1/R6.2 — cluster membership is closed. The critical assertion is not the
    status code, it is that a rejected /join did NOT add the peer. A middleware
    that returned 403 after calling AddVoter would pass a status-code-only test
    and leave the cluster compromised.

These run against the default (plaintext) compose profile, which is what CI
brings up. The TLS profile has its own module.
"""

from __future__ import annotations

import os
import socket
import time

import pytest
import requests

import contracts
from conftest import (
    HTTP_TIMEOUT,
    mgmt_urls_for,
    parse_raw_response,
    try_json,
)

# The management API is a local admin endpoint; it answers immediately or not at
# all, so it shares the client timeout rather than getting its own knob.
MGMT_TIMEOUT = HTTP_TIMEOUT


# The token docker-compose.yml falls back to when RAFTKV_MGMT_TOKEN is unset.
# The tests read the environment first so a CI run with a generated token works;
# this is the laptop default.
DEV_TOKEN = "dev-only-insecure-token"


def _token() -> str:
    return os.environ.get("RAFTKV_MGMT_TOKEN", DEV_TOKEN)


# ---------------------------------------------------------------------------
# R6.9 — request caps
# ---------------------------------------------------------------------------


def test_oversized_body_is_rejected_with_413(nodes):
    """A body over the cap gets 413, and the node survives it.

    The surviving part is the point. Phase 1 shipped a node that could be killed
    by a malformed Content-Length; the cap is worthless if exceeding it wedges
    the accept loop instead of answering.
    """
    url = nodes[0]
    oversized = b"x" * (contracts.MAX_BODY_BYTES + 1024)

    response = requests.post(
        f"{url}/insert-val",
        data=oversized,
        headers={"Content-Type": contracts.MSGPACK_CONTENT_TYPE},
        timeout=HTTP_TIMEOUT * 4,
    )

    assert response.status_code == contracts.HTTP_PAYLOAD_TOO_LARGE, (
        f"a {len(oversized)}-byte body returned {response.status_code}; "
        f"the cap is {contracts.MAX_BODY_BYTES} bytes"
    )
    body = try_json(response.text)
    assert body is not None, f"413 body was not JSON: {response.text!r}"
    assert contracts.ERROR_BODY_TOO_LARGE in body.get("error", "")

    # Still serving. If the oversized request had wedged the node, this is where
    # it would show up.
    follow_up = requests.get(f"{url}/get-val", params={"key": "nonexistent"}, timeout=HTTP_TIMEOUT)
    assert follow_up.status_code == contracts.HTTP_NOT_FOUND, (
        "the node stopped answering after an oversized request"
    )


def test_body_just_under_the_cap_is_not_rejected(nodes):
    """The cap must not be so eager that a legitimate request trips it.

    Without this, 'return 413 unconditionally' would pass the test above. What
    this asserts is only that the request got PAST the size check — a 1 MiB blob
    of 'x' is not valid MsgPack, so it is refused later, on content rather than
    on length.
    """
    url = nodes[0]
    just_under = b"x" * (contracts.MAX_BODY_BYTES - 1024)

    response = requests.post(
        f"{url}/insert-val",
        data=just_under,
        headers={"Content-Type": contracts.MSGPACK_CONTENT_TYPE},
        timeout=HTTP_TIMEOUT * 4,
    )

    assert response.status_code != contracts.HTTP_PAYLOAD_TOO_LARGE, (
        f"a {len(just_under)}-byte body was rejected as too large, but the cap is "
        f"{contracts.MAX_BODY_BYTES}"
    )


def test_oversized_headers_are_rejected_with_431(nodes):
    """Headers over the cap get 431 before a body is ever read.

    Sent over a raw socket because requests will not build a request this
    hostile, and because the interesting property is that the node answers at
    all rather than reading until it runs out of memory.
    """
    url = nodes[0]
    host, port = _host_port(url)

    filler = b"x" * 1024
    header_lines = b"".join(
        b"X-Pad-%d: %s\r\n" % (i, filler)
        for i in range((contracts.MAX_HEADER_BYTES // 1024) + 8)
    )
    request = b"GET /get-val?key=a HTTP/1.1\r\nHost: x\r\n" + header_lines + b"\r\n"

    raw = _send_raw(host, port, request, timeout=HTTP_TIMEOUT * 4)
    assert raw, "the node closed the connection without answering an oversized header block"

    response = parse_raw_response(raw)
    assert response.status == contracts.HTTP_HEADERS_TOO_LARGE, (
        f"oversized headers returned {response.status}, want "
        f"{contracts.HTTP_HEADERS_TOO_LARGE}"
    )


def test_node_survives_a_burst_of_oversized_requests(nodes):
    """Ten oversized bodies in a row, then a normal request must still work.

    One rejected request proves the check exists; a burst proves the rejection
    path does not leak the buffer it refused to keep.
    """
    url = nodes[0]
    oversized = b"x" * (contracts.MAX_BODY_BYTES + 1024)

    for _ in range(10):
        response = requests.post(
            f"{url}/insert-val",
            data=oversized,
            headers={"Content-Type": contracts.MSGPACK_CONTENT_TYPE},
            timeout=HTTP_TIMEOUT * 4,
        )
        assert response.status_code == contracts.HTTP_PAYLOAD_TOO_LARGE

    follow_up = requests.get(f"{url}/get-val", params={"key": "nonexistent"}, timeout=HTTP_TIMEOUT)
    assert follow_up.status_code == contracts.HTTP_NOT_FOUND


# ---------------------------------------------------------------------------
# R6.1 / R6.2 — cluster membership is closed
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "headers,expected,label",
    [
        ({}, 401, "no credential"),
        ({"Authorization": "Bearer wrong-token"}, 403, "wrong token"),
        ({"Authorization": "wrong-token"}, 401, "no Bearer scheme"),
        ({"Authorization": "Bearer "}, 401, "empty token"),
    ],
)
def test_join_without_a_valid_token_is_refused(nodes, headers, expected, label):
    mgmt = mgmt_urls_for(nodes)[0]

    response = requests.get(
        f"{mgmt}/join",
        params={"peerID": "intruder", "peerAddress": "intruder:8088"},
        headers=headers,
        timeout=MGMT_TIMEOUT,
    )

    assert response.status_code == expected, (
        f"/join with {label} returned {response.status_code}, want {expected}"
    )


def test_refused_join_does_not_add_the_peer(nodes):
    """The assertion that actually matters.

    A status code says what the caller was told; the Raft log says what happened.
    AddVoter appends a configuration entry, so if an unauthenticated join had
    been carried out anyway, last_log_index would move. Checked against a quiet
    cluster so nothing else is writing.
    """
    mgmt = mgmt_urls_for(nodes)[0]

    before = _last_log_index(mgmt)

    for headers in ({}, {"Authorization": "Bearer wrong-token"}):
        requests.get(
            f"{mgmt}/join",
            params={"peerID": "intruder", "peerAddress": "intruder:8088"},
            headers=headers,
            timeout=MGMT_TIMEOUT,
        )

    # Give a rogue AddVoter time to commit, so this is not merely a race the
    # test happens to win.
    time.sleep(1.0)
    after = _last_log_index(mgmt)

    assert after == before, (
        f"last_log_index moved from {before} to {after} after two REFUSED joins: "
        "the peer was added anyway, and the rejection was cosmetic"
    )


def test_valid_token_passes_authentication(nodes):
    """A correct token must get past auth.

    Deliberately sent WITHOUT peerID/peerAddress so the request is rejected on
    its parameters (400) rather than on its credential. That proves the token was
    accepted without mutating cluster membership — an actual join here would add
    a phantom voter and break every test that runs after it.
    """
    mgmt = mgmt_urls_for(nodes)[0]

    response = requests.get(
        f"{mgmt}/join",
        headers={"Authorization": f"Bearer {_token()}"},
        timeout=MGMT_TIMEOUT,
    )

    assert response.status_code == contracts.HTTP_BAD_REQUEST, (
        f"/join with a valid token but no parameters returned {response.status_code}; "
        f"want {contracts.HTTP_BAD_REQUEST}, which means the credential was accepted "
        "and the request failed on its contents"
    )


@pytest.mark.parametrize("path", ["/health", "/ready", "/status", "/metrics"])
def test_read_only_endpoints_stay_unauthenticated(nodes, path):
    """Probes and scrapers hold no credential; gating these would break them."""
    mgmt = mgmt_urls_for(nodes)[0]

    response = requests.get(f"{mgmt}{path}", timeout=MGMT_TIMEOUT)

    assert response.status_code not in (401, 403), (
        f"{path} requires authentication ({response.status_code}); container health "
        "probes and Prometheus cannot supply one"
    )


def test_remove_is_also_guarded(nodes):
    """/remove is as dangerous as /join — it can shrink the cluster below quorum."""
    mgmt = mgmt_urls_for(nodes)[0]

    response = requests.get(
        f"{mgmt}/remove",
        params={"peerID": "node2"},
        timeout=MGMT_TIMEOUT,
    )

    assert response.status_code == 401, (
        f"/remove without a credential returned {response.status_code}; an open "
        "/remove lets anyone take the cluster below quorum"
    )


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def _last_log_index(mgmt_url: str) -> int:
    response = requests.get(f"{mgmt_url}/status", timeout=MGMT_TIMEOUT)
    response.raise_for_status()
    return int(response.json()["last_log_index"])


def _host_port(base_url: str) -> tuple[str, int]:
    without_scheme = base_url.split("://", 1)[-1]
    host, _, port = without_scheme.partition(":")
    return host, int(port or 80)


def _send_raw(host: str, port: int, request: bytes, timeout: float) -> bytes:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request)
        chunks = []
        while True:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            chunks.append(chunk)
            # One response is enough; the server closes after it (no keep-alive).
            if b"\r\n\r\n" in b"".join(chunks):
                break
    return b"".join(chunks)
