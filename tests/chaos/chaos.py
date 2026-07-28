#!/usr/bin/env python3
"""Chaos runner: fault injection under load, with invariant checks (R7.2/R7.3).

    python tests/chaos/chaos.py --duration 600
    python tests/chaos/chaos.py --scenario kill-leader --duration 120 --seed 7

Needs a cluster already up (``docker compose up -d``) and drives docker directly
against it — killing, pausing and wiping nodes while the load generator keeps
writing. That is the opposite of the e2e suite's rule and entirely deliberate;
this harness exists to break things.

**What this is not.** Docker-level fault injection is not Jepsen. There is no
clock skew, no packet-level partitioning, no formal linearizability checking of
the full history — the checker verifies that acknowledged writes survive and that
replicas converge, which is weaker than linearizability and is stated as such
rather than implied. A clean run is evidence, not proof.

Exit status is 0 only when every scenario ran and no invariant was violated.
"""

from __future__ import annotations

import argparse
import random
import signal
import sys
import time
from dataclasses import dataclass
from pathlib import Path

# Run as a script from anywhere: `python tests/chaos/chaos.py`.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from cluster import Cluster, Compose, Node, describe, discover_nodes  # noqa: E402
from invariants import check_all, check_key_counts  # noqa: E402
from load import Journal, LoadGenerator, replay_journal  # noqa: E402
from selftest import SelfTestFailure  # noqa: E402
from selftest import run as run_selftest  # noqa: E402

#: How long to let the cluster settle and become writable again after a fault.
#: Generous, because it has to cover a container start plus a raft election.
RECOVERY_TIMEOUT = 120.0

#: How long the checker will wait for the cluster to satisfy an invariant before
#: calling it a violation. Also generous: replication lag is not data loss, and a
#: checker that cannot tell them apart is useless.
CHECK_TIMEOUT = 90.0


@dataclass
class ScenarioResult:
    name: str
    target: str
    ok: bool
    detail: str


class Chaos:
    def __init__(
        self,
        cluster: Cluster,
        compose: Compose,
        rng: random.Random,
        verbose: bool = True,
    ) -> None:
        self.cluster = cluster
        self.compose = compose
        self.rng = rng
        self.verbose = verbose

    def log(self, message: str) -> None:
        if self.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)

    # -- primitives ---------------------------------------------------------

    def _kill(self, node: Node) -> str | None:
        """SIGKILL the container. Not `stop` — that is a graceful shutdown, which
        Phase 5 made work properly and which therefore tests something else."""
        result = self.compose.run("kill", "-s", "SIGKILL", node.service)
        return None if result.returncode == 0 else describe(result)

    def _start(self, node: Node) -> str | None:
        result = self.compose.run("start", node.service)
        return None if result.returncode == 0 else describe(result)

    def _pause(self, node: Node) -> str | None:
        result = self.compose.run("pause", node.service)
        return None if result.returncode == 0 else describe(result)

    def _unpause(self, node: Node) -> str | None:
        result = self.compose.run("unpause", node.service)
        return None if result.returncode == 0 else describe(result)

    def _recover(self, probe_key: str) -> str | None:
        return self.cluster.wait_until_writable(probe_key, RECOVERY_TIMEOUT)

    # -- scenarios ----------------------------------------------------------

    def kill_leader(self, probe_key: str) -> ScenarioResult:
        leader = self.cluster.wait_for_leader(timeout=60.0)
        if leader is None:
            return ScenarioResult("kill-leader", "?", False, "no leader to kill")

        self.log(f"kill -9 the LEADER ({leader})")
        failure = self._kill(leader)
        if failure:
            return ScenarioResult("kill-leader", leader.service, False, failure)

        # Let the cluster run a node down for a moment, so the load generator is
        # actually writing during the election rather than after it.
        time.sleep(self.rng.uniform(2.0, 6.0))

        failure = self._start(leader)
        if failure:
            return ScenarioResult("kill-leader", leader.service, False, failure)

        failure = self._recover(probe_key)
        if failure:
            return ScenarioResult(
                "kill-leader", leader.service, False, f"never became writable: {failure}"
            )
        return ScenarioResult("kill-leader", leader.service, True, "recovered")

    def kill_follower(self, probe_key: str) -> ScenarioResult:
        followers = self.cluster.followers()
        if not followers:
            return ScenarioResult("kill-follower", "?", False, "no follower found")
        victim = self.rng.choice(followers)

        self.log(f"kill -9 a follower ({victim})")
        failure = self._kill(victim)
        if failure:
            return ScenarioResult("kill-follower", victim.service, False, failure)

        time.sleep(self.rng.uniform(2.0, 6.0))

        failure = self._start(victim)
        if failure:
            return ScenarioResult("kill-follower", victim.service, False, failure)

        failure = self._recover(probe_key)
        if failure:
            return ScenarioResult(
                "kill-follower", victim.service, False, f"never became writable: {failure}"
            )
        return ScenarioResult("kill-follower", victim.service, True, "recovered")

    def pause_node(self, probe_key: str) -> ScenarioResult:
        """SIGSTOP the whole container: a frozen node, not a dead one.

        Distinct from a kill in the way that matters to raft. A killed node's TCP
        connections are refused immediately; a paused one accepts nothing and
        refuses nothing, so its peers wait on it until they time out. That is what
        a GC pause or a starved VM looks like, and it is the case where a
        heartbeat-driven protocol is most likely to misbehave.
        """
        candidates = self.cluster.followers() or self.cluster.nodes
        victim = self.rng.choice(candidates)

        self.log(f"pause (freeze) {victim}")
        failure = self._pause(victim)
        if failure:
            return ScenarioResult("pause-node", victim.service, False, failure)

        frozen = self.rng.uniform(4.0, 10.0)
        time.sleep(frozen)

        failure = self._unpause(victim)
        if failure:
            # A container left paused would poison every later scenario, so this
            # is worth shouting about rather than just recording.
            self.log(f"!! FAILED TO UNPAUSE {victim}: {failure}")
            return ScenarioResult("pause-node", victim.service, False, failure)

        failure = self._recover(probe_key)
        if failure:
            return ScenarioResult(
                "pause-node", victim.service, False, f"never became writable: {failure}"
            )
        return ScenarioResult(
            "pause-node", victim.service, True, f"frozen for {frozen:.1f}s, recovered"
        )

    def wipe_follower(self, probe_key: str) -> ScenarioResult:
        """Delete a follower's whole data directory and bring it back empty.

        The Phase 3 snapshot path under load: the node has no log and no store, so
        the leader must ship it a snapshot. Doing it while writes are in flight is
        the part the e2e version does not cover.
        """
        followers = self.cluster.followers()
        if not followers:
            return ScenarioResult("wipe-follower", "?", False, "no follower found")
        victim = self.rng.choice(followers)

        self.log(f"wipe {victim}'s data directory and restart it empty")
        stopped = self.compose.run("stop", victim.service)
        if stopped.returncode != 0:
            return ScenarioResult(
                "wipe-follower", victim.service, False, describe(stopped)
            )

        # `run --rm --no-deps` on the stopped service: same image, same bind
        # mount, so it deletes the real directory contents. The directory itself
        # must survive — it is the mount target.
        wiped = self.compose.run(
            "run", "--rm", "--no-deps", "--entrypoint", "sh", victim.service,
            "-c", "rm -rf /app/data/* /app/data/.[!.]* 2>/dev/null; ls -A /app/data",
        )
        if wiped.returncode != 0:
            self.compose.run("start", victim.service)
            return ScenarioResult(
                "wipe-follower", victim.service, False, f"wipe failed: {describe(wiped)}"
            )
        leftovers = [line for line in wiped.stdout.split() if line.strip()]
        if leftovers:
            self.compose.run("start", victim.service)
            return ScenarioResult(
                "wipe-follower", victim.service, False,
                f"data directory not empty after wipe: {leftovers}",
            )

        failure = self._start(victim)
        if failure:
            return ScenarioResult("wipe-follower", victim.service, False, failure)

        failure = self._recover(probe_key)
        if failure:
            return ScenarioResult(
                "wipe-follower", victim.service, False,
                f"never became writable after a wipe: {failure}",
            )
        return ScenarioResult("wipe-follower", victim.service, True, "rejoined empty")


SCENARIOS = {
    "kill-leader": Chaos.kill_leader,
    "kill-follower": Chaos.kill_follower,
    "pause-node": Chaos.pause_node,
    "wipe-follower": Chaos.wipe_follower,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--duration", type=float, default=600.0,
                        help="total seconds of chaos (default: 600)")
    parser.add_argument("--scenario", action="append", choices=sorted(SCENARIOS),
                        help="run only this scenario; repeatable (default: all, cycled)")
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--keys-per-client", type=int, default=50)
    parser.add_argument("--seed", type=int, default=1,
                        help="RNG seed; the same seed replays the same fault sequence")
    parser.add_argument("--journal", type=Path, default=Path("chaos-journal.jsonl"))
    parser.add_argument("--settle", type=float, default=8.0,
                        help="seconds of clean load before the first fault")
    parser.add_argument("--skip-selftest", action="store_true",
                        help="do not verify the checker can fail before running "
                             "(you almost never want this)")
    args = parser.parse_args()

    compose = Compose()
    if not compose.available():
        print("docker is not available; the chaos harness drives containers directly",
              file=sys.stderr)
        return 2

    cluster = Cluster(discover_nodes())
    run_id = f"{args.seed:03d}-{int(time.time()) % 100000}"

    leader = cluster.wait_for_leader(timeout=60.0)
    if leader is None:
        print("no leader; is the cluster up? (docker compose up -d)", file=sys.stderr)
        return 2
    print(f"cluster is up, leader={leader}, run_id={run_id}", flush=True)

    # Prove the checker can fail BEFORE trusting it to say the cluster is fine.
    # A broken checker and a healthy cluster look identical from the outside, and
    # this repository has already shipped two assertions that could not fail.
    if args.skip_selftest:
        print("WARNING: checker self-test skipped; a PASSED result means little",
              flush=True)
    else:
        try:
            controls = run_selftest(cluster)
        except SelfTestFailure as exc:
            print(f"checker self-test FAILED: {exc}", file=sys.stderr)
            print("Refusing to run: a chaos run with a broken checker reports "
                  "PASSED no matter what happens.", file=sys.stderr)
            return 2
        print(f"checker self-test ok ({', '.join(controls)})", flush=True)

    journal = Journal(args.journal)
    generator = LoadGenerator(
        cluster, journal, run_id,
        clients=args.clients,
        keys_per_client=args.keys_per_client,
        seed=args.seed,
    )
    rng = random.Random(args.seed)
    chaos = Chaos(cluster, compose, rng)

    # A SIGINT must still produce a report. An interrupted chaos run whose journal
    # is never checked is a run that told you nothing.
    interrupted = {"flag": False}

    def handle_signal(signum, frame):  # noqa: ARG001
        interrupted["flag"] = True
        chaos.log(f"received signal {signum}; finishing the current scenario")

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    order = args.scenario or sorted(SCENARIOS)
    results: list[ScenarioResult] = []

    generator.start()
    chaos.log(f"load started: {args.clients} clients, settling for {args.settle:g}s")
    time.sleep(args.settle)

    deadline = time.monotonic() + args.duration
    index = 0
    try:
        while time.monotonic() < deadline and not interrupted["flag"]:
            name = order[index % len(order)]
            index += 1

            probe_key = f"chaos-probe-{run_id}-{index}"
            result = SCENARIOS[name](chaos, probe_key)
            results.append(result)
            chaos.log(f"  {name} [{result.target}]: "
                      f"{'ok' if result.ok else 'FAILED'} — {result.detail}")

            if not result.ok:
                # Keep going. A scenario that could not be set up is not the same
                # as a broken invariant, and stopping here would skip the check
                # that tells them apart.
                continue

            # Check after every scenario, not only at the end: a violation that
            # heals before the run finishes is still a violation, and the
            # scenario that caused it is the useful part of the report.
            #
            # The load is PARKED first. Reading a journal while clients are still
            # writing means the checker can expect a value that a client deleted
            # a millisecond later, and report a lost write that never happened —
            # which is what the first version of this harness did.
            if not generator.pause(timeout=30.0):
                chaos.log("  WARNING: load did not park in time; skipping the "
                          "mid-run check rather than risking a false violation")
                generator.resume()
                continue
            try:
                expected, indeterminate = replay_journal(args.journal)
                check = check_all(cluster, expected, indeterminate, timeout=CHECK_TIMEOUT)
            finally:
                generator.resume()
            chaos.log(f"  invariants after {name}: {check.summary()}")
            for violation in check.violations:
                chaos.log(f"    {violation}")
            if not check.ok:
                results.append(
                    ScenarioResult(f"{name}:invariants", result.target, False,
                                   "; ".join(str(v) for v in check.violations[:5]))
                )

            time.sleep(rng.uniform(2.0, 5.0))
    finally:
        chaos.log("stopping load")
        generator.stop()
        journal.close()

    # Final check, after the load has stopped and the cluster is quiet. This is
    # the strictest one: nothing is in flight, so there is no excuse left.
    chaos.log("final invariant check (load stopped)")
    expected, indeterminate = replay_journal(args.journal)
    final = check_all(cluster, expected, indeterminate, timeout=CHECK_TIMEOUT)

    print()
    print("=" * 72)
    print(f"run_id            {run_id}   seed {args.seed}")
    print(f"journal           {args.journal} ({journal.count} records)")
    print(f"load              {generator.stats.as_dict()}")
    print(f"store key counts  {check_key_counts(cluster)}")
    print(f"scenarios         {len(results)} run")
    for result in results:
        print(f"  {'ok  ' if result.ok else 'FAIL'} {result.name} [{result.target}] "
              f"{result.detail}")
    print(f"final invariants  {final.summary()}")
    for violation in final.violations:
        print(f"  {violation}")
    print("=" * 72)

    failed_scenarios = [r for r in results if not r.ok]
    if not final.ok:
        print("\nFAILED: an acknowledged write was lost or the replicas diverged.")
        return 1
    if failed_scenarios:
        print(f"\nFAILED: {len(failed_scenarios)} scenario(s) could not run or "
              "violated an invariant.")
        return 1
    if interrupted["flag"]:
        print("\nInterrupted, but every invariant checked so far held.")
        return 0

    print("\nPASSED: every acknowledged write survived and all replicas agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
