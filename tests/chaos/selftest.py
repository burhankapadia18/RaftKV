"""Negative controls for the invariant checker.

Run before every chaos run, because the failure mode this guards against has
already happened twice in this repository: a check that cannot fail reports
PASSED and teaches you nothing. Two of the Phase 3 snapshot assertions were
silently vacuous for two phases for exactly that reason.

A chaos harness is worse than most, because a broken checker and a healthy
cluster produce identical output. So before making any claim about the cluster,
the harness lies to its own checker four times and requires it to object three
times and stay quiet once.

Costs about a second and needs only a live cluster.
"""

from __future__ import annotations

import time

import requests

from cluster import Cluster
from invariants import check_all

#: Short: these controls are about whether a disagreement is DETECTED, not about
#: giving a healthy cluster time to converge.
CONTROL_TIMEOUT = 6.0


class SelfTestFailure(RuntimeError):
    pass


def run(cluster: Cluster) -> list[str]:
    """Returns a list of human-readable control results, or raises."""
    leader = cluster.wait_for_leader(timeout=60.0)
    if leader is None:
        raise SelfTestFailure("no leader; cannot self-test the checker")

    key = f"chaos-selftest-{int(time.time())}"
    results = []

    # 1. A value that was never written must be reported as lost.
    absent = check_all(cluster, {f"{key}-absent": "never-written"}, set(),
                       timeout=CONTROL_TIMEOUT)
    if absent.ok:
        raise SelfTestFailure(
            "the checker accepted a value that was never written; it would report "
            "PASSED for a cluster that lost every write"
        )
    results.append("missing key detected")

    # 2. Establish a real value, then claim it holds something else.
    response = cluster.put(leader, key, "actual", timeout=10.0)
    if response.status_code != 200:
        raise SelfTestFailure(
            f"could not write the control key: HTTP {response.status_code}"
        )
    # A linearizable read goes through the leader, so no sleep is needed for
    # correctness — but the write has to have been applied locally first, and the
    # barrier guarantees exactly that.

    mismatch = check_all(cluster, {key: "wrong-value"}, set(), timeout=CONTROL_TIMEOUT)
    if mismatch.ok:
        raise SelfTestFailure(
            "the checker accepted a value that does not match what the cluster holds"
        )
    results.append("wrong value detected")

    # 3. The truth must pass. Without this the other three would be satisfied by
    #    a checker that simply always fails.
    correct = check_all(cluster, {key: "actual"}, set(), timeout=CONTROL_TIMEOUT)
    if not correct.ok:
        raise SelfTestFailure(
            "the checker rejected a value the cluster genuinely holds: "
            + "; ".join(str(v) for v in correct.violations)
        )
    results.append("correct value accepted")

    # 4. An acknowledged DELETE that did not happen must be reported.
    undeleted = check_all(cluster, {key: None}, set(), timeout=CONTROL_TIMEOUT)
    if undeleted.ok:
        raise SelfTestFailure(
            "the checker accepted a key as deleted while the cluster still serves it"
        )
    results.append("undeleted key detected")

    try:
        cluster.delete(leader, key, timeout=10.0)
    except requests.exceptions.RequestException:
        pass  # Tidiness only; the control key is namespaced and harmless.

    return results
