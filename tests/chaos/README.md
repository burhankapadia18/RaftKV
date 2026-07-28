# Chaos harness (Phase 7)

Fault injection under continuous load, with invariant checking. Answers one
question: **does an acknowledged write survive whatever happens next?**

```bash
docker build -t raftkv:latest .
docker compose up -d
pip install -r tests/e2e/requirements.txt      # same deps: requests, pytest

python tests/chaos/chaos.py --duration 600
```

Exit status is 0 only if every scenario ran and no invariant was violated.

## What it is not

Docker-level fault injection is not Jepsen, and this file will not pretend
otherwise. There is no clock skew, no packet-level partitioning, no formal
linearizability checking of the operation history. The two properties checked —
acknowledged writes survive, and replicas converge — are strictly weaker than
linearizability. A clean run is evidence, not proof.

What it does cover is the failure modes this system is actually built around:
processes dying mid-write, a frozen node that neither answers nor refuses, and a
replica losing its entire disk while the cluster keeps taking traffic.

## Scenarios (R7.2)

| Scenario | What it does | Why it is distinct |
|---|---|---|
| `kill-leader` | `kill -9` the current leader, restart it | Forces an election with writes in flight |
| `kill-follower` | `kill -9` a random follower, restart it | Log catch-up under load; quorum survives |
| `pause-node` | `docker pause` for 4–10s, then unpause | A frozen node neither answers nor refuses, so peers block until they time out — a GC pause or a starved VM, and where a heartbeat protocol is most likely to misbehave |
| `wipe-follower` | Stop, delete the whole data directory, start | Phase 3 snapshot transfer under load: the node has no log and no store |

Faults are cycled in order and repeated until `--duration` runs out. `--seed`
makes the sequence and the load reproducible.

## The two invariants (R7.3)

**Durability of acknowledgement.** Every key whose last acknowledged operation
was a SET must read back that value; every key whose last acknowledged DELETE
must read 404. Checked with `?consistency=linearizable` — a stale local read
could mask a lost write, which is the one thing this harness exists not to do.

**Convergence.** Every node must serve the same value for every key touched,
read from each node's own store (not linearizably — a linearizable read is
answered by the leader whichever node receives it, so it cannot see a follower
that disagrees). This check includes keys whose value is unknown: the harness
cannot say what such a key should hold, but three replicas of one state machine
must not disagree about it.

Both are polled to a deadline. Immediately after a fault the cluster is
legitimately still catching up, and a checker that reported the first
disagreement it saw would be reporting replication lag as data loss.

## Why the journal is trustworthy

Two rules, both load-bearing:

**One writer per key.** Client *i* owns `chaos-<run>-c<i>-k<n>`. Without that,
two clients writing the same key concurrently produce two acknowledgements with
no order between them — HTTP acknowledgement order is not raft commit order — and
"what should this key hold?" has no answer.

**An unknown outcome poisons its key.** A write that times out or returns 502 may
have committed anyway; the entry can be replicated and applied while the response
is lost. Recording it as acknowledged would invent a guarantee. So the key is
marked indeterminate: no claim is made about its value, but the nodes must still
agree on it.

**Poisoned keys are reclaimed, and that is what makes the run mean anything.**
After 15 seconds — longer than every retry the write path can perform, since
`GrpcRaftClient::propose` has a 5s deadline and `rpc.Server.Propose` bounds
`raft.Apply` at 4s — nothing can still be retrying the lost write, so a fresh
acknowledged write to that key is genuinely the last one and the key returns to
the exact-value check. Without this, one `kill -9` poisons nearly the whole
keyspace: the first run of this harness ended up checking **one** key against 79
indeterminate ones, and printed PASSED.

## The checker proves it can fail, every run

Before any claim about the cluster, the harness lies to its own checker four
times (a key that was never written; a key holding a different value; a key
holding the correct value; a key claimed as deleted that is not) and requires
three objections and one silence. It refuses to run if the checker gets any of
them wrong.

That is not defensive decoration. A broken checker and a healthy cluster produce
identical output, and this repository has already shipped two assertions that
could not fail — see the Phase 3 snapshot scenarios, which were vacuous for two
phases while CI stayed green. `--skip-selftest` exists and you almost never want
it.

## Files

| File | Role |
|---|---|
| `chaos.py` | CLI, scenario loop, reporting |
| `load.py` | Journaling load generator; the poisoning and reclaim rules |
| `invariants.py` | The two checks |
| `selftest.py` | Negative controls for the checker |
| `cluster.py` | HTTP client and compose control |
| `test_journal.py` | Unit tests for the journal fold — no cluster needed |

```bash
pytest tests/chaos/test_journal.py -v
```

Not collected by `pytest tests/e2e` (pytest.ini pins `testpaths`); it tests the
harness, not the product.

## CI

`.github/workflows/chaos.yml` runs a 10-minute pass nightly, not per-PR (R7.4) —
it takes too long for a pull request and its value is in repetition. The journal
and container logs are uploaded on failure, which is the only way to reconstruct
what happened after the fact.
