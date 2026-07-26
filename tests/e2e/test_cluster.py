"""End-to-end tests for a running 3-node RaftKV cluster.

Run against an already-running cluster; these tests never touch docker.

Two families of test live here:

* **R0.1-R0.5** -- the Phase 0 replication and rejection scenarios.
* **R1.7/R1.8** -- the Phase 1 HTTP contract: every failure carries a real
  status code, the matching reason phrase and a JSON body. Phase 0 pinned the
  opposite (HTTP 200 for everything, ``"error"`` / ``"Key Not Found"`` in the
  body); those pins were flipped here when Phase 1 landed.

What is *still* deliberately pinned is called out in each docstring: a write to
a follower is rejected rather than forwarded, and reads are served from the
local store and may be stale. Both change in Phase 4.

R0.5 (non-zero exit on failure) needs no code: it is pytest's default. Nothing
in this file catches an assertion or a transport error to soften a result.
"""

from __future__ import annotations

import pytest

from contracts import (
    ERROR_EMPTY_BODY,
    ERROR_KEY_NOT_FOUND,
    ERROR_MALFORMED_CONTENT_LENGTH,
    ERROR_MISSING_KEY_PARAM,
    ERROR_NOT_FOUND,
    ERROR_NOT_LEADER,
    ERROR_UNSUPPORTED_MEDIA_TYPE,
    HTTP_BAD_REQUEST,
    HTTP_NOT_FOUND,
    HTTP_OK,
    HTTP_SERVICE_UNAVAILABLE,
    HTTP_UNSUPPORTED_MEDIA_TYPE,
    JSON_CONTENT_TYPE,
    MSGPACK_CONTENT_TYPE,
    RAFT_ADDRESS_RE,
    REASON_PHRASES,
    TEXT_CONTENT_TYPE,
    WRITE_OK_BODY,
)

# --------------------------------------------------------------------------
# Assertion helpers
# --------------------------------------------------------------------------


def assert_write_accepted(cluster, response, base_url):
    """The committed-write contract: 200, JSON, ``{"ok":true}``."""
    assert response.status_code == HTTP_OK, (
        f"{base_url} answered HTTP {response.status_code} "
        f"({response.reason!r}) to a write it should have committed: "
        f"{cluster.body(response)!r}"
    )
    assert response.headers.get("Content-Type") == JSON_CONTENT_TYPE, (
        f"{base_url} answered a committed write with "
        f"Content-Type={response.headers.get('Content-Type')!r}"
    )
    assert cluster.json(response) == WRITE_OK_BODY, (
        f"{base_url} committed the write but answered "
        f"{cluster.body(response)!r} instead of the success envelope"
    )


def assert_error_envelope(cluster, response, status, message, context=""):
    """The standard failure contract: status, reason phrase, JSON, ``{"error": ...}``.

    All four are asserted together on purpose. The status code alone is not
    enough -- several paths answer 400, and checking only the code would let a
    request take the wrong branch unnoticed.

    ``context`` names the node (or whatever else) under test; it is prefixed to
    every message so a loop over nodes reports which one failed.
    """
    where = f"{context}: " if context else ""
    body = cluster.body(response)

    assert response.status_code == status, (
        f"{where}expected HTTP {status} ({REASON_PHRASES[status]}), got "
        f"{response.status_code} ({response.reason!r}) with body {body!r}"
    )
    # R1.7: the status line used to read "HTTP/1.1 404 OK" for every code.
    assert response.reason == REASON_PHRASES[status], (
        f"{where}HTTP {status} came back with the reason phrase "
        f"{response.reason!r}; expected {REASON_PHRASES[status]!r} (R1.7 -- a "
        "hardcoded phrase is exactly what this asserts against)"
    )
    assert response.headers.get("Content-Type") == JSON_CONTENT_TYPE, (
        f"{where}error bodies must be JSON; got "
        f"Content-Type={response.headers.get('Content-Type')!r} body={body!r}"
    )
    assert cluster.json(response) == {"error": message}, (
        f"{where}expected the error envelope {{'error': {message!r}}}, got {body!r}"
    )


# --------------------------------------------------------------------------
# R0.1 -- a write on the leader becomes readable on every node
# --------------------------------------------------------------------------


def test_r0_1_set_on_leader_replicates_to_all_nodes(cluster, unique_key, unique_value):
    """SET via the leader is readable on all three nodes within the deadline.

    This is the replication check ``test_client.py`` never made: it wrote to
    :8080, slept 0.3s and read back from :8080 only. Here every node is polled
    (no fixed sleep) until it serves the value.

    STILL PINNED (Phase 4): the polling is not politeness, it is the contract.
    Reads are answered from the local in-memory store with no read-index check,
    so a follower can legitimately serve a stale value until the entry is
    applied there, and there is no linearizable read mode to ask for instead.
    """
    response = cluster.set(cluster.leader, unique_key, unique_value)
    assert_write_accepted(cluster, response, cluster.leader)

    mismatches = cluster.wait_for_value_on_all(unique_key, unique_value)
    assert not mismatches, (
        f"key {unique_key!r} written on the leader ({cluster.leader}) did not "
        f"replicate to every node. Last observation per failing node: "
        f"{mismatches!r}. Nodes checked: {list(cluster.nodes)}"
    )

    # A hit is the one response that is not JSON: the stored bytes go back
    # verbatim as text/plain, with no envelope around them.
    hit = cluster.get(cluster.leader, unique_key)
    assert hit.status_code == HTTP_OK
    assert hit.reason == REASON_PHRASES[HTTP_OK]
    assert hit.headers.get("Content-Type") == TEXT_CONTENT_TYPE, (
        f"a successful read must be {TEXT_CONTENT_TYPE!r}, got "
        f"{hit.headers.get('Content-Type')!r}"
    )
    assert cluster.body(hit) == unique_value, (
        f"the value came back wrapped or altered: {cluster.body(hit)!r}"
    )


# --------------------------------------------------------------------------
# R0.2 -- DELETE round-trip
# --------------------------------------------------------------------------


def test_r0_2_delete_round_trip_removes_key_on_all_nodes(
    cluster, unique_key, unique_value
):
    """SET, confirm everywhere, DELETE on the leader, gone everywhere."""
    set_response = cluster.set(cluster.leader, unique_key, unique_value)
    assert_write_accepted(cluster, set_response, cluster.leader)

    before = cluster.wait_for_value_on_all(unique_key, unique_value)
    assert not before, (
        f"precondition failed: {unique_key!r} did not replicate before the DELETE. "
        f"Last observation per failing node: {before!r}"
    )

    delete_response = cluster.delete(cluster.leader, unique_key)
    assert_write_accepted(cluster, delete_response, cluster.leader)

    # A deleted key is indistinguishable from a never-written one: both are a
    # 404 with the same envelope. That is intentional -- the store has no
    # tombstones -- but it means this assertion alone cannot tell "deleted"
    # from "never existed"; the `before` check above is what makes it mean
    # something.
    after = cluster.wait_for_missing_on_all(unique_key)
    assert not after, (
        f"key {unique_key!r} was still readable after DELETE on the leader "
        f"({cluster.leader}). Last observation per failing node: {after!r}"
    )


# --------------------------------------------------------------------------
# R0.3 -- missing key (Phase 1 contract: 404)
# --------------------------------------------------------------------------


def test_r0_3_get_missing_key_returns_404_not_found(cluster, unique_key):
    """GET of a never-written key: HTTP 404 with ``{"error":"key not found"}``.

    GUARANTEED SINCE PHASE 1 (R1.8). Phase 0 pinned the opposite -- ``handle_get``
    returned ``HttpResponse::ok("Key Not Found")``, so a miss was a 200 and the
    status code carried no information at all. It is now a real 404 with the
    JSON envelope, and the reason phrase reads "Not Found" rather than "OK".
    """
    for base_url in cluster.nodes:
        response = cluster.get(base_url, unique_key)
        assert_error_envelope(
            cluster,
            response,
            HTTP_NOT_FOUND,
            ERROR_KEY_NOT_FOUND,
            context=f"{base_url} for never-written key {unique_key!r}",
        )


# --------------------------------------------------------------------------
# R0.4 -- writes to a follower are not forwarded
# --------------------------------------------------------------------------


def test_r0_4_set_on_follower_returns_503_naming_the_leader(
    cluster, unique_key, unique_value
):
    """SET sent to a follower: HTTP 503 with the leader's Raft address.

    GUARANTEED SINCE PHASE 1 (R1.5 + R1.8): ``raft.Apply`` returns
    ``ErrNotLeader``, ``rpc.Server.Propose`` tags it ``not_leader:<addr>`` with
    the address from ``LeaderWithID``, and the C++ handler turns that prefix
    into a 503 carrying the address. Phase 0 pinned this as HTTP 200 with the
    body ``error``, indistinguishable from "the sidecar is down" (which is now
    a 502).

    STILL PINNED (Phase 4): the rejection itself. There is no leader
    forwarding, so a client has to retry against the address in the ``leader``
    field. That address is the peer's **Raft** address (``node1:8088`` under
    docker-compose.yml, where entrypoint.sh advertises the container hostname),
    not an HTTP endpoint a client can dial -- which is why this asserts a shape
    rather than a usable URL.
    """
    if not cluster.followers:
        pytest.skip(
            "single-node cluster (leader is the only node in "
            f"{list(cluster.nodes)}) -- no follower to reject a write"
        )

    for base_url in cluster.followers:
        response = cluster.set(base_url, unique_key, unique_value)

        assert response.status_code == HTTP_SERVICE_UNAVAILABLE, (
            f"follower {base_url} answered HTTP {response.status_code} "
            f"({response.reason!r}) body={cluster.body(response)!r}; expected "
            f"{HTTP_SERVICE_UNAVAILABLE} for a write it cannot commit. If this "
            f"says {HTTP_OK}, leadership moved off {cluster.leader} mid-run"
        )
        assert response.reason == REASON_PHRASES[HTTP_SERVICE_UNAVAILABLE], (
            f"follower {base_url} answered with the reason phrase "
            f"{response.reason!r} (R1.7)"
        )
        assert response.headers.get("Content-Type") == JSON_CONTENT_TYPE, (
            f"follower {base_url} answered Content-Type="
            f"{response.headers.get('Content-Type')!r}"
        )

        payload = cluster.json(response)
        assert payload.get("error") == ERROR_NOT_LEADER, (
            f"follower {base_url} answered {cluster.body(response)!r}; expected "
            "the not-leader envelope. A different message here means a propose "
            "that failed for some other reason was mis-mapped onto 503 instead "
            "of 502"
        )

        leader_addr = payload.get("leader", "")
        assert leader_addr, (
            f"follower {base_url} returned a 503 with an empty 'leader' field. "
            "The sidecar emits a bare 'not_leader:' only while no leader is "
            "known -- but this run already discovered a leader at "
            f"{cluster.leader}, so an election in flight is the only benign "
            "explanation. If it reproduces, LeaderWithID is not reaching the "
            "response (go-sidecar/internal/rpc/server.go)"
        )
        assert RAFT_ADDRESS_RE.match(leader_addr), (
            f"follower {base_url} named the leader as {leader_addr!r}, which is "
            "not a host:port Raft address. It comes from raft's LeaderWithID "
            "and under docker-compose.yml should look like 'node1:8088'"
        )

    # The rejected write must not have reached any node's store, including the
    # follower that refused it. One probe each, no polling: there is nothing to
    # wait for, and waiting would only hide a slow leak of the entry.
    leaked = cluster.wait_for_missing_on_all(unique_key, timeout=0)
    assert not leaked, (
        f"the only write for {unique_key!r} was rejected by a follower, yet "
        f"some node has state for it: {leaked!r}"
    )


# --------------------------------------------------------------------------
# R1.8 -- the malformed-Content-Length remote DoS (regression test)
# --------------------------------------------------------------------------

_DOS_EXPLANATION = (
    "REGRESSION -- this is the remote denial of service found in Phase 0 and "
    "fixed by R1.8.\n"
    "HttpRequestParser::parse called std::stoi on the Content-Length value. On "
    "garbage it threw std::invalid_argument (and on a huge number, "
    "std::out_of_range); the exception unwound out of "
    "HttpServer::handle_connection, out of run(), into main.cpp's catch-all, "
    "which printed 'Fatal error: stoi' and exited 1. entrypoint.sh exits when "
    "either process dies and docker-compose.yml declares no restart policy, so "
    "one unauthenticated header permanently killed a node.\n"
    "The fix has two halves, both required:\n"
    "  1. cpp-app/src/network/http_request.hpp catches std::invalid_argument "
    "AND std::out_of_range around the stoi, sets HttpRequest::bad_content_length "
    "and leaves content_length at 0, so parse() is total;\n"
    "  2. KVHttpHandler::handle_insert turns that flag into a 400 "
    '{"error":"malformed Content-Length"}.'
)


@pytest.mark.parametrize(
    "bad_length",
    [
        pytest.param("abc", id="not-a-number"),
        pytest.param("", id="empty"),
        pytest.param("99999999999999999999", id="wider-than-int"),
        pytest.param("-1", id="negative"),
        pytest.param("-2147483648", id="most-negative-int"),
    ],
)
def test_r1_8_malformed_content_length_is_rejected_without_killing_the_node(
    cluster, unique_key, unique_value, bad_length
):
    """A malformed ``Content-Length`` is a 400, and every node survives it.

    The highest-value test in Phase 1: it is the only one whose failure means
    an unauthenticated stranger can take the cluster down. ``requests`` cannot
    express this request -- it derives ``Content-Length`` from the body -- so
    the bytes are handcrafted and sent over a raw socket.

    Each node is poisoned in turn and then asked to serve a read, because a
    crash here is permanent: entrypoint.sh exits with the dead process and
    docker-compose.yml declares no ``restart:`` policy, so the port stays shut
    for the rest of the session.

    The negative values are a distinct failure mode and were missed by the
    first version of this test. ``std::stoi("-1")`` does not throw, so the
    exception handling that fixed ``abc`` does not cover it; the value flows
    through to ``static_cast<size_t>(content_length)`` in the body top-up loop,
    becomes 18446744073709551615, and the loop blocks in ``recv()`` until the
    peer disconnects. That does not crash the node -- it *wedges* it, silently,
    while the container still reports healthy. It shows up here as ``send_raw``
    timing out rather than as a refused connection.
    """
    for base_url in cluster.nodes:
        request = cluster.malformed_content_length_request(base_url, bad_length)

        try:
            response = cluster.send_raw(base_url, request)
        except (OSError, ValueError) as exc:
            pytest.fail(
                f"{base_url} did not answer a request carrying "
                f"'Content-Length: {bad_length}' "
                f"({type(exc).__name__}: {exc}).\n"
                f"Request sent:\n{request!r}\n\n{_DOS_EXPLANATION}",
                pytrace=False,
            )

        assert response.status == HTTP_BAD_REQUEST, (
            f"{base_url} answered {response} to 'Content-Length: {bad_length}'; "
            f"expected {HTTP_BAD_REQUEST}.\n\n{_DOS_EXPLANATION}"
        )
        assert response.reason == REASON_PHRASES[HTTP_BAD_REQUEST], (
            f"{base_url} answered {response}; expected the reason phrase "
            f"{REASON_PHRASES[HTTP_BAD_REQUEST]!r} (R1.7)"
        )
        # The message matters as much as the code. If the parser stopped
        # flagging the bad header, content_length would fall back to 0, the
        # empty body would trip the *next* check, and the answer would still be
        # a 400 -- with "empty request body" as the reason. Asserting the
        # message is what keeps this test honest about which branch replied.
        assert response.json() == {"error": ERROR_MALFORMED_CONTENT_LENGTH}, (
            f"{base_url} answered {response}; expected the envelope "
            f"{{'error': {ERROR_MALFORMED_CONTENT_LENGTH!r}}}. A body of "
            f"{{'error': {ERROR_EMPTY_BODY!r}}} means the parser silently "
            "ignored the malformed Content-Length instead of flagging it"
        )

        # The point of the test: the process must still be there afterwards.
        alive = cluster.probe_read(base_url, unique_key)
        if not alive.reachable:
            pytest.fail(
                f"{base_url} stopped serving reads after receiving "
                f"'Content-Length: {bad_length}' ({alive.body})."
                f"\n\n{_DOS_EXPLANATION}",
                pytrace=False,
            )
        assert alive.status == HTTP_NOT_FOUND, (
            f"{base_url} is still answering after the malformed request, but "
            f"served {alive} for a never-written key"
        )

    # Every node took the poison header; the cluster must still do its job.
    # This exercises the sidecars and the raft path too, not just the HTTP loop.
    write = cluster.set(cluster.leader, unique_key, unique_value)
    assert_write_accepted(cluster, write, cluster.leader)

    mismatches = cluster.wait_for_value_on_all(unique_key, unique_value)
    assert not mismatches, (
        "after every node received a malformed Content-Length, a write on the "
        f"leader ({cluster.leader}) no longer replicates everywhere. Last "
        f"observation per failing node: {mismatches!r}"
    )


# --------------------------------------------------------------------------
# R1.8 -- the remaining request-validation status codes
# --------------------------------------------------------------------------


def test_r1_8_wrong_content_type_returns_415(cluster, unique_key):
    """POST /insert-val without the msgpack media type: 415, naming what it wants.

    Routing is on method and path only, so a wrong ``Content-Type`` reaches the
    insert handler and earns a 415. Before Phase 1 the media type was part of
    the route match, so this fell through to ``HttpResponse::not_found()`` and
    came back as a 404 with the plain body ``404 Not Found`` -- on the
    malformed status line ``HTTP/1.1 404 OK``, since the reason phrase was
    hardcoded. (``200`` + ``Key Not Found`` was the read path, not this one.)

    The 415 body carries a second member (``expected``), so this does not go
    through :func:`assert_error_envelope`.
    """
    for base_url in cluster.nodes:
        response = cluster.post_payload(
            base_url, b"not-msgpack", content_type="application/json"
        )

        assert response.status_code == HTTP_UNSUPPORTED_MEDIA_TYPE, (
            f"{base_url} answered HTTP {response.status_code} "
            f"({response.reason!r}) body={cluster.body(response)!r}"
        )
        assert response.reason == REASON_PHRASES[HTTP_UNSUPPORTED_MEDIA_TYPE], (
            f"{base_url} answered with the reason phrase {response.reason!r} (R1.7)"
        )
        assert response.headers.get("Content-Type") == JSON_CONTENT_TYPE, (
            f"{base_url} answered Content-Type="
            f"{response.headers.get('Content-Type')!r}"
        )
        assert cluster.json(response) == {
            "error": ERROR_UNSUPPORTED_MEDIA_TYPE,
            "expected": MSGPACK_CONTENT_TYPE,
        }, (
            f"{base_url} answered {cluster.body(response)!r}; the 415 body must "
            "name the media type the endpoint does accept"
        )

    # Nothing reached Raft, so no node may have state for the key.
    leaked = cluster.wait_for_missing_on_all(unique_key, timeout=0)
    assert not leaked, (
        f"a request rejected with 415 left state for {unique_key!r}: {leaked!r}"
    )


def test_r1_8_empty_body_returns_400(cluster):
    """POST /insert-val with the right media type and no body: 400.

    An empty body is a client mistake, not something to hand to Raft: the
    handler rejects it before proposing, so this costs the cluster nothing even
    though it is sent to every node.
    """
    for base_url in cluster.nodes:
        response = cluster.post_payload(base_url, b"")
        assert_error_envelope(
            cluster, response, HTTP_BAD_REQUEST, ERROR_EMPTY_BODY, context=base_url
        )


def test_r1_8_get_without_key_parameter_returns_400(cluster):
    """GET /get-val with no query string at all: 400, naming the parameter.

    Phase 0 pinned this as a 200 with ``Key Not Found`` -- the same answer as a
    genuine miss, so a client could not tell a typo in the query string from an
    absent key.
    """
    for base_url in cluster.nodes:
        response = cluster.visit(base_url, "/get-val")
        assert_error_envelope(
            cluster,
            response,
            HTTP_BAD_REQUEST,
            ERROR_MISSING_KEY_PARAM,
            context=base_url,
        )


def test_r1_8_unknown_route_returns_404(cluster):
    """Any other path: 404 with the JSON envelope, not a 200 with a sentence."""
    for base_url in cluster.nodes:
        response = cluster.visit(base_url, "/no-such-route")
        assert_error_envelope(
            cluster, response, HTTP_NOT_FOUND, ERROR_NOT_FOUND, context=base_url
        )
