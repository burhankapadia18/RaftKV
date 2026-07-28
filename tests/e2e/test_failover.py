"""Leader failover under continuous load (Phase 4 exit criterion).

The claim Phase 4 makes is that a client may talk to ONE node forever and never
implement retry-against-the-leader logic itself. Slices A and B make that true in
the steady state — a follower forwards writes and barriers reads. This test is
the interesting half: does it stay true while the leader is being killed?

The scenario, from ROADMAP.md Phase 4:

    continuous writes against a fixed follower while the leader is killed —
    after re-election, writes succeed again with no client-side redirect logic;
    no acknowledged write lost.

Two properties, and they are not the same:

  1. LIVENESS — writes start succeeding again once a new leader is elected,
     without the client changing where it sends them.
  2. SAFETY — every write that was ACKNOWLEDGED with 200 is still readable
     afterwards. A write that failed during the election may or may not have
     landed; that is allowed and is not what this checks. Acknowledged means
     committed, and committed must survive.

Property 2 is the one worth having. A test that only checked liveness would pass
on a system that silently dropped acknowledged writes during the handover.

Like test_crash.py this must kill a real container, so it carries the
``requires_docker`` marker and is deselected by default. See pytest.ini.
"""

from __future__ import annotations

import shutil
import subprocess
import threading
import time
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path

import pytest
import requests

from contracts import HTTP_OK

# Every test in this module kills a container.
pytestmark = pytest.mark.requires_docker

REPO_ROOT = Path(__file__).resolve().parents[2]

#: How long to keep writing after the kill before giving up on re-election.
#: A default-configured raft election is ~1-2s; 45s is generous enough that a
#: failure here means something is actually wrong rather than merely slow.
RECOVERY_DEADLINE = 45.0

#: Gap between writes in the load loop. Small enough to have several in flight
#: across the handover, large enough not to drown a laptop.
WRITE_INTERVAL = 0.02


@dataclass
class WriteLog:
    """What the load generator observed, and what it was promised."""

    #: Keys whose PUT returned 200. These MUST all be readable at the end.
    acknowledged: list[str] = field(default_factory=list)
    #: Keys whose PUT failed or errored. Allowed to be present or absent.
    failed: list[str] = field(default_factory=list)
    #: Status codes seen, for the failure message.
    statuses: dict = field(default_factory=dict)
    stop: bool = False
    #: Set once a write succeeds AFTER the kill — i.e. the cluster recovered.
    recovered_at: float | None = None


def _docker(*args: str, timeout: float = 30.0) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["docker", *args], capture_output=True, text=True, timeout=timeout
    )


def _compose_file() -> Path:
    return REPO_ROOT / "docker-compose.yml"


@pytest.fixture(scope="session")
def docker_available() -> None:
    """Skip cleanly when this cannot possibly run."""
    if shutil.which("docker") is None:
        pytest.skip("no `docker` CLI on PATH; failover cannot kill the leader")
    version = _docker("compose", "version")
    if version.returncode != 0:
        pytest.skip("`docker compose` (the CLI plugin) is not available")
    if not _compose_file().is_file():
        pytest.skip(f"no compose file at {_compose_file()}")


def _service_for_url(url: str) -> str | None:
    """Map a published node URL back to its compose service name.

    Done by asking compose which host port each service publishes, rather than
    assuming node1 is :8080. The mapping lives in docker-compose.yml and a test
    that hardcodes it silently tests the wrong container the day it changes.
    """
    port = urllib.parse.urlparse(url).port
    if port is None:
        return None
    listed = _docker("compose", "-f", str(_compose_file()), "ps",
                     "--format", "{{.Service}} {{.Publishers}}")
    if listed.returncode != 0:
        return None
    for line in listed.stdout.splitlines():
        if not line.strip():
            continue
        service = line.split()[0]
        if f":{port}" in line or f" {port}" in line:
            return service
    return None


def _put(base_url: str, key: str, value: bytes, timeout: float):
    encoded = urllib.parse.quote(key, safe="")
    return requests.put(f"{base_url}/kv/{encoded}", data=value, timeout=timeout)


def _load_generator(base_url: str, log: WriteLog, run_id: str,
                    kill_time_holder: list) -> None:
    """Write continuously to ONE node until told to stop.

    Deliberately never changes target: the whole point is that the client does
    not need redirect logic. Failures are recorded, not retried against another
    node, because retrying would hide the very thing being measured.
    """
    index = 0
    while not log.stop:
        key = f"failover-{run_id}-{index}"
        index += 1
        try:
            response = _put(base_url, key, f"v{index}".encode(), timeout=10)
            code = response.status_code
        except requests.RequestException as exc:
            log.failed.append(key)
            log.statuses[type(exc).__name__] = log.statuses.get(
                type(exc).__name__, 0) + 1
            time.sleep(WRITE_INTERVAL)
            continue

        log.statuses[code] = log.statuses.get(code, 0) + 1
        if code == HTTP_OK:
            log.acknowledged.append(key)
            # First success after the kill is the recovery moment.
            if kill_time_holder and log.recovered_at is None:
                log.recovered_at = time.monotonic()
        else:
            log.failed.append(key)
        time.sleep(WRITE_INTERVAL)


def test_r4_leader_failover_under_load_loses_no_acknowledged_write(
    docker_available, cluster, run_id
):
    """Kill the leader mid-load; the client keeps writing to the same follower."""
    if not cluster.followers:
        pytest.skip("single-node cluster: no follower to keep writing to")

    target = cluster.followers[0]
    leader_service = _service_for_url(cluster.leader)
    if leader_service is None:
        pytest.skip(
            f"could not map the leader URL {cluster.leader} back to a compose "
            "service; is this a local compose cluster?"
        )

    log = WriteLog()
    kill_marker: list = []
    writer = threading.Thread(
        target=_load_generator,
        args=(target, log, run_id, kill_marker),
        daemon=True,
    )
    writer.start()

    # Let a real baseline accumulate before disturbing anything, so "writes were
    # working before the kill" is an observation rather than an assumption.
    time.sleep(1.0)
    baseline = len(log.acknowledged)
    assert baseline > 0, (
        f"no write to the follower {target} succeeded even before the leader was "
        f"killed; statuses={log.statuses}. Slice A forwarding is broken."
    )

    # SIGKILL, not stop: a graceful shutdown could hand leadership over politely
    # and would not exercise an election at all.
    killed = _docker("compose", "-f", str(_compose_file()),
                     "kill", "-s", "KILL", leader_service)
    assert killed.returncode == 0, f"failed to kill {leader_service}: {killed.stderr}"
    kill_marker.append(time.monotonic())

    # Wait for the cluster to elect a new leader and for the SAME follower to
    # start accepting writes again, with no client-side redirect.
    deadline = time.monotonic() + RECOVERY_DEADLINE
    while time.monotonic() < deadline and log.recovered_at is None:
        time.sleep(0.1)

    log.stop = True
    writer.join(timeout=15)

    try:
        assert log.recovered_at is not None, (
            f"after killing the leader ({leader_service}), no write to {target} "
            f"succeeded within {RECOVERY_DEADLINE:g}s.\n"
            f"Statuses seen: {log.statuses}\n"
            "LIVENESS FAILURE: the client never changed where it was writing, "
            "which is exactly the promise Phase 4 makes — a follower should "
            "forward to whichever node won the election."
        )

        post_kill = len(log.acknowledged) - baseline
        assert post_kill > 0, "recovered_at was set but no post-kill write recorded"

        # THE SAFETY PROPERTY. Every 200 must still be readable. Reads go to a
        # surviving node with linearizable consistency so this cannot pass on a
        # stale local replica that merely happens to have the data.
        survivors = [u for u in cluster.nodes if u != cluster.leader]
        assert survivors, "no surviving node to read from"
        reader = survivors[0]

        missing = []
        for key in log.acknowledged:
            encoded = urllib.parse.quote(key, safe="")
            try:
                response = requests.get(
                    f"{reader}/kv/{encoded}",
                    params={"consistency": "linearizable"},
                    timeout=15,
                )
            except requests.RequestException as exc:
                missing.append(f"{key} (unreadable: {type(exc).__name__})")
                continue
            if response.status_code != HTTP_OK:
                missing.append(f"{key} ({response.status_code})")

        assert not missing, (
            f"{len(missing)} of {len(log.acknowledged)} ACKNOWLEDGED writes are "
            f"gone after the failover.\nFirst few: {missing[:10]}\n\n"
            "SAFETY FAILURE — this is the serious one. Writes that failed during "
            "the election are allowed to be absent; these were answered 200, "
            "which means committed, and committed must survive a leader change."
        )
    finally:
        # Always bring the node back, or every later test in the session runs
        # against a degraded cluster and fails for the wrong reason.
        _docker("compose", "-f", str(_compose_file()), "start", leader_service)
        _wait_for_cluster_health(cluster, run_id)


def _wait_for_cluster_health(cluster, run_id: str) -> None:
    """Block until some node will accept a write again.

    A fixed sleep here was wrong and produced a real failure: this test leaves the
    cluster mid-election, and the next docker-gated test started before a new
    leader existed, so its very first write came back
    503 {"error":"not leader","leader":""}. Polling for the condition is the same
    rule the rest of the suite follows — the only sleeps allowed are inside a
    bounded wait for something observable.

    Not an assertion: this is teardown. If the cluster never recovers, the NEXT
    test should be the one that says so, with its own message, rather than this
    one failing twice over.
    """
    deadline = time.monotonic() + RECOVERY_DEADLINE
    probe = f"failover-health-{run_id}"
    while time.monotonic() < deadline:
        for node in cluster.nodes:
            try:
                if _put(node, probe, b"1", timeout=5).status_code == HTTP_OK:
                    return
            except requests.RequestException:
                continue
        time.sleep(0.2)
