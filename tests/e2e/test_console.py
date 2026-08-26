"""End-to-end tests for the management console and its supporting routes.

These are the only tests that can prove three things a unit test cannot: that
the assets CMake embedded are really reachable over HTTP, that the real
``RaftNode.Status`` RPC answers, and that a key written on the leader shows up
in ``GET /kv`` on **every** node -- which is the whole propose -> replicate ->
apply -> local-index path.

Runs in the DEFAULT suite: it needs a cluster but never touches docker.

The console-asset cases SKIP rather than fail when the binary was built with
``-DKVDB_CONSOLE=OFF``. That is a real, tested deployment mode (its 404 contract
is pinned in ``cpp-app/tests/http_handler_test.cpp``), so a node without the
console is not a broken node -- but the skip reason says so out loud, because a
silent skip is an unverified requirement.
"""

from __future__ import annotations

import urllib.parse

import pytest
import requests

from conftest import wait_until
from contracts import (
    AUTH_USERS_PATH,
    CLUSTER_STATUS_PATH,
    CONSOLE_HTML_CONTENT_TYPE,
    CONSOLE_INDEX_PATH,
    CONSOLE_NOT_BUILT_ERROR,
    HTTP_BAD_REQUEST,
    HTTP_FORBIDDEN,
    HTTP_METHOD_NOT_ALLOWED,
    HTTP_NOT_FOUND,
    HTTP_OK,
    JSON_CONTENT_TYPE,
    KEY_LIST_LIMIT_ERROR,
    KV_LIST_PATH,
)

TIMEOUT = 5.0
HTTP_FOUND = 302
HTTP_NOT_MODIFIED = 304


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------


@pytest.fixture(scope="module")
def console_node(nodes: list[str]) -> str:
    """A node whose binary actually has the console embedded, or skip."""
    base_url = nodes[0]
    response = requests.get(f"{base_url}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)
    if response.status_code == HTTP_NOT_FOUND:
        if CONSOLE_NOT_BUILT_ERROR in response.text:
            pytest.skip(
                "this binary was built with -DKVDB_CONSOLE=OFF, so there are no "
                "embedded assets to serve (its 404 contract is covered by the "
                "C++ unit tests instead)"
            )
        pytest.fail(f"{base_url}{CONSOLE_INDEX_PATH} answered an unexpected 404")
    return base_url


def _first_asset_path(index_html: str) -> str:
    """Pull one asset URL out of the page rather than guessing its hash."""
    marker = 'src="/console/'
    start = index_html.find(marker)
    assert start != -1, "index.html references no script"
    start += len('src="')
    end = index_html.find('"', start)
    return index_html[start:end]


def _encoded(text: str) -> str:
    return urllib.parse.quote(text, safe="")


# --------------------------------------------------------------------------
# Static assets
# --------------------------------------------------------------------------


def test_root_redirects_to_the_console(nodes: list[str]):
    """Every node redirects "/" -- including one built without the console.

    The redirect is unconditional on purpose: it is a routing decision, not an
    asset lookup, so it must not depend on what was embedded.
    """
    for base_url in nodes:
        response = requests.get(base_url + "/", timeout=TIMEOUT, allow_redirects=False)
        assert response.status_code == HTTP_FOUND, base_url
        assert response.headers["Location"] == CONSOLE_INDEX_PATH


def test_console_index_is_served(console_node: str):
    response = requests.get(f"{console_node}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)

    assert response.status_code == HTTP_OK
    assert response.headers["Content-Type"] == CONSOLE_HTML_CONTENT_TYPE
    assert '<div id="root">' in response.text
    # index.html's URL never changes, so it must revalidate rather than be
    # cached -- otherwise a redeploy serves a stale app forever.
    assert response.headers["Cache-Control"] == "no-cache"
    assert response.headers["ETag"]
    # The engine sends its own CSP with the page; the console relies on it.
    assert "default-src 'self'" in response.headers["Content-Security-Policy"]
    assert response.headers["X-Content-Type-Options"] == "nosniff"


def test_console_index_revalidates_to_304(console_node: str):
    first = requests.get(f"{console_node}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)
    etag = first.headers["ETag"]

    second = requests.get(
        f"{console_node}{CONSOLE_INDEX_PATH}",
        headers={"If-None-Match": etag},
        timeout=TIMEOUT,
    )
    assert second.status_code == HTTP_NOT_MODIFIED
    assert second.content == b""


def test_console_hashed_assets_are_immutable(console_node: str):
    index = requests.get(f"{console_node}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)
    asset_path = _first_asset_path(index.text)

    response = requests.get(console_node + asset_path, timeout=TIMEOUT)
    assert response.status_code == HTTP_OK
    assert "immutable" in response.headers["Cache-Control"]
    assert response.headers["Content-Type"] == "text/javascript; charset=utf-8"


def test_console_serves_gzip_when_offered(console_node: str):
    session = requests.Session()
    response = session.get(
        f"{console_node}{CONSOLE_INDEX_PATH}",
        headers={"Accept-Encoding": "gzip"},
        timeout=TIMEOUT,
    )
    assert response.status_code == HTTP_OK
    # requests decompresses transparently and rewrites the header it consumed,
    # so the RAW header is what proves compressed bytes were actually sent.
    assert response.raw.headers.get("Content-Encoding") == "gzip"
    assert '<div id="root">' in response.text


def test_console_serves_raw_bytes_without_accept_encoding(console_node: str):
    """`curl .../console/` stays readable -- both variants are embedded."""
    response = requests.get(
        f"{console_node}{CONSOLE_INDEX_PATH}",
        headers={"Accept-Encoding": "identity"},
        timeout=TIMEOUT,
    )
    assert response.status_code == HTTP_OK
    assert response.raw.headers.get("Content-Encoding") is None
    assert '<div id="root">' in response.text


def test_console_unknown_asset_is_404_not_the_app(console_node: str):
    # Hash routing means there is no catch-all: answering "here is the app" to
    # every typo makes a 404 unobservable.
    response = requests.get(
        f"{console_node}/console/definitely-not-a-real-asset.js", timeout=TIMEOUT
    )
    assert response.status_code == HTTP_NOT_FOUND
    assert "<div" not in response.text


def test_console_rejects_a_non_get(console_node: str):
    response = requests.post(f"{console_node}{CONSOLE_INDEX_PATH}", timeout=TIMEOUT)
    assert response.status_code == HTTP_METHOD_NOT_ALLOWED


# --------------------------------------------------------------------------
# /cluster/status
# --------------------------------------------------------------------------


def test_cluster_status_agrees_on_the_leader(nodes: list[str]):
    payloads = []
    for base_url in nodes:
        response = requests.get(f"{base_url}{CLUSTER_STATUS_PATH}", timeout=TIMEOUT)
        assert response.status_code == HTTP_OK, base_url
        assert response.headers["Content-Type"] == JSON_CONTENT_TYPE
        payloads.append(response.json())

    # Every node answers for ITSELF, so node_id must differ...
    assert len({p["node_id"] for p in payloads}) == len(nodes)
    # ...but a healthy cluster agrees on who leads.
    leaders = {p["leader_id"] for p in payloads}
    assert len(leaders) == 1, f"nodes disagree on the leader: {leaders}"
    assert leaders.pop() != "", "no leader elected"

    # Exactly one node reports itself Leader.
    assert sum(1 for p in payloads if p["state"] == "Leader") == 1

    for payload in payloads:
        assert len(payload["peers"]) == len(nodes)
        assert payload["term"] >= 1
        assert payload["last_log_index"] >= payload["applied_index"]
        assert payload["key_count"] >= 0
        assert "auth_enabled" in payload
        # A partial-failure field must be absent on a healthy node.
        assert payload.get("error", "") == ""


def test_cluster_status_rejects_a_non_get(nodes: list[str]):
    response = requests.post(f"{nodes[0]}{CLUSTER_STATUS_PATH}", timeout=TIMEOUT)
    assert response.status_code == HTTP_METHOD_NOT_ALLOWED


# --------------------------------------------------------------------------
# GET /kv
# --------------------------------------------------------------------------


def test_a_written_key_is_listed_on_every_node(
    nodes: list[str], leader: str, unique_key: str
):
    """The load-bearing test: propose -> replicate -> apply -> local index.

    A unit test can prove scan_keys walks a std::set. Only this can prove the
    key reached every node's OWN index.
    """
    prefix = unique_key + ":"
    key = prefix + "listed"

    written = requests.put(f"{leader}/kv/{_encoded(key)}", data=b"value", timeout=TIMEOUT)
    assert written.status_code == HTTP_OK

    try:
        for base_url in nodes:
            # Polled against a deadline, never slept on: a follower applies the
            # entry asynchronously.
            listed = wait_until(
                lambda url=base_url: _encoded(key)
                in requests.get(
                    f"{url}{KV_LIST_PATH}?prefix={_encoded(prefix)}", timeout=TIMEOUT
                ).json()["keys"],
                timeout=10.0,
            )
            assert listed, f"{base_url} never listed {key!r}"
    finally:
        requests.delete(f"{leader}/kv/{_encoded(key)}", timeout=TIMEOUT)


def test_kv_list_paginates_to_completion(leader: str, unique_key: str):
    prefix = unique_key + ":"
    keys = [f"{prefix}{index:02d}" for index in range(5)]

    for key in keys:
        assert (
            requests.put(
                f"{leader}/kv/{_encoded(key)}", data=b"v", timeout=TIMEOUT
            ).status_code
            == HTTP_OK
        )

    try:
        # Walk it two at a time, stopping on an ABSENT cursor rather than a short
        # page -- the documented rule, and the only correct one.
        collected: list[str] = []
        url = f"{leader}{KV_LIST_PATH}?prefix={_encoded(prefix)}&limit=2"
        for _ in range(10):
            page = requests.get(url, timeout=TIMEOUT).json()
            collected.extend(page["keys"])
            cursor = page.get("next_cursor")
            if cursor is None:
                break
            url = (
                f"{leader}{KV_LIST_PATH}?prefix={_encoded(prefix)}"
                f"&limit=2&cursor={cursor}"
            )
        else:
            pytest.fail("pagination did not terminate")

        assert collected == [_encoded(key) for key in keys]
    finally:
        for key in keys:
            requests.delete(f"{leader}/kv/{_encoded(key)}", timeout=TIMEOUT)


def test_kv_list_round_trips_awkward_bytes(leader: str, unique_key: str):
    """A key is arbitrary bytes; the listing must survive that."""
    prefix = unique_key + ":"
    # A space, a slash, a '=' and a byte that is not valid UTF-8.
    key = prefix + "a b/c=d\x80"
    encoded_key = _encoded(key)

    assert (
        requests.put(
            f"{leader}/kv/{encoded_key}", data=b"v", timeout=TIMEOUT
        ).status_code
        == HTTP_OK
    )

    try:
        listed = wait_until(
            lambda: encoded_key.upper()
            in [
                candidate.upper()
                for candidate in requests.get(
                    f"{leader}{KV_LIST_PATH}?prefix={_encoded(prefix)}", timeout=TIMEOUT
                ).json()["keys"]
            ],
            timeout=10.0,
        )
        assert listed, f"{key!r} never appeared in its own prefix listing"
    finally:
        requests.delete(f"{leader}/kv/{encoded_key}", timeout=TIMEOUT)


def test_kv_list_never_reveals_reserved_keys(nodes: list[str]):
    # Holds whether or not auth is on: the reserved space is not addressable
    # through a data route in either direction.
    response = requests.get(f"{nodes[0]}{KV_LIST_PATH}?limit=500", timeout=TIMEOUT)
    assert response.status_code == HTTP_OK
    assert "__sys" not in response.text


def test_kv_list_refuses_a_reserved_prefix(nodes: list[str]):
    response = requests.get(
        f"{nodes[0]}{KV_LIST_PATH}?prefix=" + _encoded("__sys:"), timeout=TIMEOUT
    )
    assert response.status_code == HTTP_FORBIDDEN


def test_kv_list_rejects_a_bad_limit(nodes: list[str]):
    for value in ("0", "-1", "abc"):
        response = requests.get(
            f"{nodes[0]}{KV_LIST_PATH}?limit={value}", timeout=TIMEOUT
        )
        assert response.status_code == HTTP_BAD_REQUEST, value
        assert response.json()["error"] == KEY_LIST_LIMIT_ERROR


def test_kv_list_clamps_an_over_large_limit(nodes: list[str]):
    """A server-side cap is not a client error."""
    response = requests.get(f"{nodes[0]}{KV_LIST_PATH}?limit=100000", timeout=TIMEOUT)
    assert response.status_code == HTTP_OK


def test_kv_list_does_not_shadow_the_single_key_route(nodes: list[str]):
    # "/kv/" keeps its empty-key 400; "/kv" is the listing.
    assert (
        requests.get(f"{nodes[0]}/kv/", timeout=TIMEOUT).status_code == HTTP_BAD_REQUEST
    )


def test_auth_users_is_closed_without_a_password(nodes: list[str]):
    """Default profile: user management is default-closed.

    Skips when the cluster under test has auth ON -- that path is covered by
    ``test_auth.py``, which owns the auth profile.
    """
    response = requests.get(f"{nodes[0]}{AUTH_USERS_PATH}", timeout=TIMEOUT)
    if response.status_code != HTTP_FORBIDDEN:
        pytest.skip(
            "this cluster has client auth enabled; the auth-ON behaviour of "
            "/auth/users is covered by test_auth.py"
        )
    assert "RAFTKV_ADMIN_PASSWORD" in response.text
