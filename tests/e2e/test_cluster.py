"""End-to-end tests for a running 3-node RaftKV cluster (spec R0.1-R0.5).

Run against an already-running cluster; these tests never touch docker.

Some assertions here **pin current behavior that is known to be wrong**. Phase 0
is a safety net, not a fix: a test that asserts the buggy-but-real contract is
correct at this point. Each such assertion names the phase that will change it.

R0.5 (non-zero exit on failure) needs no code: it is pytest's default. Nothing
in this file catches an assertion or a transport error to soften a result.
"""

from __future__ import annotations

import pytest

from contracts import BODY_ERROR, BODY_KEY_NOT_FOUND, BODY_OK


# --------------------------------------------------------------------------
# R0.1 -- a write on the leader becomes readable on every node
# --------------------------------------------------------------------------


def test_r0_1_set_on_leader_replicates_to_all_nodes(cluster, unique_key, unique_value):
    """SET via the leader is readable on all three nodes within the deadline.

    This is the replication check ``test_client.py`` never made: it wrote to
    :8080, slept 0.3s and read back from :8080 only. Here every node is polled
    (no fixed sleep) until it serves the value.
    """
    response = cluster.set(cluster.leader, unique_key, unique_value)

    assert response.status_code == 200
    assert cluster.body(response) == BODY_OK, (
        f"leader {cluster.leader} rejected the write: {cluster.body(response)!r}"
    )

    mismatches = cluster.wait_for_body_on_all(unique_key, unique_value)
    assert not mismatches, (
        f"key {unique_key!r} written on the leader ({cluster.leader}) did not "
        f"replicate to every node. Last body served per failing node: {mismatches!r}. "
        f"Nodes checked: {list(cluster.nodes)}"
    )


# --------------------------------------------------------------------------
# R0.2 -- DELETE round-trip
# --------------------------------------------------------------------------


def test_r0_2_delete_round_trip_removes_key_on_all_nodes(
    cluster, unique_key, unique_value
):
    """SET, confirm everywhere, DELETE on the leader, gone everywhere."""
    set_response = cluster.set(cluster.leader, unique_key, unique_value)
    assert set_response.status_code == 200
    assert cluster.body(set_response) == BODY_OK

    before = cluster.wait_for_body_on_all(unique_key, unique_value)
    assert not before, (
        f"precondition failed: {unique_key!r} did not replicate before the DELETE. "
        f"Last body served per failing node: {before!r}"
    )

    delete_response = cluster.delete(cluster.leader, unique_key)
    assert delete_response.status_code == 200
    assert cluster.body(delete_response) == BODY_OK, (
        f"leader {cluster.leader} rejected the DELETE: "
        f"{cluster.body(delete_response)!r}"
    )

    # A deleted key is indistinguishable from a never-written one: the handler
    # returns the same 200 / "Key Not Found" body for both.
    after = cluster.wait_for_body_on_all(unique_key, BODY_KEY_NOT_FOUND)
    assert not after, (
        f"key {unique_key!r} was still readable after DELETE on the leader "
        f"({cluster.leader}). Last body served per failing node: {after!r}"
    )


# --------------------------------------------------------------------------
# R0.3 -- missing key
# --------------------------------------------------------------------------


def test_r0_3_get_missing_key_returns_key_not_found(cluster, unique_key):
    """GET of a never-written key: HTTP 200 with the body ``Key Not Found``.

    PINNED BUGGY BEHAVIOR: a miss is not an error status. ``KVHttpHandler::
    handle_get`` returns ``HttpResponse::ok("Key Not Found")``
    (cpp-app/src/network/http_server.hpp), so the status code carries no
    information at all.

    Phase 1 turns this into a real HTTP 404 with a structured body; this test
    is rewritten there. Until then, 200 + "Key Not Found" is the contract.
    """
    for base_url in cluster.nodes:
        response = cluster.get(base_url, unique_key)

        assert response.status_code == 200, (
            f"{base_url} answered HTTP {response.status_code} for a missing key; "
            "Phase 0 pins the current 200-for-everything contract"
        )
        assert cluster.body(response) == BODY_KEY_NOT_FOUND, (
            f"{base_url} served {cluster.body(response)!r} for the "
            f"never-written key {unique_key!r}"
        )


# --------------------------------------------------------------------------
# R0.4 -- writes to a follower are not forwarded
# --------------------------------------------------------------------------


def test_r0_4_set_on_follower_is_rejected(cluster, unique_key, unique_value):
    """SET sent to a follower: HTTP 200 with the body ``error``.

    PINNED BUGGY BEHAVIOR, two of them:

    1. No leader forwarding. ``raft.Apply`` on a follower returns
       ``ErrNotLeader``; ``rpc.Server.Propose`` reports that as
       ``ProposeResponse{Success: false}`` (go-sidecar/internal/rpc/server.go)
       and the client just sees a failure.
    2. The failure is stringly reported: ``handle_insert`` returns
       ``HttpResponse::ok(success ? "ok" : "error")``
       (cpp-app/src/network/http_server.hpp) -- HTTP 200 with the body
       ``error``, and no way to tell "not the leader" from "sidecar down".

    Phase 1 replaces the stringly error with a real status code; Phase 4
    replaces the rejection itself with leader forwarding. This test is
    rewritten then.
    """
    if not cluster.followers:
        pytest.skip(
            "single-node cluster (leader is the only node in "
            f"{list(cluster.nodes)}) -- no follower to reject a write"
        )

    for base_url in cluster.followers:
        response = cluster.set(base_url, unique_key, unique_value)

        assert response.status_code == 200, (
            f"follower {base_url} answered HTTP {response.status_code}; "
            "Phase 0 pins the current 200-for-everything contract"
        )
        assert cluster.body(response) == BODY_ERROR, (
            f"follower {base_url} answered {cluster.body(response)!r} for a write; "
            "expected the no-forwarding rejection body 'error'. If this says 'ok', "
            f"leadership moved off {cluster.leader} mid-run"
        )

    # The rejected write must not have reached any node's store, including the
    # follower that refused it.
    for base_url in cluster.nodes:
        assert cluster.body(cluster.get(base_url, unique_key)) == BODY_KEY_NOT_FOUND, (
            f"{base_url} has a value for {unique_key!r}, but the only write for "
            "that key was rejected by a follower"
        )
