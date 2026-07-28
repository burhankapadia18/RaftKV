"""Invariant checker (R7.3).

Two properties, checked after every scenario and again at the end:

**Durability of acknowledgement.** Every key whose last acknowledged operation
was a SET must read back that value; every key whose last acknowledged operation
was a DELETE must read 404. Checked with ``?consistency=linearizable``, so the
read goes through the leader's barrier and quorum check instead of whatever a
local replica happens to hold — a stale local read could mask a lost write, and
masking lost writes is the one thing this harness exists not to do.

**Convergence.** Every node must serve the same value for every key the run
touched, including keys whose value is indeterminate. This is the check that
catches divergence, and it is why indeterminate keys are read rather than
skipped: the harness cannot say what such a key should hold, but it can say that
three replicas of the same state machine must not disagree about it.

Both are polled to a deadline rather than sampled once. Immediately after a
scenario the cluster is legitimately still catching up, and a checker that
reported the first disagreement it saw would report replication lag as data loss.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field

import requests

from cluster import Cluster, Node


@dataclass
class Violation:
    kind: str
    key: str
    detail: str

    def __str__(self) -> str:
        return f"[{self.kind}] {self.key}: {self.detail}"


@dataclass
class CheckResult:
    violations: list[Violation] = field(default_factory=list)
    keys_checked: int = 0
    indeterminate_checked: int = 0
    duration_s: float = 0.0

    @property
    def ok(self) -> bool:
        return not self.violations

    def summary(self) -> str:
        state = "OK" if self.ok else f"{len(self.violations)} VIOLATION(S)"
        return (
            f"{state} — {self.keys_checked} acknowledged key(s) + "
            f"{self.indeterminate_checked} indeterminate, in {self.duration_s:.1f}s"
        )


def _read_linearizable(cluster: Cluster, node: Node, key: str) -> tuple[int, str] | None:
    try:
        response = cluster.get(node, key, linearizable=True, timeout=10.0)
    except requests.exceptions.RequestException:
        return None
    return response.status_code, response.text


def _read_local(cluster: Cluster, node: Node, key: str) -> tuple[int, str] | None:
    try:
        response = cluster.get(node, key, timeout=10.0)
    except requests.exceptions.RequestException:
        return None
    return response.status_code, response.text


def check_acknowledged_writes(
    cluster: Cluster,
    expected: dict[str, str | None],
    timeout: float = 60.0,
) -> list[Violation]:
    """Every acknowledged write readable, every acknowledged delete gone."""
    outstanding = dict(expected)
    deadline = time.monotonic() + timeout
    last_seen: dict[str, str] = {}

    while outstanding and time.monotonic() < deadline:
        leader = cluster.wait_for_leader(timeout=15.0)
        if leader is None:
            time.sleep(0.5)
            continue

        for key in list(outstanding):
            want = outstanding[key]
            observed = _read_linearizable(cluster, leader, key)
            if observed is None:
                last_seen[key] = "unreachable"
                continue
            status, body = observed

            if want is None:
                if status == 404:
                    del outstanding[key]
                else:
                    last_seen[key] = f"HTTP {status} {body[:80]!r}"
            else:
                if status == 200 and body == want:
                    del outstanding[key]
                else:
                    last_seen[key] = f"HTTP {status} {body[:80]!r}"

        if outstanding:
            time.sleep(0.5)

    violations = []
    for key, want in outstanding.items():
        wanted = "absent (acknowledged DELETE)" if want is None else repr(want)
        violations.append(
            Violation(
                kind="lost-acknowledged-write",
                key=key,
                detail=(
                    f"want {wanted}, linearizable read gave "
                    f"{last_seen.get(key, 'nothing')} after {timeout:g}s"
                ),
            )
        )
    return violations


def check_convergence(
    cluster: Cluster,
    keys: list[str],
    timeout: float = 60.0,
) -> list[Violation]:
    """All nodes serve identical state for every key.

    Local reads on purpose: a linearizable read is answered by the leader
    whichever node receives it, so it cannot detect a follower whose own store
    disagrees. The whole question here is what each replica actually holds.

    A node that is unreachable is skipped rather than treated as a disagreement —
    a stopped container is not a divergent one, and the scenarios stop containers.
    """
    disputed = list(keys)
    deadline = time.monotonic() + timeout
    last_seen: dict[str, dict[str, str]] = {}

    while disputed and time.monotonic() < deadline:
        still_disputed = []
        for key in disputed:
            per_node: dict[str, str] = {}
            for node in cluster.nodes:
                observed = _read_local(cluster, node, key)
                if observed is None:
                    continue
                status, body = observed
                per_node[node.service] = "<absent>" if status == 404 else body

            last_seen[key] = per_node
            if len(set(per_node.values())) > 1:
                still_disputed.append(key)

        disputed = still_disputed
        if disputed:
            time.sleep(0.5)

    return [
        Violation(
            kind="divergence",
            key=key,
            detail=(
                "nodes disagree after "
                f"{timeout:g}s of convergence time: {last_seen.get(key)}"
            ),
        )
        for key in disputed
    ]


def check_all(
    cluster: Cluster,
    expected: dict[str, str | None],
    indeterminate: set[str],
    timeout: float = 60.0,
) -> CheckResult:
    started = time.monotonic()

    violations = check_acknowledged_writes(cluster, expected, timeout=timeout)

    all_keys = sorted(set(expected) | indeterminate)
    violations += check_convergence(cluster, all_keys, timeout=timeout)

    return CheckResult(
        violations=violations,
        keys_checked=len(expected),
        indeterminate_checked=len(indeterminate),
        duration_s=time.monotonic() - started,
    )


def check_key_counts(cluster: Cluster) -> str:
    """A one-line report of each node's raftkv_store_keys gauge.

    Informational, not an assertion. The nodes are expected to agree, but the
    gauge counts the whole store — including keys written by earlier test runs
    against the same volumes — so a mismatch here is a hint to investigate, not
    a violation. The per-key comparison above is the real check.
    """
    counts = {node.service: cluster.store_keys(node) for node in cluster.nodes}
    return ", ".join(f"{service}={count}" for service, count in counts.items())
