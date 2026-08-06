"""Acceptance for client authentication and Redis-style user ACLs.

    export RAFTKV_ADMIN_PASSWORD="$(openssl rand -hex 16)"
    docker compose -f docker-compose.yml -f docker-compose.auth.yml up -d
    pytest tests/e2e -m requires_auth -v -rs

Marked ``requires_auth`` and deselected by default, and unlike the other two
markers this one is not merely a convenience: with client auth enabled EVERY
unauthenticated test in the default suite would fail, so the auth-on and auth-off
profiles cannot share a cluster. This module therefore needs its own bring-up,
exactly as ``test_secure_profile.py`` does. Every test SKIPS rather than fails
when that cluster is not up, so an accidental run reports "not verified here"
instead of a wall of red.

What is here that a unit test cannot reach:

* **Replication of the user table.** The load-bearing test is
  :func:`test_a_created_user_authenticates_on_every_node`: a user created through
  the leader must become usable on all three nodes. Creating a user is a raft
  entry like any other, and nothing but a real cluster proves the record reaches
  the followers, survives their apply path and is readable by their
  authenticator.
* **That a refusal happens BEFORE the side effect.** A 403 that arrived after the
  write was already proposed would pass a status-code-only test and leave the
  cluster modified. The same argument as ``test_security.py``'s ``last_log_index``
  assertion on a refused ``/join``.
* **That the reserved key space really is unreachable end to end**, including the
  read direction, which is what keeps password hashes off the wire.
"""

from __future__ import annotations

import os
import time
import uuid

import msgpack
import pytest
import requests

from contracts import (
    BOOTSTRAP_ADMIN,
    CLASS_ADMIN,
    CLASS_READ,
    CLASS_WRITE,
    ERROR_INVALID_CREDENTIALS,
    ERROR_PERMISSION_DENIED,
    HTTP_BAD_REQUEST,
    HTTP_FORBIDDEN,
    HTTP_NOT_FOUND,
    HTTP_OK,
    HTTP_UNAUTHORIZED,
    JSON_CONTENT_TYPE,
    MSGPACK_CONTENT_TYPE,
    REASON_PHRASES,
    USER_KEY_PREFIX,
    WWW_AUTHENTICATE_BASIC,
    WWW_AUTHENTICATE_HEADER,
)

pytestmark = pytest.mark.requires_auth

TIMEOUT = 10.0

#: Deadline for a committed user record to become usable on every node. Generous
#: on purpose: this is replication plus an apply, and a flaky deadline here would
#: look like a security bug.
CONVERGE_TIMEOUT = 15.0
POLL_INTERVAL = 0.2


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------


@pytest.fixture(scope="module")
def admin_password() -> str:
    """The bootstrap admin password, which must match the running cluster's."""
    password = os.environ.get("RAFTKV_ADMIN_PASSWORD", "").strip()
    if not password:
        # docker-compose.auth.yml interpolates this same fallback, so an
        # unexported variable still lines up with a default `up -d`.
        password = "dev-only-insecure-password"
    return password


@pytest.fixture(scope="module")
def auth_cluster(nodes: list[str], admin_password: str) -> list[str]:
    """Skip unless every node is up AND actually enforcing authentication."""
    for base_url in nodes:
        try:
            anonymous = requests.get(f"{base_url}/kv/probe", timeout=TIMEOUT)
        except requests.RequestException as exc:
            pytest.skip(f"{base_url} is not reachable: {exc}")
        if anonymous.status_code != HTTP_UNAUTHORIZED:
            pytest.skip(
                f"{base_url} is not enforcing client auth (an unauthenticated "
                f"GET returned {anonymous.status_code}, expected "
                f"{HTTP_UNAUTHORIZED}). Bring the cluster up with "
                "docker-compose.auth.yml."
            )
        # ...and that the password this module holds is the cluster's, or every
        # assertion below would fail for one uninteresting reason.
        whoami = requests.get(
            f"{base_url}/auth/whoami",
            auth=(BOOTSTRAP_ADMIN, admin_password),
            timeout=TIMEOUT,
        )
        if whoami.status_code != HTTP_OK:
            pytest.skip(
                f"RAFTKV_ADMIN_PASSWORD does not match the cluster at "
                f"{base_url} (/auth/whoami returned {whoami.status_code}); "
                "export the same value the cluster was started with."
            )
    return list(nodes)


@pytest.fixture
def admin(admin_password: str) -> tuple[str, str]:
    return (BOOTSTRAP_ADMIN, admin_password)


@pytest.fixture
def user_factory(auth_cluster: list[str], admin: tuple[str, str]):
    """Create users on the leader and delete them afterwards.

    Names are unique per run so a re-run cannot collide with a record a previous
    run left behind, and cleanup runs even when a test fails.
    """
    created: list[str] = []
    leader_url = _find_write_node(auth_cluster, admin)

    def create(
        classes: list[str],
        patterns: list[str],
        password: str = "generated-password",
        enabled: bool = True,
        name: str | None = None,
    ) -> tuple[str, str]:
        user = name or f"t{uuid.uuid4().hex[:12]}"
        response = _put_user(
            leader_url, admin, user, password, classes, patterns, enabled
        )
        assert response.status_code == HTTP_OK, (
            f"creating {user} failed: {response.status_code} {response.text}"
        )
        created.append(user)
        return user, password

    yield create

    for user in created:
        try:
            requests.delete(
                f"{leader_url}/auth/users/{user}", auth=admin, timeout=TIMEOUT
            )
        except requests.RequestException:
            pass


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------


def _put_user(
    base_url: str,
    admin: tuple[str, str],
    name: str,
    password: str,
    classes: list[str],
    patterns: list[str],
    enabled: bool = True,
) -> requests.Response:
    body = msgpack.packb(
        {
            "password": password,
            "enabled": enabled,
            "classes": classes,
            "patterns": patterns,
        }
    )
    return requests.put(
        f"{base_url}/auth/users/{name}",
        data=body,
        headers={"Content-Type": MSGPACK_CONTENT_TYPE},
        auth=admin,
        timeout=TIMEOUT,
    )


def _find_write_node(nodes: list[str], admin: tuple[str, str]) -> str:
    """A node that accepts a write.

    Phase 4 forwards writes, so in principle any node will do; picking one that
    demonstrably answers 200 avoids blaming auth for an election in progress.
    """
    deadline = time.monotonic() + CONVERGE_TIMEOUT
    last = ""
    while time.monotonic() < deadline:
        for base_url in nodes:
            try:
                probe = requests.put(
                    f"{base_url}/kv/__probe_write",
                    data=b"x",
                    auth=admin,
                    timeout=TIMEOUT,
                )
            except requests.RequestException as exc:
                last = f"{base_url}: {exc}"
                continue
            if probe.status_code == HTTP_OK:
                return base_url
            last = f"{base_url}: {probe.status_code} {probe.text[:120]}"
        time.sleep(POLL_INTERVAL)
    pytest.skip(f"no node accepted a write within {CONVERGE_TIMEOUT}s: {last}")


def _wait_until_all(nodes: list[str], probe, expected) -> dict[str, object]:
    """Poll ``probe(node)`` on every node until it equals ``expected``.

    Returns the nodes that never converged, mapped to their last observation --
    empty means all of them did. Polling rather than sleeping is the point: a
    follower is expected to lag briefly, and a fixed sleep either flakes or
    hides the lag entirely.
    """
    stragglers: dict[str, object] = {}
    for base_url in nodes:
        deadline = time.monotonic() + CONVERGE_TIMEOUT
        observed: object = "never probed"
        while time.monotonic() < deadline:
            observed = probe(base_url)
            if observed == expected:
                break
            time.sleep(POLL_INTERVAL)
        else:
            stragglers[base_url] = observed
            continue
        if observed != expected:
            stragglers[base_url] = observed
    return stragglers


def _status_for(base_url: str, credentials: tuple[str, str]) -> int:
    try:
        return requests.get(
            f"{base_url}/auth/whoami", auth=credentials, timeout=TIMEOUT
        ).status_code
    except requests.RequestException:
        return -1


# --------------------------------------------------------------------------
# Authentication
# --------------------------------------------------------------------------


def test_unauthenticated_requests_are_refused_on_every_node(auth_cluster):
    """Every node, every data route: 401 with a usable challenge."""
    for base_url in auth_cluster:
        for method, path in [
            ("GET", "/kv/anything"),
            ("PUT", "/kv/anything"),
            ("DELETE", "/kv/anything"),
            ("GET", "/get-val?key=anything"),
            ("POST", "/insert-val"),
            ("GET", "/auth/whoami"),
            ("GET", "/auth/users/anyone"),
        ]:
            response = requests.request(
                method, f"{base_url}{path}", timeout=TIMEOUT
            )
            assert response.status_code == HTTP_UNAUTHORIZED, (
                f"{method} {base_url}{path} answered {response.status_code}"
            )
            # RFC 9110: a 401 without this tells a client to authenticate and
            # not how.
            assert (
                response.headers.get(WWW_AUTHENTICATE_HEADER)
                == WWW_AUTHENTICATE_BASIC
            ), f"{method} {path} is missing the challenge"
            assert response.reason == REASON_PHRASES[HTTP_UNAUTHORIZED]


def test_metrics_stays_reachable_without_a_credential(auth_cluster):
    """Not an oversight: the secure profile's proxy health-checks this path.

    Requiring a credential here drops every node out of Caddy's rotation. It
    exposes counters and latencies, never keys or values.
    """
    for base_url in auth_cluster:
        response = requests.get(f"{base_url}/metrics", timeout=TIMEOUT)
        assert response.status_code == HTTP_OK
        assert "raftkv_http_requests_total" in response.text


def test_a_rejected_credential_is_403_without_a_challenge(auth_cluster, admin):
    """403, not 401: the credential was presented and refused, so no retry."""
    for base_url in auth_cluster:
        response = requests.get(
            f"{base_url}/kv/anything",
            auth=(BOOTSTRAP_ADMIN, admin[1] + "-wrong"),
            timeout=TIMEOUT,
        )
        assert response.status_code == HTTP_FORBIDDEN
        assert response.reason == REASON_PHRASES[HTTP_FORBIDDEN]
        assert WWW_AUTHENTICATE_HEADER not in response.headers
        assert response.json()["error"] == ERROR_INVALID_CREDENTIALS


def test_unknown_and_wrong_password_are_indistinguishable(auth_cluster, admin):
    """One answer for both, so the endpoint cannot enumerate accounts."""
    base_url = auth_cluster[0]

    unknown = requests.get(
        f"{base_url}/kv/anything",
        auth=("nobody-with-this-name", "any-password"),
        timeout=TIMEOUT,
    )
    wrong = requests.get(
        f"{base_url}/kv/anything",
        auth=(BOOTSTRAP_ADMIN, "definitely-not-the-password"),
        timeout=TIMEOUT,
    )

    assert unknown.status_code == wrong.status_code == HTTP_FORBIDDEN
    assert unknown.text == wrong.text, (
        "an unknown user and a wrong password must not be told apart"
    )


def test_a_malformed_authorization_header_is_401(auth_cluster):
    base_url = auth_cluster[0]
    for header in [
        "Bearer some-token",  # right idea, wrong scheme for this surface
        "Basic",
        "Basic ",
        "Basic !!!not-base64!!!",
        "Digest username=x",
    ]:
        response = requests.get(
            f"{base_url}/kv/anything",
            headers={"Authorization": header},
            timeout=TIMEOUT,
        )
        assert response.status_code == HTTP_UNAUTHORIZED, (
            f"header {header!r} answered {response.status_code}"
        )


def test_the_bootstrap_admin_can_read_and_write(auth_cluster, admin, unique_key):
    base_url = _find_write_node(auth_cluster, admin)

    write = requests.put(
        f"{base_url}/kv/{unique_key}", data=b"admin-wrote-this", auth=admin,
        timeout=TIMEOUT,
    )
    assert write.status_code == HTTP_OK

    read = requests.get(f"{base_url}/kv/{unique_key}", auth=admin, timeout=TIMEOUT)
    assert read.status_code == HTTP_OK
    assert read.content == b"admin-wrote-this"


def test_whoami_reports_the_callers_own_acl(auth_cluster, admin):
    response = requests.get(
        f"{auth_cluster[0]}/auth/whoami", auth=admin, timeout=TIMEOUT
    )

    assert response.status_code == HTTP_OK
    assert response.headers["Content-Type"] == JSON_CONTENT_TYPE
    body = response.json()
    assert body["name"] == BOOTSTRAP_ADMIN
    assert set(body["classes"]) == {CLASS_READ, CLASS_WRITE, CLASS_ADMIN}
    assert body["patterns"] == ["*"]


# --------------------------------------------------------------------------
# User management, and its replication
# --------------------------------------------------------------------------


def test_a_created_user_authenticates_on_every_node(
    auth_cluster, admin, user_factory
):
    """THE load-bearing test of this module.

    A user is a raft entry: created on the leader, replicated, applied on each
    node, then read back by that node's authenticator. Only a real cluster shows
    that the whole chain works — a unit test can prove the record is well-formed
    but not that node3 can log the user in.

    The negative half matters just as much: before creation the same credential
    is refused everywhere, so a pass cannot come from the cluster being open.
    """
    name = f"t{uuid.uuid4().hex[:12]}"
    password = "replication-test-password"

    before = _wait_until_all(
        auth_cluster, lambda url: _status_for(url, (name, password)), HTTP_FORBIDDEN
    )
    assert not before, f"a nonexistent user was accepted somewhere: {before}"

    created, _ = user_factory(
        classes=[CLASS_READ], patterns=["*"], password=password, name=name
    )
    assert created == name

    stragglers = _wait_until_all(
        auth_cluster, lambda url: _status_for(url, (name, password)), HTTP_OK
    )
    assert not stragglers, (
        f"the user never became usable on: {stragglers} "
        f"(within {CONVERGE_TIMEOUT}s)"
    )


def test_a_deleted_user_stops_authenticating_on_every_node(
    auth_cluster, admin, user_factory
):
    name, password = user_factory(
        classes=[CLASS_READ], patterns=["*"], password="revocation-test-password"
    )
    assert not _wait_until_all(
        auth_cluster, lambda url: _status_for(url, (name, password)), HTTP_OK
    )

    leader_url = _find_write_node(auth_cluster, admin)
    removed = requests.delete(
        f"{leader_url}/auth/users/{name}", auth=admin, timeout=TIMEOUT
    )
    assert removed.status_code == HTTP_OK

    # POLLED, not asserted immediately: a follower is expected to lag by the
    # replication delay, which is a documented property rather than a bug.
    stragglers = _wait_until_all(
        auth_cluster, lambda url: _status_for(url, (name, password)), HTTP_FORBIDDEN
    )
    assert not stragglers, f"a deleted user still authenticates on: {stragglers}"


def test_deleting_a_user_twice_succeeds(auth_cluster, admin, user_factory):
    """Writes are at-least-once, so a retried delete must not report failure."""
    name, _ = user_factory(classes=[CLASS_READ], patterns=["*"])
    leader_url = _find_write_node(auth_cluster, admin)

    first = requests.delete(
        f"{leader_url}/auth/users/{name}", auth=admin, timeout=TIMEOUT
    )
    second = requests.delete(
        f"{leader_url}/auth/users/{name}", auth=admin, timeout=TIMEOUT
    )

    assert first.status_code == HTTP_OK
    assert second.status_code == HTTP_OK


def test_reading_a_user_never_returns_its_secret(auth_cluster, admin, user_factory):
    """An admin has no use for the hash, and an endpoint that returns it turns
    one compromised admin credential into an offline attack on every password."""
    name, password = user_factory(
        classes=[CLASS_READ, CLASS_WRITE],
        patterns=["app:*"],
        password="secret-to-not-leak",
    )
    leader_url = _find_write_node(auth_cluster, admin)

    response = requests.get(
        f"{leader_url}/auth/users/{name}", auth=admin, timeout=TIMEOUT
    )

    assert response.status_code == HTTP_OK
    body = response.json()
    assert body["name"] == name
    assert body["enabled"] is True
    assert set(body["classes"]) == {CLASS_READ, CLASS_WRITE}
    assert body["patterns"] == ["app:*"]
    assert set(body) == {"name", "enabled", "classes", "patterns"}, (
        f"unexpected members in a user response: {sorted(body)}"
    )
    lowered = response.text.lower()
    assert "salt" not in lowered
    assert "hash" not in lowered
    assert password not in response.text


def test_reading_an_absent_user_is_404(auth_cluster, admin):
    response = requests.get(
        f"{auth_cluster[0]}/auth/users/definitely-not-a-user",
        auth=admin,
        timeout=TIMEOUT,
    )
    assert response.status_code == HTTP_NOT_FOUND


def test_the_bootstrap_admin_cannot_be_managed_through_the_api(auth_cluster, admin):
    """It is configuration, checked before the store, so a record under its name
    would be dead weight that looks like a live account."""
    leader_url = _find_write_node(auth_cluster, admin)

    created = _put_user(
        leader_url, admin, BOOTSTRAP_ADMIN, "attacker-chosen-password",
        [CLASS_ADMIN], ["*"],
    )
    assert created.status_code == HTTP_BAD_REQUEST

    removed = requests.delete(
        f"{leader_url}/auth/users/{BOOTSTRAP_ADMIN}", auth=admin, timeout=TIMEOUT
    )
    assert removed.status_code == HTTP_BAD_REQUEST

    # And the real admin still works, i.e. nothing was half-applied.
    assert _status_for(leader_url, admin) == HTTP_OK


def test_a_weak_or_malformed_upsert_is_refused(auth_cluster, admin):
    leader_url = _find_write_node(auth_cluster, admin)
    name = f"t{uuid.uuid4().hex[:12]}"

    # Too short to survive an offline attack on the stored digest.
    assert (
        _put_user(leader_url, admin, name, "short", [CLASS_READ], ["*"]).status_code
        == HTTP_BAD_REQUEST
    )
    # An unknown class is a typo that would otherwise silently grant nothing.
    assert (
        _put_user(
            leader_url, admin, name, "long-enough-password", ["superuser"], ["*"]
        ).status_code
        == HTTP_BAD_REQUEST
    )
    # Not msgpack at all.
    assert (
        requests.put(
            f"{leader_url}/auth/users/{name}",
            data=b"{}",
            headers={"Content-Type": "application/json"},
            auth=admin,
            timeout=TIMEOUT,
        ).status_code
        == 415
    )
    # A name this server would never store.
    assert (
        _put_user(
            leader_url, admin, "has%20space", "long-enough-password",
            [CLASS_READ], ["*"],
        ).status_code
        == HTTP_BAD_REQUEST
    )

    # None of them created anything.
    assert (
        requests.get(
            f"{leader_url}/auth/users/{name}", auth=admin, timeout=TIMEOUT
        ).status_code
        == HTTP_NOT_FOUND
    )


# --------------------------------------------------------------------------
# Authorization: classes and key patterns
# --------------------------------------------------------------------------


def test_a_read_only_user_cannot_write(auth_cluster, admin, user_factory, unique_key):
    name, password = user_factory(
        classes=[CLASS_READ], patterns=["*"], password="read-only-password"
    )
    leader_url = _find_write_node(auth_cluster, admin)
    requests.put(
        f"{leader_url}/kv/{unique_key}", data=b"seeded", auth=admin, timeout=TIMEOUT
    )
    _wait_until_all(auth_cluster, lambda url: _status_for(url, (name, password)),
                    HTTP_OK)

    reader = (name, password)
    assert (
        requests.get(
            f"{leader_url}/kv/{unique_key}", auth=reader, timeout=TIMEOUT
        ).status_code
        == HTTP_OK
    )

    denied = requests.put(
        f"{leader_url}/kv/{unique_key}", data=b"nope", auth=reader, timeout=TIMEOUT
    )
    assert denied.status_code == HTTP_FORBIDDEN
    assert denied.json()["error"] == ERROR_PERMISSION_DENIED
    assert (
        requests.delete(
            f"{leader_url}/kv/{unique_key}", auth=reader, timeout=TIMEOUT
        ).status_code
        == HTTP_FORBIDDEN
    )

    # The refused write really was refused, not merely reported as refused.
    still_there = requests.get(
        f"{leader_url}/kv/{unique_key}", auth=admin, timeout=TIMEOUT
    )
    assert still_there.content == b"seeded"


def test_key_patterns_are_enforced_on_every_data_route(
    auth_cluster, admin, user_factory, run_id
):
    scope = f"app{run_id[:8]}"
    name, password = user_factory(
        classes=[CLASS_READ, CLASS_WRITE],
        patterns=[f"{scope}:*"],
        password="scoped-user-password",
    )
    leader_url = _find_write_node(auth_cluster, admin)
    _wait_until_all(auth_cluster, lambda url: _status_for(url, (name, password)),
                    HTTP_OK)
    scoped = (name, password)

    in_scope = f"{scope}:allowed"
    out_of_scope = f"other{run_id[:8]}:refused"
    requests.put(
        f"{leader_url}/kv/{out_of_scope}", data=b"not-for-them", auth=admin,
        timeout=TIMEOUT,
    )

    assert (
        requests.put(
            f"{leader_url}/kv/{in_scope}", data=b"v", auth=scoped, timeout=TIMEOUT
        ).status_code
        == HTTP_OK
    )

    for method, url in [
        ("GET", f"{leader_url}/kv/{out_of_scope}"),
        ("PUT", f"{leader_url}/kv/{out_of_scope}"),
        ("DELETE", f"{leader_url}/kv/{out_of_scope}"),
        ("GET", f"{leader_url}/get-val?key={out_of_scope}"),
    ]:
        response = requests.request(
            method, url, data=b"v", auth=scoped, timeout=TIMEOUT
        )
        assert response.status_code == HTTP_FORBIDDEN, f"{method} {url}"
        assert b"not-for-them" not in response.content, (
            "the refusal body leaked the value it refused to serve"
        )

    # The legacy write route is checked against the key INSIDE the body, which
    # is the only way to apply a key ACL to a route that forwards a raw command.
    smuggled = requests.post(
        f"{leader_url}/insert-val",
        data=msgpack.packb({"op": "SET", "key": out_of_scope, "value": "v"}),
        headers={"Content-Type": MSGPACK_CONTENT_TYPE},
        auth=scoped,
        timeout=TIMEOUT,
    )
    assert smuggled.status_code == HTTP_FORBIDDEN
    assert (
        requests.get(
            f"{leader_url}/kv/{out_of_scope}", auth=admin, timeout=TIMEOUT
        ).content
        == b"not-for-them"
    ), "an out-of-scope write reached the store"


def test_a_non_admin_cannot_manage_users(auth_cluster, admin, user_factory):
    """The classes are independent: read+write is not a licence to create users."""
    name, password = user_factory(
        classes=[CLASS_READ, CLASS_WRITE], patterns=["*"],
        password="data-user-password",
    )
    leader_url = _find_write_node(auth_cluster, admin)
    _wait_until_all(auth_cluster, lambda url: _status_for(url, (name, password)),
                    HTTP_OK)
    data_user = (name, password)
    victim = f"t{uuid.uuid4().hex[:12]}"

    assert (
        _put_user(
            leader_url, data_user, victim, "escalation-password", [CLASS_ADMIN],
            ["*"],
        ).status_code
        == HTTP_FORBIDDEN
    )
    assert (
        requests.get(
            f"{leader_url}/auth/users/{name}", auth=data_user, timeout=TIMEOUT
        ).status_code
        == HTTP_FORBIDDEN
    )
    assert (
        requests.delete(
            f"{leader_url}/auth/users/{name}", auth=data_user, timeout=TIMEOUT
        ).status_code
        == HTTP_FORBIDDEN
    )

    # THE PART A STATUS CODE ALONE WOULD NOT CATCH: the escalation attempt must
    # not have created anything. A middleware that refused *after* proposing
    # would pass every assertion above and leave an admin account behind.
    assert (
        requests.get(
            f"{leader_url}/auth/users/{victim}", auth=admin, timeout=TIMEOUT
        ).status_code
        == HTTP_NOT_FOUND
    )


def test_a_disabled_user_cannot_authenticate(auth_cluster, admin, user_factory):
    name, password = user_factory(
        classes=[CLASS_READ, CLASS_WRITE], patterns=["*"],
        password="disabled-user-password", enabled=False,
    )

    stragglers = _wait_until_all(
        auth_cluster, lambda url: _status_for(url, (name, password)), HTTP_FORBIDDEN
    )
    assert not stragglers, f"a disabled user authenticated on: {stragglers}"

    # It still exists as a record — disabled is not deleted.
    leader_url = _find_write_node(auth_cluster, admin)
    stored = requests.get(
        f"{leader_url}/auth/users/{name}", auth=admin, timeout=TIMEOUT
    )
    assert stored.status_code == HTTP_OK
    assert stored.json()["enabled"] is False


# --------------------------------------------------------------------------
# The reserved key space
# --------------------------------------------------------------------------


def test_the_reserved_key_space_is_unreachable_through_the_data_api(
    auth_cluster, admin
):
    """Refused for everyone, admin included, in both directions.

    The read direction is the one that matters: a local read of a user record
    would hand out its salt and password hash to anyone holding the read class.
    """
    leader_url = _find_write_node(auth_cluster, admin)
    reserved = f"{USER_KEY_PREFIX}{BOOTSTRAP_ADMIN}"

    for method, url, in [
        ("GET", f"{leader_url}/kv/{reserved}"),
        ("PUT", f"{leader_url}/kv/{reserved}"),
        ("DELETE", f"{leader_url}/kv/{reserved}"),
        ("GET", f"{leader_url}/get-val?key={reserved}"),
        # Percent-encoded, because the check must run after decoding.
        ("PUT", f"{leader_url}/kv/__sys%3Auser%3A{BOOTSTRAP_ADMIN}"),
    ]:
        response = requests.request(
            method, url, data=b"forged", auth=admin, timeout=TIMEOUT
        )
        assert response.status_code == HTTP_FORBIDDEN, (
            f"{method} {url} answered {response.status_code}"
        )

    # The legacy route, whose body could otherwise carry the key straight past.
    forged = requests.post(
        f"{leader_url}/insert-val",
        data=msgpack.packb({"op": "SET", "key": reserved, "value": "forged"}),
        headers={"Content-Type": MSGPACK_CONTENT_TYPE},
        auth=admin,
        timeout=TIMEOUT,
    )
    assert forged.status_code == HTTP_FORBIDDEN

    # And the admin still authenticates, i.e. nothing overwrote its identity.
    assert _status_for(leader_url, admin) == HTTP_OK


def test_user_management_operations_are_refused_on_the_legacy_write_route(
    auth_cluster, admin
):
    """Otherwise /insert-val is a way to mint an administrator with the write
    class alone, bypassing the admin check on /auth/users/{name}."""
    leader_url = _find_write_node(auth_cluster, admin)

    for op in ["USER_SET", "USER_DEL"]:
        response = requests.post(
            f"{leader_url}/insert-val",
            data=msgpack.packb({"op": op, "key": "smuggled", "value": "x"}),
            headers={"Content-Type": MSGPACK_CONTENT_TYPE},
            auth=admin,
            timeout=TIMEOUT,
        )
        assert response.status_code == HTTP_BAD_REQUEST, f"op={op}"
        assert "user-management" in response.json()["error"]

    assert (
        requests.get(
            f"{leader_url}/auth/users/smuggled", auth=admin, timeout=TIMEOUT
        ).status_code
        == HTTP_NOT_FOUND
    )
