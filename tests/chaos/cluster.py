"""Cluster access for the chaos harness: HTTP client and compose control.

Deliberately standalone rather than importing ``tests/e2e``. Those helpers are
pytest fixtures, and the chaos runner is a long-lived program with a CLI, a
signal handler and its own lifecycle. Sharing them would mean either dragging
pytest into a load generator or bending the fixtures into something neither user
wants. The overlap is about eighty lines.
"""

from __future__ import annotations

import json
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

import requests

#: Where the compose project lives. The chaos harness, unlike the e2e suite,
#: openly drives docker — that is its entire job.
REPO_ROOT = Path(__file__).resolve().parents[2]
COMPOSE_FILE = REPO_ROOT / "docker-compose.yml"

#: Host ports published by docker-compose.yml, service -> (http, mgmt).
DEFAULT_NODES: dict[str, tuple[int, int]] = {
    "node1": (8080, 6000),
    "node2": (8081, 6001),
    "node3": (8082, 6002),
}

DOCKER_TIMEOUT = 120.0
HTTP_TIMEOUT = 5.0


@dataclass(frozen=True)
class Node:
    service: str
    http: str
    mgmt: str

    def __str__(self) -> str:
        return self.service


def discover_nodes(nodes: dict[str, tuple[int, int]] | None = None) -> list[Node]:
    mapping = nodes or DEFAULT_NODES
    return [
        Node(service, f"http://localhost:{http}", f"http://localhost:{mgmt}")
        for service, (http, mgmt) in mapping.items()
    ]


class Cluster:
    """HTTP access to the cluster, with no assumptions about who leads.

    Since Phase 4 a write to any node is forwarded to the leader, so most of this
    does not need to know. The exception is fault injection, which specifically
    wants to kill the leader — hence :meth:`leader`.
    """

    def __init__(self, nodes: list[Node]) -> None:
        self.nodes = nodes
        # One session per cluster, not per request: the load generator issues
        # thousands of requests and a fresh TCP connection for each would measure
        # connection setup as much as the store. requests.Session is documented
        # as thread-safe for this usage pattern.
        self._session = requests.Session()

    # -- reads and writes ---------------------------------------------------

    def put(self, node: Node, key: str, value: str, timeout: float = HTTP_TIMEOUT):
        return self._session.put(
            f"{node.http}/kv/{key}", data=value.encode(), timeout=timeout
        )

    def delete(self, node: Node, key: str, timeout: float = HTTP_TIMEOUT):
        return self._session.delete(f"{node.http}/kv/{key}", timeout=timeout)

    def get(
        self,
        node: Node,
        key: str,
        linearizable: bool = False,
        timeout: float = HTTP_TIMEOUT,
    ):
        params = {"consistency": "linearizable"} if linearizable else None
        return self._session.get(f"{node.http}/kv/{key}", params=params, timeout=timeout)

    # -- cluster state ------------------------------------------------------

    def status(self, node: Node, timeout: float = HTTP_TIMEOUT) -> dict | None:
        try:
            response = self._session.get(f"{node.mgmt}/status", timeout=timeout)
        except requests.exceptions.RequestException:
            return None
        if response.status_code != 200:
            return None
        try:
            return response.json()
        except json.JSONDecodeError:
            return None

    def leader(self) -> Node | None:
        """The node that currently says it leads, or None during an election."""
        for node in self.nodes:
            status = self.status(node)
            if status and status.get("is_leader"):
                return node
        return None

    def followers(self) -> list[Node]:
        leader = self.leader()
        return [node for node in self.nodes if node is not leader]

    def wait_for_leader(self, timeout: float) -> Node | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            leader = self.leader()
            if leader is not None:
                return leader
            time.sleep(0.2)
        return None

    def wait_until_writable(self, probe_key: str, timeout: float) -> str | None:
        """Wait until EVERY node accepts a write. Returns None, or the last error.

        Every node, not just one: a scenario that killed node1 is not over when
        node2 starts answering. The C++ HTTP server also starts listening well
        before its sidecar binds the gRPC port, so "the port is open" is not the
        same question and would let the next scenario start against a node that
        still 502s every write.
        """
        deadline = time.monotonic() + timeout
        last = "no attempt completed"
        while time.monotonic() < deadline:
            pending = []
            for node in self.nodes:
                try:
                    response = self.put(node, probe_key, "probe")
                except requests.exceptions.RequestException as exc:
                    pending.append(f"{node}: {exc}")
                    continue
                if response.status_code != 200:
                    pending.append(f"{node}: HTTP {response.status_code}")
            if not pending:
                return None
            last = "; ".join(pending)
            time.sleep(0.5)
        return last

    def store_keys(self, node: Node) -> int | None:
        """``raftkv_store_keys`` from a node's own metrics endpoint."""
        try:
            response = self._session.get(f"{node.http}/metrics", timeout=HTTP_TIMEOUT)
        except requests.exceptions.RequestException:
            return None
        if response.status_code != 200:
            return None
        for line in response.text.splitlines():
            if line.startswith("raftkv_store_keys"):
                _, _, value = line.partition(" ")
                return int(float(value))
        return None


class Compose:
    """The subset of ``docker compose`` the fault injector needs."""

    def __init__(self, compose_file: Path = COMPOSE_FILE) -> None:
        self.file = str(compose_file)

    def run(self, *args: str, timeout: float = DOCKER_TIMEOUT):
        return subprocess.run(
            ["docker", "compose", "-f", self.file, *args],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )

    def available(self) -> bool:
        try:
            return (
                subprocess.run(
                    ["docker", "info"], capture_output=True, timeout=30, check=False
                ).returncode
                == 0
            )
        except (OSError, subprocess.SubprocessError):
            return False

    def container_id(self, service: str) -> str | None:
        result = self.run("ps", "--all", "--quiet", service)
        if result.returncode != 0:
            return None
        ids = result.stdout.split()
        return ids[0] if ids else None

    def is_running(self, service: str) -> bool:
        container = self.container_id(service)
        if not container:
            return False
        result = subprocess.run(
            ["docker", "inspect", "-f", "{{.State.Running}}", container],
            capture_output=True, text=True, timeout=30, check=False,
        )
        return result.stdout.strip() == "true"


def describe(result: subprocess.CompletedProcess) -> str:
    """A one-line summary of a failed command, for an error message."""
    parts = [f"exit {result.returncode}"]
    if result.stdout.strip():
        parts.append(f"stdout: {result.stdout.strip()[:300]}")
    if result.stderr.strip():
        parts.append(f"stderr: {result.stderr.strip()[:300]}")
    return "; ".join(parts)
