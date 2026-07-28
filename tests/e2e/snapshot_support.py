"""Cluster plumbing for the Phase 3 snapshot scenarios.

A plain module, not a test module -- pytest collects ``test_*.py`` only, so
nothing here runs on its own. It exists for the same reason ``contracts.py``
does: to keep one definition of something two places would otherwise restate.
``test_snapshot.py`` holds the scenarios and their assertions; everything it
needs in order to *drive* a cluster lives here.

Three capabilities, all of which the rest of the suite deliberately does not
have:

* **`docker compose` across several ``-f`` files.** ``test_crash.py``'s
  single-file ``Compose`` is enough to kill and start a container. The snapshot
  scenarios stop, wipe and restart a service, and every one of those has to
  carry the same file set the cluster was started with -- otherwise compose
  resolves the service from the base file alone and silently drops the
  aggressive snapshot flags the scenarios depend on.
* **Reading ``:6000/status``.** ``first_log_index`` and ``last_snapshot_index``
  are the only machine-readable evidence that a snapshot happened and that the
  log was truncated, and the management port is not published to the host, so
  this goes through ``docker compose exec ... curl`` exactly as the CI readiness
  poll does.
* **Looking at, and emptying, a node's data directory from inside a container.**
  The bind mounts are written by root inside the container, so a Linux CI runner
  cannot reliably read or delete them from the host, and a named volume or a
  remote daemon could not be reached from the host at all.

The lower-level docker plumbing -- the ``docker`` subprocess wrapper, the
timeouts, the container paths -- is imported from ``test_crash.py``, which was
the first module in the suite to need it. Importing rather than copying is the
same call the suite already makes with ``assert_write_accepted``.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path
from typing import NamedTuple

import pytest
import requests

from test_crash import (
    CONTAINER_DATA_DIR,
    DOCKER_TIMEOUT,
    REPO_ROOT,
    Compose,
    _describe,
    _docker,
    _env,
)

__all__ = [
    "CONFIG_LINE_RE",
    "CPP_APPLY_COUNTER",
    "CPP_SNAPSHOT_RESTORED",
    "CPP_SNAPSHOT_SENT",
    "DEFAULT_COMPOSE_FILES",
    "DOCKER_POLL_INTERVAL",
    "GO_SNAPSHOT_CAPTURED",
    "GO_SNAPSHOT_RESTORED",
    "MGMT_PORT",
    "SNAPSHOT_DIR",
    "SNAPSHOT_META_FILE",
    "SNAPSHOT_STATE_FILE",
    "SNAPSHOT_TMP_SUFFIX",
    "ComposeProject",
    "SnapshotDirectory",
    "SnapshotTunables",
    "Status",
    "StatusUnavailable",
    "compose_files",
    "count_markers",
    "list_snapshots",
    "parse_tunables",
    "read_apply_counter",
    "poll",
    "read_status",
    "require_compose_project",
    "require_status",
    "try_status",
    "wipe_data_dir",
]


# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

#: Compose files identifying the project under test, in ``-f`` order. Override
#: with a comma-separated ``RAFTKV_COMPOSE_FILES``.
DEFAULT_COMPOSE_FILES = ("docker-compose.yml", "docker-compose.test.yml")

#: Management API port inside the container (not published by docker-compose.yml).
MGMT_PORT = 6000

#: Where raft.FileSnapshotStore puts snapshots, relative to the data directory.
#: One directory per snapshot, named ``<term>-<index>-<millis>``, holding
#: ``meta.json`` and ``state.bin``; an in-progress one carries a ``.tmp`` suffix.
SNAPSHOT_DIR = "snapshots"
SNAPSHOT_STATE_FILE = "state.bin"
SNAPSHOT_META_FILE = "meta.json"
SNAPSHOT_TMP_SUFFIX = ".tmp"

#: Poll interval for status and log probes. One second, not the suite's 0.1s:
#: each of these costs a `docker compose exec`, which is not free.
DOCKER_POLL_INTERVAL = _env("RAFTKV_DOCKER_POLL_INTERVAL", 1.0)

#: Ceilings above which a cluster counts as "not configured for these tests".
#: Anything at or below them snapshots quickly enough to observe inside a test
#: run; the production defaults (8192 / 10240) are far above and produce a skip.
MAX_TEST_SNAPSHOT_THRESHOLD = _env("RAFTKV_MAX_SNAPSHOT_THRESHOLD", 512, cast=int)
MAX_TEST_TRAILING_LOGS = _env("RAFTKV_MAX_TRAILING_LOGS", 512, cast=int)


# --------------------------------------------------------------------------
# Log markers
# --------------------------------------------------------------------------
#
# Every marker below is a log line emitted by code in THIS repository, named
# here so that changing one of them is a visible diff against the tests that
# depend on it. They are the only way to tell "raft installed a snapshot" from
# "the C++ engine took it": /status reports the former, nothing reports the
# latter.

#: go-sidecar/internal/fsm/fsm.go, CppFSM.Snapshot -- state captured from C++.
GO_SNAPSHOT_CAPTURED = "fsm: captured a"

#: go-sidecar/internal/fsm/fsm.go, CppFSM.Restore -- snapshot pushed into C++.
GO_SNAPSHOT_RESTORED = "fsm: restored a"

#: cpp-app/src/raft/state_machine.hpp, GetSnapshot -- state streamed out.
#:
#: Phase 5 replaced the C++ side's `[Prefix] Message:` prose with JSON lines, so
#: these match the `msg` field rather than a bracketed prefix. Matching the field
#: (`"msg":"snapshot sent"`) and not the whole object keeps them stable against
#: fields being added alongside.
CPP_SNAPSHOT_SENT = '"msg":"snapshot sent"'

#: cpp-app/src/raft/state_machine.hpp, snapshot::restore_from_payload.
CPP_SNAPSHOT_RESTORED = '"msg":"restored snapshot"'

#: The number of committed entries this node's C++ engine has applied since the
#: process started, read from its Prometheus endpoint.
#:
#: This used to count log markers, and could not continue to. Phase 5 moved the
#: successful-apply line to DEBUG, which the containers do not emit, so the
#: marker count silently became zero — and the assertion it feeds
#: ("did this node replay its whole history?") passes trivially when the count is
#: always zero. A test that cannot fail is worse than no test.
#:
#: The counter is better than the log line ever was: it is incremented on the
#: apply path itself rather than beside it, it cannot be turned off by a log
#: level, and because it resets with the process, the value read AFTER a restart
#: is exactly "entries applied since this node came back" — which is the quantity
#: the scenario is actually about.
CPP_APPLY_COUNTER = "raftkv_apply_total"


def read_apply_counter(base_url: str, timeout: float = 5.0) -> int:
    """``raftkv_apply_total`` from a node's C++ /metrics endpoint.

    Returns 0 when the endpoint answers but does not carry the counter, which is
    what a node that has applied nothing since start looks like. A transport
    failure is raised, not swallowed: "the node is unreachable" and "the node has
    applied nothing" are opposite conclusions.
    """
    response = requests.get(f"{base_url}/metrics", timeout=timeout)
    response.raise_for_status()

    for line in response.text.splitlines():
        if line.startswith("#"):
            continue
        name, _, value = line.partition(" ")
        if name == CPP_APPLY_COUNTER:
            return int(float(value))
    return 0


# --------------------------------------------------------------------------
# Polling
# --------------------------------------------------------------------------


def poll(predicate, timeout: float, interval: float):
    """Poll ``predicate`` until it returns something truthy or ``timeout`` elapses.

    Returns the last value the predicate produced, so callers get both the "did
    it happen" answer and whatever it computed.

    Deliberately a local copy of ``conftest.wait_until`` rather than an import:
    the suite's rule is that modules never import ``conftest`` by name, because
    that breaks the moment a second conftest exists in the tree, and
    ``wait_until`` is not reachable through a fixture.

    Nothing in the snapshot scenarios sleeps anywhere else. Raft snapshots on
    its own randomised schedule, so every wait is a bounded poll for a
    condition, never a fixed sleep sized by guesswork.
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
# docker compose, across several -f files
# --------------------------------------------------------------------------


class ComposeProject(Compose):
    """``Compose`` bound to a list of compose files instead of exactly one.

    Only ``run`` is overridden; ``services``, ``container_id``,
    ``published_ports`` and ``host_data_dir`` all go through it and are
    inherited unchanged.
    """

    def __init__(self, files) -> None:
        self.files = [str(path) for path in files]
        # Keeps Compose.file meaningful (the first -f file is also what compose
        # derives the project name from) even though `run` no longer reads it.
        super().__init__(Path(self.files[0]))

    def run(self, *args: str, timeout: float = DOCKER_TIMEOUT):
        flags: list[str] = []
        for path in self.files:
            flags.extend(("-f", path))
        return _docker("compose", *flags, *args, timeout=timeout)

    def __str__(self) -> str:
        return " ".join(f"-f {path}" for path in self.files)

    # -- shells -------------------------------------------------------------

    def exec_sh(self, service: str, script: str, timeout: float = DOCKER_TIMEOUT):
        """Run ``sh -c script`` inside the **running** container for ``service``.

        ``-T`` disables TTY allocation, which is what makes the output usable
        from a captured subprocess.
        """
        return self.run("exec", "-T", service, "sh", "-c", script, timeout=timeout)

    def run_sh(self, service: str, script: str, timeout: float = DOCKER_TIMEOUT):
        """Run ``sh -c script`` in a one-off container with ``service``'s mounts.

        The one operation ``exec`` cannot do: touch the data directory of a
        service that is **stopped**. The container is thrown away afterwards,
        the entrypoint is replaced so nothing starts, ``--no-deps`` keeps it
        from pulling the rest of the cluster up, and ``run`` does not publish
        ports unless asked, so this cannot collide with the stopped service's
        own port bindings.
        """
        return self.run(
            "run",
            "--rm",
            "-T",
            "--no-deps",
            "--entrypoint",
            "sh",
            service,
            "-c",
            script,
            timeout=timeout,
        )

    # -- logs ---------------------------------------------------------------

    def logs(self, service: str, timeout: float = DOCKER_TIMEOUT) -> str:
        """Everything ``service`` has printed, from the start of its container.

        Not filtered by time on purpose. ``docker compose logs --since`` would
        need the test host's clock to agree with the daemon's; counting a marker
        before and after an action and taking the difference needs no clock at
        all, and is what every log assertion in these scenarios does.
        """
        result = self.run("logs", "--no-color", service, timeout=timeout)
        return result.stdout + result.stderr

    def count_markers(self, service: str, *markers: str) -> int:
        """How many log lines of ``service`` contain any of ``markers``."""
        return count_markers(self.logs(service), *markers)


def count_markers(logs: str, *markers: str) -> int:
    """Lines in ``logs`` containing at least one of ``markers`` (counted once)."""
    return sum(1 for line in logs.splitlines() if any(m in line for m in markers))


def compose_files() -> list[Path]:
    """The compose files to drive, from ``RAFTKV_COMPOSE_FILES`` or the default."""
    raw = os.environ.get("RAFTKV_COMPOSE_FILES", "").strip()
    if raw:
        return [Path(part.strip()).expanduser() for part in raw.split(",") if part.strip()]
    return [REPO_ROOT / name for name in DEFAULT_COMPOSE_FILES]


def require_compose_project() -> ComposeProject:
    """A usable local compose project, or a skip explaining what is missing.

    The same guards as ``test_crash.py``'s ``compose`` fixture, over the
    multi-file set. Everything here *skips* rather than fails: a missing docker
    CLI says nothing about whether snapshots work, and a skip means "not
    verified here", never "verified".
    """
    if shutil.which("docker") is None:
        pytest.skip("no `docker` CLI on PATH; the snapshot tests cannot drive a cluster")

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

    files = compose_files()
    missing = [str(path) for path in files if not path.is_file()]
    if missing:
        pytest.skip(
            f"compose file(s) not found: {missing}. Set RAFTKV_COMPOSE_FILES to the "
            "comma-separated list the running cluster was started with."
        )

    project = ComposeProject(files)
    listed = project.run("ps", "--all", "--quiet")
    if listed.returncode != 0:
        pytest.skip(
            f"`docker compose {project} ps` failed -- {_describe(listed)}. "
            "Is the docker daemon running?"
        )
    if not listed.stdout.strip():
        pytest.skip(
            f"the compose project for {project} has no containers. Start it with:\n"
            "  docker compose -f docker-compose.yml -f docker-compose.test.yml up -d"
        )
    return project


# --------------------------------------------------------------------------
# /status
# --------------------------------------------------------------------------

#: Field names from statusResponse in go-sidecar/internal/management/server.go.
#: Listed rather than read loosely so that a rename over there fails here with
#: "the sidecar does not report ..." instead of silently reporting zeros.
STATUS_FIELDS = (
    "is_leader",
    "leader_addr",
    "first_log_index",
    "last_log_index",
    "applied_index",
    "commit_index",
    "last_snapshot_index",
)


class Status(NamedTuple):
    """One ``GET :6000/status`` response.

    ``first_log_index`` comes from the BoltDB log store and is the only field
    that proves *truncation*: ``last_snapshot_index`` moving says a snapshot was
    taken, which on its own is perfectly compatible with a log that never
    shrank.
    """

    is_leader: bool
    leader_addr: str
    first_log_index: int
    last_log_index: int
    applied_index: int
    commit_index: int
    last_snapshot_index: int
    log_store_error: str

    def __str__(self) -> str:
        base = (
            f"first_log_index={self.first_log_index} "
            f"last_log_index={self.last_log_index} "
            f"applied_index={self.applied_index} "
            f"last_snapshot_index={self.last_snapshot_index} "
            f"leader={self.leader_addr!r}"
        )
        if self.log_store_error:
            return f"{base} log_store_error={self.log_store_error!r}"
        return base


class StatusUnavailable(Exception):
    """``/status`` could not be read or did not parse. Never a test failure."""


def read_status(compose: ComposeProject, service: str) -> Status:
    """Read ``:6000/status`` from inside ``service``'s container.

    Raises :class:`StatusUnavailable` for anything that means "ask again" -- a
    container that is down, a sidecar still starting, a truncated body. Callers
    poll; nothing here decides on its own that a test has failed.
    """
    result = compose.exec_sh(
        service, f"curl -sf --max-time 2 http://localhost:{MGMT_PORT}/status"
    )
    if result.returncode != 0:
        raise StatusUnavailable(f"{service}: /status not answering -- {_describe(result)}")

    try:
        payload = json.loads(result.stdout)
    except ValueError as exc:
        raise StatusUnavailable(
            f"{service}: /status body is not JSON ({exc}): {result.stdout!r}"
        ) from exc
    if not isinstance(payload, dict):
        raise StatusUnavailable(f"{service}: /status body is not an object: {payload!r}")

    missing = [name for name in STATUS_FIELDS if name not in payload]
    if missing:
        raise StatusUnavailable(
            f"{service}: /status does not report {missing} -- the sidecar predates "
            "the Phase 3 index fields, or they were renamed in "
            f"go-sidecar/internal/management/server.go. Body: {payload!r}"
        )

    return Status(
        is_leader=bool(payload["is_leader"]),
        leader_addr=str(payload["leader_addr"]),
        first_log_index=int(payload["first_log_index"]),
        last_log_index=int(payload["last_log_index"]),
        applied_index=int(payload["applied_index"]),
        commit_index=int(payload["commit_index"]),
        last_snapshot_index=int(payload["last_snapshot_index"]),
        log_store_error=str(payload.get("log_store_error", "")),
    )


def try_status(compose: ComposeProject, service: str) -> Status | None:
    """``read_status`` folded into ``None`` so it can be used inside a poll."""
    try:
        return read_status(compose, service)
    except StatusUnavailable:
        return None


def require_status(compose: ComposeProject, service: str, timeout: float) -> Status:
    """Poll ``/status`` until it answers, or fail saying why it never did."""
    last = ["never probed"]

    def probe():
        try:
            return read_status(compose, service)
        except StatusUnavailable as exc:
            last[0] = str(exc)
            return None

    status = poll(probe, timeout=timeout, interval=DOCKER_POLL_INTERVAL)
    if status is None:
        pytest.fail(
            f"the management API of {service} never answered within {timeout:g}s. "
            f"Last attempt: {last[0]}",
            pytrace=False,
        )
    return status


# --------------------------------------------------------------------------
# The snapshot tunables the containers actually got
# --------------------------------------------------------------------------

#: main.go logs the whole Config once at startup, and Config.String() carries the
#: three snapshot tunables precisely so this is observable. Reading them out of
#: the running container's own log beats reading the compose file: it reports what
#: arrived, not what was intended.
#:
#: This pattern has been broken twice, and both breaks were invisible because the
#: fixture that uses it SKIPPED on a parse failure. Phase 5 replaced the prose
#: prefix "Starting sidecar with config: " with a JSON line
#: (`{"msg":"starting sidecar",...,"config":"Config{...}"}`), and Phase 6 appended
#: RaftTLS/MgmtTLS/MgmtAuth after TrailingLogs, breaking a `\}` anchor. So:
#:
#:   * anchor on `Config{` only — not on any surrounding prose, which is
#:     formatting and will change again;
#:   * terminate on `[,}]`, so a field appended after TrailingLogs is harmless.
#:
#: And the fixture now FAILS rather than skips when this does not match. See
#: test_snapshot.py::tunables.
CONFIG_LINE_RE = re.compile(
    r"Config\{[^}]*?"
    r"SnapshotInterval:\s*(?P<interval>[^,]+),\s*"
    r"SnapshotThreshold:\s*(?P<threshold>\d+),\s*"
    r"TrailingLogs:\s*(?P<trailing>\d+)\s*[,}]"
)


class SnapshotTunables(NamedTuple):
    interval: str
    threshold: int
    trailing_logs: int

    @property
    def is_aggressive(self) -> bool:
        """Low enough that a snapshot and a truncation happen inside a test run."""
        return (
            self.threshold <= MAX_TEST_SNAPSHOT_THRESHOLD
            and self.trailing_logs <= MAX_TEST_TRAILING_LOGS
        )

    def __str__(self) -> str:
        return (
            f"SnapshotInterval={self.interval} SnapshotThreshold={self.threshold} "
            f"TrailingLogs={self.trailing_logs}"
        )


def parse_tunables(logs: str) -> SnapshotTunables | None:
    """The tunables from the LAST startup line in ``logs``, or ``None``.

    Last, not first: a container that has been restarted has logged the line
    more than once, and only the most recent start describes the process that
    is running now.
    """
    matches = list(CONFIG_LINE_RE.finditer(logs))
    if not matches:
        return None
    last = matches[-1]
    return SnapshotTunables(
        interval=last.group("interval").strip(),
        threshold=int(last.group("threshold")),
        trailing_logs=int(last.group("trailing")),
    )


# --------------------------------------------------------------------------
# The data directory, from inside a container
# --------------------------------------------------------------------------


class SnapshotDirectory(NamedTuple):
    """What ``<data dir>/snapshots`` holds on one node."""

    #: Names of complete snapshots: a directory holding both meta.json and
    #: state.bin whose own name does not end in ``.tmp``.
    complete: tuple[str, ...]

    #: Every path ``find`` reported, kept for failure messages.
    listing: tuple[str, ...]

    def __str__(self) -> str:
        if not self.listing:
            return "(empty or absent)"
        return ", ".join(self.listing)


def parse_snapshot_listing(output: str) -> SnapshotDirectory:
    """Turn the ``find`` output of :func:`list_snapshots` into a result."""
    root = f"{CONTAINER_DATA_DIR}/{SNAPSHOT_DIR}/"
    paths = tuple(line.strip() for line in output.splitlines() if line.strip())

    have_state: set[str] = set()
    have_meta: set[str] = set()
    for path in paths:
        if not path.startswith(root):
            continue
        name, _, leaf = path[len(root) :].partition("/")
        if not name or name.endswith(SNAPSHOT_TMP_SUFFIX):
            continue
        if leaf == SNAPSHOT_STATE_FILE:
            have_state.add(name)
        elif leaf == SNAPSHOT_META_FILE:
            have_meta.add(name)

    return SnapshotDirectory(tuple(sorted(have_state & have_meta)), paths)


def list_snapshots(
    compose: ComposeProject, service: str, running: bool
) -> SnapshotDirectory:
    """List the snapshot store of ``service`` from inside a container.

    ``running`` picks ``exec`` (cheap, needs the service up) over ``run``
    (a throwaway container, the only option once the service is stopped).
    """
    script = (
        f"find {CONTAINER_DATA_DIR}/{SNAPSHOT_DIR} -mindepth 1 -maxdepth 2 -print "
        "2>/dev/null || true"
    )
    shell = compose.exec_sh if running else compose.run_sh
    return parse_snapshot_listing(shell(service, script).stdout)


def wipe_data_dir(compose: ComposeProject, service: str) -> tuple[bool, str]:
    """Delete everything in a **stopped** service's data directory.

    Returns ``(emptied, detail)``. The directory itself survives -- it is the
    bind-mount target, and removing it would break the mount on restart.

    The removal and the verification are one shell invocation, so the listing
    cannot come from a different container than the delete.
    """
    script = (
        f"rm -rf {CONTAINER_DATA_DIR}/* {CONTAINER_DATA_DIR}/.[!.]* "
        f"{CONTAINER_DATA_DIR}/..?* 2>/dev/null; "
        f"ls -A {CONTAINER_DATA_DIR}"
    )
    result = compose.run_sh(service, script)
    if result.returncode != 0:
        return False, _describe(result)
    leftovers = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if leftovers:
        return False, f"still present: {leftovers}"
    return True, "empty"
