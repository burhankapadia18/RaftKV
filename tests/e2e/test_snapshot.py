"""Snapshots and log compaction: the log is bounded and a wiped node rejoins (Phase 3).

The second module in the suite that touches docker, and it carries the same
``requires_docker`` marker as ``test_crash.py`` for the same reason: it cannot
be expressed against an anonymous cluster. Read that module's docstring first --
the rule these two break, and why breaking it is quarantined behind a marker
rather than done silently, is explained there and not repeated here.

What these scenarios need that a normal e2e test does not
---------------------------------------------------------

1. **A cluster configured to snapshot inside a test run.** The shipped defaults
   are Raft's own (120s / 8192 entries / 10240 trailing logs). Nothing here can
   wait for that, so the cluster has to be started with
   ``docker-compose.test.yml`` layered on top::

       docker compose -f docker-compose.yml -f docker-compose.test.yml up -d
       pytest tests/e2e -m requires_docker -v -rs

   Every test reads the tunables back out of each sidecar's startup log line and
   **skips** when they are not aggressive. That is deliberate: running
   ``-m requires_docker`` against the ordinary cluster -- which is what CI's
   crash-test step does today -- then reports "not verified here" instead of
   hanging for two minutes and failing.

2. **The management API.** ``first_log_index`` and ``last_snapshot_index`` are
   the only machine-readable evidence that a snapshot happened and that the log
   was actually truncated, and they live on ``:6000/status``, which
   docker-compose.yml does not publish to the host.

3. **Container-side file operations.** The volumes are bind mounts written by
   root inside the container, so the wipe and the data-directory listings are
   done from inside a container rather than from the test host, where they would
   fail on permissions on a Linux CI runner.

All three live in ``snapshot_support.py``; this module is the scenarios.

Evidence, and what it does and does not prove
---------------------------------------------

The headline scenario stops a follower, deletes its entire volume, and starts it
again. The naive assertion -- "it serves every key" -- would pass just as well if
the leader had replayed its whole log into the blank node, which is what
happened before this phase and is precisely what the phase makes unnecessary.
Two independent facts rule that out, and both are asserted:

* **The leader's log no longer contains the beginning of history.** Its
  ``first_log_index`` is recorded immediately before the wipe and is > 1, so
  there are committed entries the leader simply cannot send. A node starting
  from index 0 therefore *has* to be caught up with an ``InstallSnapshot``.
* **The wiped node reports a snapshot it cannot have had locally.** Its
  ``last_snapshot_index`` is > 0 after the catch-up, and its data directory --
  including ``snapshots/`` -- was verified empty while it was down. The only
  place that snapshot can have come from is the leader.

Alongside those, the module asserts on a handful of log lines this repository
emits itself (``fsm: restored a …``, ``[StateMachine] Restored snapshot: …``;
they are named in ``snapshot_support.py``). That is a real coupling: renaming
one of those lines breaks this test. It is worth it, because the log lines are
what distinguish "raft installed a snapshot" from "raft handed the C++ engine a
snapshot and the C++ engine took it", which no index exposes.

Log evidence is always taken as a **delta** -- the marker is counted before and
after the action -- because ``docker compose logs`` for a restarted container
still contains everything it printed before the restart.
"""

from __future__ import annotations

import urllib.parse
from typing import NamedTuple

import pytest

from contracts import HTTP_OK
from snapshot_support import (
    CPP_ENTRY_MARKERS,
    CPP_SNAPSHOT_RESTORED,
    CPP_SNAPSHOT_SENT,
    DOCKER_POLL_INTERVAL,
    GO_SNAPSHOT_CAPTURED,
    GO_SNAPSHOT_RESTORED,
    SNAPSHOT_DIR,
    SNAPSHOT_META_FILE,
    SNAPSHOT_STATE_FILE,
    SNAPSHOT_TMP_SUFFIX,
    ComposeProject,
    SnapshotTunables,
    Status,
    count_markers,
    list_snapshots,
    parse_tunables,
    poll,
    require_compose_project,
    require_status,
    try_status,
    wipe_data_dir,
)

# The committed-write contract (200 + JSON + {"ok":true}) is asserted in one
# place for the whole suite; tests/e2e is on sys.path via pytest.ini.
from test_cluster import assert_write_accepted

# The lower-level docker plumbing (the `docker` wrapper, the timeouts, the
# container paths) belongs to test_crash.py, the first module that needed it.
from test_crash import (
    CONTAINER_DATA_DIR,
    CONTAINER_HTTP_PORT,
    LOCAL_HOSTS,
    _describe,
    _env,
)

# Every test in this module stops, wipes or restarts a container. Marking the
# module rather than each function means a test added here later cannot forget.
pytestmark = pytest.mark.requires_docker


# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

#: How many keys to write to push the cluster past the snapshot threshold. Must
#: be comfortably above SNAPSHOT_THRESHOLD in docker-compose.test.yml (20) so
#: that a snapshot is due and there is enough log left over to truncate.
SNAPSHOT_KEY_COUNT = _env("RAFTKV_SNAPSHOT_KEYS", 60, cast=int)

#: Deadline for a snapshot plus a log truncation to show up on every node. Raft
#: checks on a randomised 1x-2x SNAPSHOT_INTERVAL tick, so this is a bound on
#: several checks, not on one.
COMPACTION_TIMEOUT = _env("RAFTKV_COMPACTION_TIMEOUT", 120.0)

#: Deadline for a wiped or restarted node to come back and serve every key:
#: container start, C++ recovery, sidecar start, raft catch-up (a snapshot
#: transfer for the wiped node), and the local apply of the tail.
CATCHUP_TIMEOUT = _env("RAFTKV_CATCHUP_TIMEOUT", 180.0)

#: Poll interval for plain HTTP reads against a node.
READ_POLL_INTERVAL = _env("RAFTKV_POLL_INTERVAL", 0.1)

#: Extra applies tolerated on top of the expected tail when a node restarts:
#: entries committed in the window between reading /status and the container
#: actually going down, plus the no-op entry a leader appends per term.
RESTART_TAIL_SLACK = _env("RAFTKV_RESTART_TAIL_SLACK", 8, cast=int)


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------


@pytest.fixture(scope="module")
def compose_project() -> ComposeProject:
    """A usable local compose project, or a skip explaining what is missing."""
    return require_compose_project()


@pytest.fixture(scope="module")
def services(compose_project: ComposeProject, cluster) -> dict[str, str]:
    """Map every node base URL under test to its compose service name.

    ``compose_project`` is requested first on purpose: both fixtures here are
    module-scoped and ``cluster`` is session-scoped, so argument order decides
    what is set up first, and a missing docker CLI should skip immediately
    rather than after a 30-second hunt for a leader.
    """
    remote = [
        url
        for url in cluster.nodes
        if (urllib.parse.urlsplit(url).hostname or "") not in LOCAL_HOSTS
    ]
    if remote:
        pytest.skip(
            f"RAFTKV_NODES points at non-local nodes ({', '.join(remote)}); these "
            "tests stop, wipe and restart containers of the local docker compose "
            "project, which is only meaningful when the nodes under test are it"
        )

    declared = compose_project.services()
    if not declared:
        pytest.skip(f"`docker compose {compose_project} config --services` listed none")

    published = {
        service: compose_project.published_ports(service, CONTAINER_HTTP_PORT)
        for service in declared
    }

    mapping: dict[str, str] = {}
    for url in cluster.nodes:
        port = urllib.parse.urlsplit(url).port or 80
        for service, ports in published.items():
            if port in ports:
                mapping[url] = service
                break

    unmapped = [url for url in cluster.nodes if url not in mapping]
    if unmapped:
        pytest.skip(
            f"no service in {compose_project} publishes container port "
            f"{CONTAINER_HTTP_PORT} on the host port(s) of {unmapped}; the running "
            "cluster is not this compose project"
        )
    return mapping


@pytest.fixture(scope="module")
def tunables(
    compose_project: ComposeProject, services: dict[str, str]
) -> SnapshotTunables:
    """The snapshot tunables the running sidecars were actually started with.

    Skips -- loudly, with the command to fix it -- when the cluster is running
    production values. This is the guard that keeps ``pytest -m requires_docker``
    safe to run against the ordinary compose cluster: the whole module reports
    "not verified here" instead of timing out three times over.
    """
    seen: dict[str, SnapshotTunables | None] = {
        service: parse_tunables(compose_project.logs(service))
        for service in sorted(set(services.values()))
    }

    unreadable = [service for service, value in seen.items() if value is None]
    if unreadable:
        pytest.skip(
            "could not read the snapshot tunables out of the startup log of "
            f"{unreadable}. main.go logs `Starting sidecar with config: Config{{...}}` "
            "once per start and Config.String() carries SnapshotInterval / "
            "SnapshotThreshold / TrailingLogs; either the log has been rotated away "
            "or that line changed shape."
        )

    lazy = {
        service: str(value) for service, value in seen.items() if not value.is_aggressive
    }
    if lazy:
        pytest.skip(
            "the cluster is running production snapshot settings, so no snapshot "
            f"would happen inside this test run: {lazy}. Restart it with the test "
            "override and run again:\n"
            "  docker compose down -v && rm -rf vol-node1 vol-node2 vol-node3\n"
            "  docker compose -f docker-compose.yml -f docker-compose.test.yml up -d\n"
            "  pytest tests/e2e -m requires_docker -v -rs"
        )

    # All nodes agree in practice (one override file); report a disagreement
    # rather than silently reasoning about the first node's configuration.
    distinct = set(seen.values())
    if len(distinct) > 1:
        per_service = {service: str(value) for service, value in seen.items()}
        pytest.skip(
            f"nodes were started with different snapshot tunables: {per_service}. "
            "These scenarios reason about one cluster-wide configuration."
        )
    return next(iter(distinct))


class Compaction(NamedTuple):
    """The state of the cluster after it has been pushed past the threshold."""

    keys: dict[str, str]
    before: dict[str, Status]
    after: dict[str, Status]


@pytest.fixture(scope="module")
def compacted(
    compose_project: ComposeProject,
    services: dict[str, str],
    tunables: SnapshotTunables,
    cluster,
    run_id: str,
) -> Compaction:
    """Write past the snapshot threshold and wait for every node to compact.

    Shared by all three scenarios, because all three need the same
    precondition: a cluster that has snapshotted and truncated its log. Doing
    the write burst once keeps the module to a single pass over raft.

    It fails rather than skips when compaction never happens: the tunables have
    already been checked by then, so a log that refuses to shrink is the product
    being wrong, not the environment.
    """
    node_services = sorted(set(services.values()))
    before = {
        service: require_status(compose_project, service, COMPACTION_TIMEOUT)
        for service in node_services
    }

    keys = {
        f"e2e-snap-{run_id}-{index:04d}": f"value-{run_id}-{index:04d}"
        for index in range(SNAPSHOT_KEY_COUNT)
    }
    for key, value in keys.items():
        assert_write_accepted(
            cluster, cluster.set(cluster.leader, key, value), cluster.leader
        )

    # One replication check, on the last key: it is ordered after every other
    # write in the raft log, so a node serving it has applied all of them.
    last_key, last_value = list(keys.items())[-1]
    mismatches = cluster.wait_for_value_on_all(
        last_key, last_value, timeout=CATCHUP_TIMEOUT
    )
    assert not mismatches, (
        f"precondition failed: the last of {len(keys)} writes did not replicate to "
        f"every node. Last observation per failing node: {mismatches!r}"
    )

    # Now wait for raft to do its own thing. Both conditions matter and they are
    # different claims: last_snapshot_index moving means a snapshot was taken,
    # first_log_index moving means the log was actually truncated. A node can do
    # the first forever without the second -- that is what TrailingLogs controls.
    observed: dict[str, Status | None] = {service: None for service in node_services}

    def compacted_everywhere() -> bool:
        done = True
        for service in node_services:
            status = try_status(compose_project, service)
            if status is not None:
                observed[service] = status
            current = observed[service]
            if current is None:
                done = False
                continue
            baseline = before[service]
            if current.last_snapshot_index <= baseline.last_snapshot_index:
                done = False
            elif current.first_log_index <= baseline.first_log_index:
                done = False
        return done

    if not poll(compacted_everywhere, COMPACTION_TIMEOUT, DOCKER_POLL_INTERVAL):
        detail = "\n  ".join(
            f"{service}: before[{before[service]}] after[{observed[service]}]"
            for service in node_services
        )
        pytest.fail(
            f"after {len(keys)} writes and {COMPACTION_TIMEOUT:g}s, not every node had "
            f"taken a snapshot AND truncated its log ({tunables}).\n  {detail}\n"
            "last_snapshot_index must rise (a snapshot was taken) and first_log_index "
            "must rise (the log was truncated behind it). first_log_index staying put "
            "while last_snapshot_index moves means TrailingLogs is holding the whole "
            "log; a snapshot store that discards snapshots would leave both at their "
            "starting values.",
            pytrace=False,
        )

    return Compaction(keys=keys, before=before, after=dict(observed))


# --------------------------------------------------------------------------
# Helpers shared by the scenarios
# --------------------------------------------------------------------------


def follower_services(cluster, services: dict[str, str]) -> list[tuple[str, str]]:
    """``(url, service)`` for every follower, leader excluded.

    Never the leader: taking it down forces an election, which would invalidate
    the session-scoped ``leader`` fixture for everything that runs afterwards.
    Leader failover is a Phase 4 scenario.
    """
    return [(url, services[url]) for url in cluster.followers if url in services]


def keys_not_served(cluster, url: str, keys: dict[str, str]) -> dict[str, str]:
    """Keys ``url`` does not currently serve with the expected value."""
    wrong = {}
    for key, value in keys.items():
        observed = cluster.probe_read(url, key)
        if observed.status != HTTP_OK or observed.body != value:
            wrong[key] = str(observed)
    return wrong


def wait_for_every_key(
    cluster, url: str, keys: dict[str, str], timeout: float
) -> dict[str, str]:
    """Poll ``url`` until it serves every key; return what it still gets wrong.

    Polled, unlike the read-back in ``test_crash.py``. There the point was that
    the state had to be on disk *before* the restart, so waiting would have
    hidden the thing under test. Here catching up **is** the thing under test,
    and it legitimately takes as long as a snapshot transfer takes.
    """
    outstanding: dict[str, str] = {"": "never probed"}

    def check() -> bool:
        outstanding.clear()
        outstanding.update(keys_not_served(cluster, url, keys))
        return not outstanding

    poll(check, timeout=timeout, interval=READ_POLL_INTERVAL)
    return dict(outstanding)


def assert_rejoined(cluster, url: str, service: str, run_id: str, suffix: str) -> None:
    """A fresh write on the leader still reaches ``url``.

    Without this, a scenario would pass on a node that recovered its state
    perfectly and then never spoke to raft again.
    """
    key = f"e2e-snap-rejoin-{suffix}-{run_id}"
    value = f"value-rejoin-{suffix}-{run_id}"
    assert_write_accepted(
        cluster, cluster.set(cluster.leader, key, value), cluster.leader
    )

    matched, last = cluster.wait_for(
        url,
        key,
        lambda result: result.status == HTTP_OK and result.body == value,
        timeout=CATCHUP_TIMEOUT,
    )
    assert matched, (
        f"{service} ({url}) serves the historical keys but a NEW write on the leader "
        f"({cluster.leader}) does not reach it: {last}. It recovered its state "
        "without rejoining the cluster."
    )


# --------------------------------------------------------------------------
# Scenario 1 -- a snapshot is taken and the log is truncated
# --------------------------------------------------------------------------


def test_r3_writes_past_the_threshold_snapshot_and_compact_the_log(
    compose_project, services, tunables, compacted
):
    """Acceptance criterion 1: a snapshot file appears and the raft log shrinks.

    Both halves are asserted, because either on its own is compatible with a
    broken system:

    * A snapshot **file** on disk with no index movement is what a mis-wired
      snapshot store produces. The pre-Phase-3 ``DiscardSnapshotStore`` produced
      neither, so a test that only looked for the file would catch that one
      regression and nothing else.
    * ``last_snapshot_index`` moving while ``first_log_index`` stays put is the
      real trap: raft happily snapshots forever while ``TrailingLogs`` retains
      the entire log. The log is only *bounded* when the first index advances.
    """
    for service in sorted(set(services.values())):
        before, after = compacted.before[service], compacted.after[service]

        assert after.last_snapshot_index > before.last_snapshot_index, (
            f"{service} never took a snapshot: last_snapshot_index stayed at "
            f"{after.last_snapshot_index} across {len(compacted.keys)} writes "
            f"({tunables}). before[{before}] after[{after}]"
        )
        assert after.first_log_index > before.first_log_index, (
            f"{service} took a snapshot but did not truncate its log: "
            f"first_log_index stayed at {after.first_log_index}. before[{before}] "
            f"after[{after}] ({tunables}). Raft truncates to "
            "snapshot_index - TrailingLogs, so this is what a too-large TrailingLogs "
            "looks like: the snapshot happens and the log still grows without bound."
        )
        assert after.first_log_index > 1, (
            f"{service} reports first_log_index={after.first_log_index}, so its log "
            f"still begins at the beginning of history. after[{after}]"
        )
        assert not after.log_store_error, (
            f"{service} could not read its first log index: "
            f"{after.log_store_error!r}. first_log_index reads 0 in that case, so "
            "compaction cannot be judged from this node at all."
        )

        # And the snapshot is a real file, written by the file snapshot store
        # that replaced NewDiscardSnapshotStore() (R3.6).
        found = list_snapshots(compose_project, service, running=True)
        assert found.complete, (
            f"{service} reports last_snapshot_index={after.last_snapshot_index} but "
            f"{CONTAINER_DATA_DIR}/{SNAPSHOT_DIR} holds no complete snapshot "
            f"(a directory with both {SNAPSHOT_META_FILE} and {SNAPSHOT_STATE_FILE}, "
            f"whose name does not end in {SNAPSHOT_TMP_SUFFIX}). Present: {found}"
        )

        # The snapshot content came from the C++ engine over GetSnapshot, rather
        # than raft persisting whatever the FSM happened to hand it. Only the log
        # lines show this -- no index distinguishes the two.
        logs = compose_project.logs(service)
        assert count_markers(logs, CPP_SNAPSHOT_SENT) > 0, (
            f"{service} snapshotted, but its C++ engine never logged "
            f"{CPP_SNAPSHOT_SENT!r}. The snapshot raft stored did not come from the "
            "state machine, which is the whole point of the GetSnapshot stream (R3.3)."
        )
        assert count_markers(logs, GO_SNAPSHOT_CAPTURED) > 0, (
            f"{service} snapshotted, but its sidecar never logged "
            f"{GO_SNAPSHOT_CAPTURED!r} -- CppFSM.Snapshot did not capture the state "
            "(R3.4)."
        )


# --------------------------------------------------------------------------
# Scenario 2 -- the headline: wipe a follower's volume and let it rejoin
# --------------------------------------------------------------------------


def test_r3_a_wiped_follower_rejoins_and_catches_up_from_a_snapshot(
    compose_project, services, tunables, compacted, cluster, run_id
):
    """Acceptance criterion 2, and the scenario this whole phase exists for.

    Stop a follower, delete **everything** in its data directory -- ``kv.db``,
    ``kv.wal``, ``logs.dat`` and its own ``snapshots/`` -- start it again, and
    require it to serve every key.

    Before this phase that was impossible: the only way to catch a blank node up
    was to replay the entire log into it, which the leader can no longer do
    because it no longer has the entire log.

    Two independent facts establish that the catch-up was a **snapshot
    transfer** and not a log replay, and both are asserted rather than assumed:

    1. ``first_log_index`` on the leader, read immediately before the wipe, is
       greater than 1. The entries below it are gone, and a node starting from
       index 0 cannot be brought forward with AppendEntries alone, so raft has
       no option but ``InstallSnapshot``.
    2. ``last_snapshot_index`` on the wiped node is greater than 0 afterwards,
       and its snapshot store was verified empty while it was down. A snapshot
       it did not have and did not take can only have come from the leader.

    The log markers are checked on top of those, and they say something the
    indexes cannot: that the snapshot was pushed through ``RestoreSnapshot``
    into the C++ engine (R3.3/R3.5), rather than being accepted by raft and
    dropped on the floor by the FSM.
    """
    followers = follower_services(cluster, services)
    if not followers:
        pytest.skip("no follower to wipe: the cluster under test has a single node")
    victim_url, victim_service = followers[0]
    leader_service = services[cluster.leader]

    # 1. The precondition the whole argument rests on, measured now rather than
    #    inherited from the fixture.
    leader_before = require_status(compose_project, leader_service, COMPACTION_TIMEOUT)
    assert leader_before.first_log_index > 1, (
        f"the leader ({leader_service}) still holds its log from index "
        f"{leader_before.first_log_index}, so it could catch a blank follower up by "
        "replaying it. This test cannot distinguish a snapshot transfer from a full "
        f"replay in that state. Leader status: {leader_before} ({tunables})"
    )

    # 2. Stop it. `stop`, not `kill -9`: crash durability is Phase 2's claim and
    #    test_crash.py proves it. What matters here is that the node goes away
    #    and comes back with nothing.
    stopped = compose_project.run("stop", victim_service)
    assert stopped.returncode == 0, (
        f"could not stop {victim_service} -- {_describe(stopped)}"
    )

    restarted = False
    try:
        # 3. Delete the volume contents and prove they are gone. This step is
        #    what makes the test mean anything: a partially wiped node could
        #    catch up from its own leftovers.
        emptied, detail = wipe_data_dir(compose_project, victim_service)
        assert emptied, (
            f"could not empty {victim_service}'s data directory "
            f"({CONTAINER_DATA_DIR}) -- {detail}. Without a verified-empty volume "
            "this test proves nothing: the node could serve the keys from files it "
            "still had."
        )

        leftover = list_snapshots(compose_project, victim_service, running=False)
        assert not leftover.listing, (
            f"{victim_service}'s snapshot store survived the wipe: {leftover}. A "
            "local snapshot would let the node restore itself, which is not the "
            "mechanism under test."
        )

        # 4. Bring it back. `start` reuses the container, so this is a node with
        #    the right identity and no state whatsoever.
        started = compose_project.run("start", victim_service)
        restarted = True
        assert started.returncode == 0, (
            f"could not restart {victim_service} after wiping it -- "
            f"{_describe(started)}. The cluster is now short a node; recover with "
            f"`docker compose {compose_project} start {victim_service}`"
        )

        # 5. It must serve every key that was written before it was wiped.
        outstanding = wait_for_every_key(
            cluster, victim_url, compacted.keys, CATCHUP_TIMEOUT
        )
        if outstanding:
            tail_logs = compose_project.run(
                "logs", "--no-color", "--tail", "60", victim_service
            )
            pytest.fail(
                f"{victim_service} ({victim_url}) did not catch up within "
                f"{CATCHUP_TIMEOUT:g}s of being wiped and restarted: "
                f"{len(outstanding)} of {len(compacted.keys)} keys are missing or "
                f"wrong, e.g. {dict(list(outstanding.items())[:5])}.\n"
                f"The leader's log begins at index {leader_before.first_log_index}, "
                "so the only way to catch this node up is an InstallSnapshot "
                "followed by the tail.\nLast log lines:\n"
                + tail_logs.stdout
                + tail_logs.stderr,
                pytrace=False,
            )

        # 6. It got there by snapshot transfer. `last_snapshot_index > 0` on a
        #    node whose snapshot store was empty a minute ago is unambiguous:
        #    raft sets it when it takes a snapshot (this node has applied almost
        #    nothing of its own) or when it installs one from a peer.
        after = require_status(compose_project, victim_service, CATCHUP_TIMEOUT)
        assert after.last_snapshot_index > 0, (
            f"{victim_service} caught up but reports last_snapshot_index=0, so raft "
            "never installed a snapshot into it -- it must have been fed the log "
            f"instead. Status: {after}. The leader's log began at index "
            f"{leader_before.first_log_index} at the time, so that should not have "
            "been possible; if it was, the leader is retaining more log than "
            "TrailingLogs implies."
        )

        # 7. And the snapshot reached the C++ engine. That is a different claim
        #    from "raft installed one", and it is the one R3.5 makes: a node that
        #    cannot install the snapshot must not serve.
        logs = compose_project.logs(victim_service)
        assert count_markers(logs, GO_SNAPSHOT_RESTORED) > 0, (
            f"{victim_service} reports last_snapshot_index="
            f"{after.last_snapshot_index} but its sidecar never logged "
            f"{GO_SNAPSHOT_RESTORED!r}: CppFSM.Restore did not complete a restore "
            "(R3.5)."
        )
        assert count_markers(logs, CPP_SNAPSHOT_RESTORED) > 0, (
            f"{victim_service}'s C++ engine never logged {CPP_SNAPSHOT_RESTORED!r}, "
            "so the snapshot was never installed into the store it serves reads from "
            "(R3.3/R3.7)."
        )

        # 8. It is a member again, not just a node holding the right bytes.
        assert_rejoined(cluster, victim_url, victim_service, run_id, "wiped")

    finally:
        # A run that dies between the stop and the start would leave the cluster
        # a node short for every test after this one.
        if not restarted:
            compose_project.run("start", victim_service)


# --------------------------------------------------------------------------
# Scenario 3 -- an ordinary restart replays the tail, not all of history
# --------------------------------------------------------------------------


def test_r3_a_normal_restart_applies_a_snapshot_and_the_tail(
    compose_project, services, tunables, compacted, cluster, run_id
):
    """Acceptance criterion 3: startup is snapshot + tail, not full history.

    Before this phase every start replayed the entire BoltDB log through
    ``StateMachine.Apply`` -- which is why ``test_crash.py`` has to assert on the
    killed node's disk while it is dead, and why the WAL grew on every restart.

    The measurement is a delta on the C++ engine's own per-entry log lines. Each
    committed entry reaching the state machine prints exactly one of them, so
    counting the lines a restart produces counts the entries it replayed. The
    expected number is the tail above the snapshot, read from ``/status``
    immediately before the restart; "replayed everything" would be
    ``last_log_index``, which is larger by construction because the fixture has
    already established that this node's log begins well past 1.

    What this does NOT prove: that the restart was *fast*. Wall-clock timing on
    a shared CI runner is not evidence of anything, so it is not asserted -- the
    entry count is the same claim without the noise.
    """
    followers = follower_services(cluster, services)
    if not followers:
        pytest.skip("no follower to restart: the cluster under test has a single node")

    # Prefer a follower the previous scenario did not touch, so this exercises a
    # node with a long history and a snapshot it took itself rather than one that
    # was blank a minute ago. Falls back to the only follower on a 2-node cluster.
    victim_url, victim_service = followers[-1]

    before = require_status(compose_project, victim_service, COMPACTION_TIMEOUT)
    assert before.last_snapshot_index > 0, (
        f"{victim_service} has no snapshot to restore from ({before}); the "
        "precondition fixture should have guaranteed one."
    )
    assert before.first_log_index > 1, (
        f"{victim_service} still holds its whole log ({before}), so 'restored the "
        "snapshot' and 'replayed everything' would replay the same entries and "
        "there is no distinction to measure."
    )

    tail = before.last_log_index - before.last_snapshot_index
    entries_before = compose_project.count_markers(victim_service, *CPP_ENTRY_MARKERS)
    restores_before = compose_project.count_markers(victim_service, GO_SNAPSHOT_RESTORED)

    restarted = compose_project.run("restart", victim_service)
    assert restarted.returncode == 0, (
        f"could not restart {victim_service} -- {_describe(restarted)}"
    )

    outstanding = wait_for_every_key(
        cluster, victim_url, compacted.keys, CATCHUP_TIMEOUT
    )
    assert not outstanding, (
        f"{victim_service} ({victim_url}) did not serve every key within "
        f"{CATCHUP_TIMEOUT:g}s of an ordinary restart: {len(outstanding)} of "
        f"{len(compacted.keys)} missing or wrong, e.g. "
        f"{dict(list(outstanding.items())[:5])}"
    )

    entries_after = compose_project.count_markers(victim_service, *CPP_ENTRY_MARKERS)
    restores_after = compose_project.count_markers(victim_service, GO_SNAPSHOT_RESTORED)

    assert restores_after > restores_before, (
        f"{victim_service} restarted without restoring its local snapshot: the "
        f"sidecar logged {GO_SNAPSHOT_RESTORED!r} {restores_before} time(s) before "
        f"and {restores_after} after. raft.NewRaft loads the newest snapshot before "
        "it replays anything, so a start that skips it is a start that replays "
        "everything."
    )

    replayed = entries_after - entries_before
    assert replayed <= tail + RESTART_TAIL_SLACK, (
        f"{victim_service} replayed {replayed} log entries into its state machine on "
        f"restart, more than the {tail} entries that sat above its snapshot "
        f"(+{RESTART_TAIL_SLACK} slack for entries committed while it was going "
        f"down). Its log ran from index {before.first_log_index} to "
        f"{before.last_log_index} with a snapshot at "
        f"{before.last_snapshot_index}.\nA count near {before.last_log_index} means "
        "the node replayed its history instead of restoring the snapshot and "
        "applying the tail -- the pre-Phase-3 behaviour."
    )

    assert_rejoined(cluster, victim_url, victim_service, run_id, "restarted")
