"""Crash recovery: an acknowledged write survives ``kill -9`` (Phase 2).

This is the one module in the suite that touches docker, and it is quarantined
behind the ``requires_docker`` marker for exactly that reason.

**Why the rule it breaks exists.** Every other test here runs against an
*already-running* cluster and never starts, stops or builds anything: the
compose lifecycle belongs to the CI ``e2e`` job and to the ``cluster-smoke-test``
skill (see tests/e2e/README.md and the conftest docstring). That keeps the suite
runnable against any topology -- a remote cluster, a k8s namespace, a colleague's
laptop -- because it makes no assumption about *how* the nodes are run.

**Why this module has to break it.** The Phase 2 acceptance criterion is
"``kill -9`` at any instant loses no acknowledged write". There is no way to
express that without killing a process, and SIGKILL specifically: ``docker
compose stop`` sends SIGTERM, which is a graceful shutdown and proves nothing
about durability.

So the split is explicit rather than silent. ``pytest.ini`` sets
``addopts = -m "not requires_docker"``, so the default ``pytest tests/e2e`` stays
docker-free and still works against a remote cluster; this module runs only when
you opt in::

    pytest tests/e2e -m requires_docker -v

Everything here *skips* rather than fails when the environment cannot support
it: no docker CLI, no daemon, no compose project, or ``RAFTKV_NODES`` pointed at
something other than the local compose cluster. A skip means "not verified
here", never "verified".

What the test proves, and what it cannot
----------------------------------------

The naive version of this test -- write, kill, restart, read back -- proves
almost nothing about durability, because the node would pass it with an empty
store. There are **no raft snapshots** (``DiscardSnapshotStore`` +
``DummySnapshot``, Phase 3), so on every start the sidecar replays the entire
local BoltDB log through ``FSM.Apply`` into the C++ state machine. A node whose
kv.db and kv.wal were both blank would be refilled from its own raft log within
a second and answer every read correctly.

So the load-bearing assertion is made **while the node is dead**: its data
directory must already contain the keys, before anything can have replayed. That
is a statement about the durability layer alone -- raft is not running, the
peers are not involved, and the bytes are simply on the disk or they are not.

The post-restart read-back is then deliberately **not polled**. Recovery
finishes in the ``PersistentKVStore`` constructor, before ``main()`` opens the
HTTP listener, and entrypoint.sh launches the sidecar only after the C++ gRPC
port answers -- so the first request the node accepts is served from
disk-recovered state, with raft replay still starting up. That is a race, not a
guarantee, which is why it supplements the on-disk check rather than replacing
it.

If the data directory cannot be read from the test host (named volume, remote
docker daemon, root-only permissions) the test still runs the kill/restart
scenario -- which independently catches a node that refuses to come back at all,
the failure mode a torn WAL tail would produce -- and warns loudly that the
durability half was not verified.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import urllib.parse
import warnings
from pathlib import Path
from typing import NamedTuple

import pytest

from contracts import HTTP_OK

# The committed-write contract (200 + JSON + {"ok":true}) is asserted in exactly
# one place for the whole suite. Importing it beats restating three assertions
# that would then drift; tests/e2e is on sys.path via pytest.ini's `pythonpath`.
from test_cluster import assert_write_accepted

# Every test in this module kills a container. Marking the module rather than
# each function means a test added here later cannot forget the marker.
pytestmark = pytest.mark.requires_docker


# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

#: Repo root: tests/e2e/test_crash.py -> tests/e2e -> tests -> <repo>.
REPO_ROOT = Path(__file__).resolve().parents[2]


def _env(name: str, default, cast=float):
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        return cast(raw)
    except ValueError as exc:  # pragma: no cover - operator error
        raise ValueError(f"{name} must be a number, got {raw!r}") from exc


#: Compose file identifying the project to kill a container in. Passing it
#: explicitly makes the test independent of the working directory, and compose
#: derives the project name from its parent directory exactly as
#: `docker compose up -d` in the repo root does.
COMPOSE_FILE = Path(
    os.environ.get("RAFTKV_COMPOSE_FILE", "").strip()
    or REPO_ROOT / "docker-compose.yml"
)

#: Budget for one docker CLI invocation. `kill` and `start` are fast; `inspect`
#: and `ps` are faster. Generous because a loaded CI runner is not fast.
DOCKER_TIMEOUT = _env("RAFTKV_DOCKER_TIMEOUT", 60.0)

#: Deadline for a restarted node to answer HTTP again: container start, the C++
#: process, and store recovery (which replays kv.wal).
RESTART_TIMEOUT = _env("RAFTKV_RESTART_TIMEOUT", 90.0)

#: How many distinct keys to write before the kill. Large enough that a partial
#: loss is visible and that the on-disk check is not a single lucky byte range;
#: small enough that the writes are a couple of seconds of raft round trips.
CRASH_KEY_COUNT = _env("RAFTKV_CRASH_KEYS", 25, cast=int)

#: Container port the HTTP API listens on (docker-compose.yml publishes it as
#: 8080/8081/8082 on the host). Used to map a node URL back to a service name.
CONTAINER_HTTP_PORT = 8080

#: Data directory inside the container (entrypoint.sh `DATA_DIR`), bind-mounted
#: from ./vol-node* by docker-compose.yml.
CONTAINER_DATA_DIR = "/app/data"

#: The three files the C++ storage engine owns in that directory.
BASE_FILE = "kv.db"  # KVB1 snapshot, rewritten atomically (R2.1/R2.2)
WAL_FILE = "kv.wal"  # append-only log, fsynced per record (R2.4)
TEMP_FILE = "kv.db.tmp"  # transient half-written base file (R2.2)

#: Hostnames for which "kill the container serving this URL" is meaningful.
LOCAL_HOSTS = frozenset({"localhost", "127.0.0.1", "::1", "0.0.0.0"})


class DurabilityNotVerified(UserWarning):
    """The kill/restart ran but the on-disk evidence could not be read.

    Its own class so it is greppable in a CI log and filterable in pytest.ini,
    and so it can never be mistaken for an unrelated warning.
    """


# --------------------------------------------------------------------------
# docker compose plumbing
# --------------------------------------------------------------------------


def _docker(*args: str, timeout: float = DOCKER_TIMEOUT) -> subprocess.CompletedProcess:
    """Run one ``docker`` command and capture it. Never raises on a non-zero exit.

    A fixed argument vector, never a shell string: nothing here interpolates
    into a command line.
    """
    return subprocess.run(
        ["docker", *args],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def _describe(result: subprocess.CompletedProcess) -> str:
    """One-line rendering of a finished docker command, for failure messages."""
    return (
        f"exit {result.returncode} "
        f"stdout={result.stdout.strip()!r} stderr={result.stderr.strip()!r}"
    )


class Compose:
    """The subset of ``docker compose`` this test needs, bound to one project."""

    def __init__(self, compose_file: Path) -> None:
        self.file = str(compose_file)

    def run(self, *args: str, timeout: float = DOCKER_TIMEOUT):
        return _docker("compose", "-f", self.file, *args, timeout=timeout)

    def services(self) -> list[str]:
        """Service names declared in the compose file."""
        result = self.run("config", "--services")
        if result.returncode != 0:
            return []
        return [line.strip() for line in result.stdout.splitlines() if line.strip()]

    def container_id(self, service: str) -> str | None:
        """Container id backing ``service``, running or not."""
        result = self.run("ps", "--all", "--quiet", service)
        if result.returncode != 0:
            return None
        ids = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        return ids[0] if ids else None

    def published_ports(self, service: str, container_port: int) -> set[int]:
        """Host ports ``container_port`` is published on (may be v4 and v6)."""
        result = self.run("port", service, str(container_port))
        if result.returncode != 0:
            return set()
        ports = set()
        for line in result.stdout.splitlines():
            _, _, tail = line.strip().rpartition(":")
            if tail.isdigit():
                ports.add(int(tail))
        return ports

    def host_data_dir(self, service: str) -> Path | None:
        """Host path of the container's data directory, if it is readable here.

        ``None`` when the mount is a named volume, when the daemon is remote, or
        when the path is simply not there -- all of which mean "cannot inspect",
        not "durability is broken".
        """
        container = self.container_id(service)
        if container is None:
            return None

        result = _docker("inspect", "--format", "{{json .Mounts}}", container)
        if result.returncode != 0:
            return None
        try:
            mounts = json.loads(result.stdout or "null")
        except ValueError:
            return None
        if not isinstance(mounts, list):
            return None

        for mount in mounts:
            if not isinstance(mount, dict):
                continue
            if mount.get("Destination") != CONTAINER_DATA_DIR:
                continue
            source = mount.get("Source")
            if not source:
                continue
            path = Path(source)
            return path if path.is_dir() else None
        return None


class Victim(NamedTuple):
    """The node this test kills."""

    url: str
    service: str
    data_dir: Path | None


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------


@pytest.fixture(scope="session")
def compose() -> Compose:
    """A usable local compose project, or a skip explaining what is missing."""
    if shutil.which("docker") is None:
        pytest.skip("no `docker` CLI on PATH; the crash test cannot kill a node")

    try:
        version = _docker("compose", "version")
    except (OSError, subprocess.SubprocessError) as exc:  # pragma: no cover
        pytest.skip(f"`docker compose version` could not be run: {exc}")
    if version.returncode != 0:
        pytest.skip(
            "`docker compose` (the CLI plugin) is not available -- "
            f"{_describe(version)}. The standalone docker-compose v1 binary is "
            "not supported here, and is not what CI uses."
        )

    if not COMPOSE_FILE.is_file():
        pytest.skip(
            f"no compose file at {COMPOSE_FILE}; set RAFTKV_COMPOSE_FILE to the "
            "one whose project is running"
        )

    project = Compose(COMPOSE_FILE)
    listed = project.run("ps", "--all", "--quiet")
    if listed.returncode != 0:
        pytest.skip(
            f"`docker compose -f {COMPOSE_FILE} ps` failed -- {_describe(listed)}. "
            "Is the docker daemon running?"
        )
    if not listed.stdout.strip():
        pytest.skip(
            f"the compose project for {COMPOSE_FILE} has no containers. Start it "
            "with `docker compose up -d` from the repo root, or export "
            "COMPOSE_PROJECT_NAME if it was started under a different project name."
        )
    return project


@pytest.fixture
def victim(compose: Compose, cluster) -> Victim:
    """The node to kill: a follower of the local compose project.

    ``compose`` is requested before ``cluster`` on purpose: both are
    session-scoped, so argument order decides which is set up first, and a
    missing docker CLI or a compose project that was never started should skip
    immediately rather than after a 30-second hunt for a leader that is not
    there.

    A follower, not the leader: killing the leader forces an election, which
    would invalidate the session-scoped ``leader`` fixture for anything that
    runs after this test. Leader-failover is its own scenario and belongs to
    Phase 4 (ROADMAP.md), which is where "kill the leader mid-load" is
    specified.
    """
    remote = [
        url
        for url in cluster.nodes
        if (urllib.parse.urlsplit(url).hostname or "") not in LOCAL_HOSTS
    ]
    if remote:
        pytest.skip(
            f"RAFTKV_NODES points at non-local nodes ({', '.join(remote)}); this "
            "test kills a container of the local docker compose project, which "
            "is only meaningful when the nodes under test are that project"
        )

    target = cluster.followers[0] if cluster.followers else cluster.leader
    port = urllib.parse.urlsplit(target).port or 80

    services = compose.services()
    if not services:
        pytest.skip(f"`docker compose -f {COMPOSE_FILE} config --services` listed none")

    for service in services:
        if port in compose.published_ports(service, CONTAINER_HTTP_PORT):
            return Victim(target, service, compose.host_data_dir(service))

    pytest.skip(
        f"no service in {COMPOSE_FILE} publishes container port "
        f"{CONTAINER_HTTP_PORT} on host port {port} (the node under test at "
        f"{target}); the running cluster is not this compose project"
    )
    raise AssertionError("unreachable")  # pragma: no cover - pytest.skip raises


# --------------------------------------------------------------------------
# On-disk evidence
# --------------------------------------------------------------------------


def _read_data_dir(data_dir: Path) -> dict[str, bytes]:
    """Read the store's files while the node is dead.

    Absent files are omitted rather than reported as empty: on a cluster that
    has never crossed a compaction threshold there is legitimately no ``kv.db``
    at all, only ``kv.wal``.

    Any other ``OSError`` (a permission problem, most likely) is left to
    propagate; the caller turns it into "not verified" rather than a failure,
    because it says something about the test host, not about the store.
    """
    contents = {}
    for name in (BASE_FILE, WAL_FILE, TEMP_FILE):
        try:
            contents[name] = (data_dir / name).read_bytes()
        except FileNotFoundError:
            continue
    return contents


def _assert_durable_on_disk(
    files: dict[str, bytes], written: dict[str, str], victim: Victim
) -> None:
    """Every acknowledged write is in the killed node's files. The real test.

    Both the base file and the WAL store keys and values as raw bytes with no
    escaping -- KVB1 as ``uint32 len | bytes``, the WAL as a msgpack payload --
    so a plain substring search is a format-independent way to ask "are these
    bytes on the disk". Which of the two files holds a given write depends only
    on whether compaction has run, so they are searched together.

    The substring check does not prove the key/value *association* on disk; the
    read-back after the restart is what pins that. What it does prove is the
    part that cannot be observed any other way: the bytes were durable before
    the node restarted, so no later replay can explain them.
    """
    blob = files.get(BASE_FILE, b"") + files.get(WAL_FILE, b"")

    assert blob, (
        f"{victim.service} was killed after acknowledging {len(written)} writes, "
        f"and its data directory ({victim.data_dir}) contains neither a "
        f"non-empty {BASE_FILE} nor a non-empty {WAL_FILE}. Every acknowledged "
        "write should have been fsynced to the WAL before the HTTP 200 was "
        f"sent (R2.4/R2.7). Files present: {sorted(files)}"
    )

    missing = sorted(
        key
        for key, value in written.items()
        if key.encode() not in blob or value.encode() not in blob
    )
    assert not missing, (
        f"{len(missing)} of {len(written)} acknowledged writes are not in "
        f"{victim.service}'s on-disk state after SIGKILL: {missing[:5]}"
        f"{' ...' if len(missing) > 5 else ''}.\n"
        f"Sizes: {BASE_FILE}={len(files.get(BASE_FILE, b''))}B, "
        f"{WAL_FILE}={len(files.get(WAL_FILE, b''))}B in {victim.data_dir}.\n"
        "This is the Phase 2 contract: PersistentKVStore::set appends the "
        "command to the WAL and fsyncs it BEFORE mutating the in-memory map, so "
        "a key the node served cannot be a key the node never wrote down."
    )


def _assert_base_file_is_binary(files: dict[str, bytes], victim: Victim) -> None:
    """If a base file exists it is KVB1, not the pre-Phase-2 line format (R2.1).

    Automates the spec's manual ``xxd vol-node1/kv.db | head`` sanity check. Only
    checked when the file is there and non-empty: a cluster that has not
    compacted yet never writes one, and the legacy writer could leave a 0-byte
    file behind (which the next start migrates away).
    """
    base = files.get(BASE_FILE)
    if not base:
        return
    assert base.startswith(b"KVB1"), (
        f"{victim.data_dir / BASE_FILE} does not start with the KVB1 magic; "
        f"first bytes were {base[:16]!r}. Either the node is running a "
        "pre-Phase-2 image, or migration (R2.3) did not rewrite the legacy "
        "line-based file at startup."
    )


# --------------------------------------------------------------------------
# The test
# --------------------------------------------------------------------------


def test_r2_acknowledged_writes_survive_sigkill(compose, victim, cluster, run_id):
    """Write K keys, SIGKILL a node, restart it, and find every key still there.

    The Phase 2 acceptance criterion, end to end. Read the module docstring for
    why the on-disk check between the kill and the restart is the part that
    means something.
    """
    written = {
        f"e2e-crash-{run_id}-{index:03d}": f"value-{run_id}-{index:03d}"
        for index in range(CRASH_KEY_COUNT)
    }

    # 1. Write every key through the leader and confirm each was committed.
    for key, value in written.items():
        response = cluster.set(cluster.leader, key, value)
        assert_write_accepted(cluster, response, cluster.leader)

    # 2. Confirm they replicated -- in particular that the victim applied them,
    #    which is what makes them acknowledged writes *on that node*. A value it
    #    can serve is a value it has already fsynced (set() appends to the WAL
    #    before touching the map).
    for key, value in written.items():
        mismatches = cluster.wait_for_value_on_all(key, value)
        assert not mismatches, (
            f"precondition failed: {key!r} did not replicate to every node "
            f"before the kill. Last observation per failing node: {mismatches!r}"
        )

    # 3. SIGKILL. Not `docker compose stop`: that is SIGTERM, an orderly
    #    shutdown, and it would prove nothing about crash durability.
    killed = compose.run("kill", "-s", "KILL", victim.service)
    assert killed.returncode == 0, (
        f"could not SIGKILL {victim.service} -- {_describe(killed)}"
    )

    # 4. While it is dead, look at what actually reached the disk. Wrapped so
    #    the node is restarted even when an assertion here fails -- leaving a
    #    node down would break every test that runs afterwards.
    try:
        if victim.data_dir is None:
            _warn_not_verified(
                f"the data directory of {victim.service} is not reachable from "
                "this host (a named volume, or a remote docker daemon)"
            )
        else:
            try:
                files = _read_data_dir(victim.data_dir)
            except OSError as exc:
                _warn_not_verified(f"{victim.data_dir} could not be read: {exc}")
            else:
                # A leftover kv.db.tmp is legitimate here and deliberately not
                # asserted against: crashing between the temp write and the
                # rename is exactly the case atomic_write_file exists to make
                # survivable (R2.2). The base file is either the old complete
                # one or the new complete one, never a mixture.
                _assert_durable_on_disk(files, written, victim)
                _assert_base_file_is_binary(files, victim)
    finally:
        started = compose.run("start", victim.service)

    assert started.returncode == 0, (
        f"could not restart {victim.service} after killing it -- "
        f"{_describe(started)}. The cluster is now short a node; "
        f"`docker compose -f {COMPOSE_FILE} start {victim.service}`"
    )

    # 5. Wait only for the node to answer at all, with a key nothing wrote.
    probe_key = f"e2e-crash-probe-{run_id}"
    reachable, last = cluster.wait_for(
        victim.url,
        probe_key,
        lambda result: result.reachable,
        timeout=RESTART_TIMEOUT,
    )
    if not reachable:
        logs = compose.run("logs", "--no-color", "--tail", "40", victim.service)
        pytest.fail(
            f"{victim.service} ({victim.url}) did not serve a read within "
            f"{RESTART_TIMEOUT:g}s of being restarted; last attempt: {last}.\n"
            "A node that never comes back is itself a durability failure: "
            "PersistentKVStore recovers in its constructor, and if it throws "
            "(a corrupt base file, a WAL it will not heal) main() exits 1, "
            "entrypoint.sh exits with it, and docker-compose.yml declares no "
            f"restart policy.\nLast log lines:\n{logs.stdout}{logs.stderr}",
            pytrace=False,
        )

    # 6. Read every key back from the node that died. Deliberately NOT polled:
    #    recovery completes in the store constructor before main() opens the
    #    listener, so the first answered request already reflects the whole
    #    recovered state. Polling here would also wait out the sidecar's raft
    #    replay, which refills the store from the local BoltDB log and would
    #    mask a store that recovered nothing (there are no snapshots yet --
    #    Phase 3). The on-disk assertion above is the deterministic version of
    #    this argument; this is the cheap confirmation.
    lost = {}
    for key, value in written.items():
        observed = cluster.probe_read(victim.url, key)
        if observed.status != HTTP_OK or observed.body != value:
            lost[key] = str(observed)

    assert not lost, (
        f"{len(lost)} of {len(written)} acknowledged writes are missing or wrong "
        f"on {victim.service} ({victim.url}) after SIGKILL and restart: "
        f"{dict(list(lost.items())[:5])}"
        f"{' ...' if len(lost) > 5 else ''}"
    )

    # 7. And it rejoined: a fresh write on the leader reaches it again. Without
    #    this the test would pass on a node that recovered its disk state
    #    perfectly and then never spoke to raft again.
    rejoin_key = f"e2e-crash-rejoin-{run_id}"
    rejoin_value = f"value-rejoin-{run_id}"
    rejoin = cluster.set(cluster.leader, rejoin_key, rejoin_value)
    assert_write_accepted(cluster, rejoin, cluster.leader)

    mismatches = cluster.wait_for_value_on_all(
        rejoin_key, rejoin_value, timeout=RESTART_TIMEOUT
    )
    assert not mismatches, (
        f"after {victim.service} was killed and restarted, a new write on the "
        f"leader ({cluster.leader}) no longer reaches every node: {mismatches!r}"
    )


def _warn_not_verified(reason: str) -> None:
    """Say plainly that the durability half of the test did not happen."""
    warnings.warn(
        "The kill/restart scenario ran, but the durability assertion did NOT: "
        f"{reason}. Without reading the killed node's data directory this test "
        "cannot tell a store that survived the crash from one that lost "
        "everything and was refilled by raft log replay on restart (there are "
        "no snapshots yet, so replay rebuilds the full state from the local "
        "BoltDB log). Treat the result as 'the node came back', not as "
        "'the write was durable'.",
        DurabilityNotVerified,
        stacklevel=2,
    )
