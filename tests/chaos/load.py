"""Journaling load generator (R7.1).

N client threads issue PUT/GET/DELETE against the cluster while faults are being
injected, and every **acknowledged** operation is written to a JSONL journal. The
journal is the evidence the invariant checker reasons over afterwards.

Two design decisions make that reasoning sound, and both matter more than they
look:

**The keyspace is partitioned per client.** Client *i* owns
``chaos-<run>-c<i>-k<n>`` and no other client touches it. Without that, two
clients writing the same key concurrently means the journal records two
acknowledgements with no defined order between them — HTTP acknowledgement order
is not raft commit order — and "what should this key hold at the end?" has no
answer. With it, every key has exactly one writer, so its history is a total
order and the last acknowledged operation on it is unambiguous.

**A request whose outcome is unknown poisons its key.** A write that times out,
or returns 502, may have committed anyway: the entry can be replicated and
applied while the response is lost. Journaling it as acknowledged would invent a
guarantee; ignoring it entirely would be worse, because the key may now hold that
value and a later exact-value check would fail on correct behavior. So the client
stops writing that key and marks it INDETERMINATE. The checker then makes no
claim about its value — but still requires all three nodes to agree on it, which
is the part that actually catches a bug.
"""

from __future__ import annotations

import json
import random
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import requests

from cluster import Cluster, Node

#: Operations recorded in the journal.
OP_SET = "set"
OP_DELETE = "delete"
OP_INDETERMINATE = "indeterminate"
OP_RECLAIM = "reclaim"

#: How long a key stays indeterminate before a client tries to reclaim it.
#:
#: Derived, not guessed. A write whose outcome is unknown can still commit while
#: it is in flight, so a fresh write issued immediately afterwards might be
#: OVERTAKEN by it and the key would end up holding the older value — the reclaim
#: would restore determinism in the journal and not in the store. The window has
#: to outlast every retry the write path can perform: GrpcRaftClient::propose has
#: a 5s deadline and rpc.Server.Propose bounds raft.Apply at 4s, after which
#: nothing anywhere retries the entry. 15s is that, with margin for an election.
#:
#: This matters more than it sounds. Without reclaim, one lost response poisons a
#: key permanently, and after a single kill -9 nearly the whole keyspace is
#: unverifiable: the first run of this harness ended up checking exactly ONE key
#: against 79 indeterminate ones, and reported PASSED.
RECLAIM_AFTER_S = 15.0


class Journal:
    """Append-only JSONL record of acknowledged operations.

    Flushed on every record. The whole point is to survive the harness being
    killed mid-run — a journal that is still in a buffer when the process dies
    proves nothing.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self._lock = threading.Lock()
        self._handle = path.open("w", encoding="utf-8")
        self._count = 0

    def record(self, **fields) -> None:
        line = json.dumps(fields, separators=(",", ":"))
        with self._lock:
            self._handle.write(line + "\n")
            self._handle.flush()
            self._count += 1

    @property
    def count(self) -> int:
        with self._lock:
            return self._count

    def close(self) -> None:
        with self._lock:
            self._handle.close()


@dataclass
class LoadStats:
    """Counters, for the run summary. Not assertions — the checker does those."""

    acked_writes: int = 0
    acked_deletes: int = 0
    reads_ok: int = 0
    reads_missing: int = 0
    rejected: int = 0  # a definite refusal: 503 not-leader, 4xx
    unknown: int = 0  # timeout or 5xx: may or may not have committed
    indeterminate_keys: int = 0
    reclaimed: int = 0  # poisoned keys brought back under an exact-value check

    _lock: threading.Lock = field(default_factory=threading.Lock, repr=False)

    def bump(self, name: str, amount: int = 1) -> None:
        with self._lock:
            setattr(self, name, getattr(self, name) + amount)

    def as_dict(self) -> dict:
        with self._lock:
            return {
                k: v
                for k, v in vars(self).items()
                if not k.startswith("_")
            }


class LoadGenerator:
    """Runs ``clients`` threads until :meth:`stop` is called."""

    def __init__(
        self,
        cluster: Cluster,
        journal: Journal,
        run_id: str,
        clients: int = 8,
        keys_per_client: int = 50,
        delete_ratio: float = 0.15,
        read_ratio: float = 0.30,
        seed: int = 1,
        request_timeout: float = 5.0,
    ) -> None:
        self.cluster = cluster
        self.journal = journal
        self.run_id = run_id
        self.clients = clients
        self.keys_per_client = keys_per_client
        self.delete_ratio = delete_ratio
        self.read_ratio = read_ratio
        self.seed = seed
        self.request_timeout = request_timeout

        self.stats = LoadStats()
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []

        # Pause machinery, so the runner can take a consistent snapshot of the
        # journal mid-run. Without it the checker races the load it is checking:
        # the journal is read, THEN a client deletes one of those keys, THEN the
        # checker reads 404 and reports a lost write that never happened. That
        # false positive is not hypothetical — it is what the first run of this
        # harness produced, twice, while the post-load check was clean.
        self._pause = threading.Event()
        self._parked = 0
        self._parked_cv = threading.Condition()

        #: Keys whose value cannot currently be predicted, mapped to the earliest
        #: time a reclaim may be attempted. See the module docstring.
        self.indeterminate: dict[str, float] = {}
        self._indeterminate_lock = threading.Lock()

    # -- lifecycle ----------------------------------------------------------

    def start(self) -> None:
        for index in range(self.clients):
            thread = threading.Thread(
                target=self._client_loop, args=(index,), name=f"load-{index}", daemon=True
            )
            thread.start()
            self._threads.append(thread)

    def stop(self, timeout: float = 30.0) -> None:
        self._stop.set()
        # A paused generator would never notice the stop flag.
        self.resume()
        for thread in self._threads:
            thread.join(timeout=timeout)

    def pause(self, timeout: float = 30.0) -> bool:
        """Park every client between requests. True when all of them are parked.

        Waits for clients to be BETWEEN requests, not merely to have been told to
        stop. A client blocked inside a PUT can still have it acknowledged after
        the journal snapshot is taken, which is the exact race this exists to
        close.
        """
        self._pause.set()
        deadline = time.monotonic() + timeout
        with self._parked_cv:
            while self._parked < len(self._threads):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._parked_cv.wait(timeout=remaining)
        return True

    def resume(self) -> None:
        self._pause.clear()

    # -- the client loop ----------------------------------------------------

    def _client_loop(self, index: int) -> None:
        # Per-client RNG seeded from the run seed, so a run is reproducible and
        # two clients do not walk the same sequence.
        rng = random.Random(self.seed * 1000 + index)
        keys = [f"chaos-{self.run_id}-c{index}-k{n:04d}" for n in range(self.keys_per_client)]
        counter = 0

        while not self._stop.is_set():
            if self._pause.is_set():
                with self._parked_cv:
                    self._parked += 1
                    self._parked_cv.notify_all()
                while self._pause.is_set() and not self._stop.is_set():
                    time.sleep(0.02)
                with self._parked_cv:
                    self._parked -= 1
                continue

            key = rng.choice(keys)
            # A node at random, exercising Phase 4 forwarding: two writes in three
            # land on a follower and must be relayed, not refused.
            node = rng.choice(self.cluster.nodes)

            if self._reclaimable(key):
                counter += 1
                self._do_reclaim(node, key, f"r{index}-{counter}")
                time.sleep(0.01)
                continue

            roll = rng.random()

            if roll < self.read_ratio:
                self._do_read(node, key)
            elif roll < self.read_ratio + self.delete_ratio:
                self._do_delete(node, key)
            else:
                counter += 1
                self._do_write(node, key, f"v{index}-{counter}")

            # A small pause keeps this a steady load rather than a benchmark; the
            # point is to have traffic in flight when a node dies, not to
            # saturate the cluster.
            time.sleep(0.01)

    def _mark_indeterminate(self, key: str, reason: str) -> None:
        reclaim_at = time.monotonic() + RECLAIM_AFTER_S
        with self._indeterminate_lock:
            already = key in self.indeterminate
            # Push the window out even when already poisoned: a second unknown
            # outcome means another write may be in flight.
            self.indeterminate[key] = reclaim_at
        if already:
            return
        self.stats.bump("indeterminate_keys")
        self.journal.record(
            op=OP_INDETERMINATE, key=key, reason=reason, ts=time.time()
        )

    def _reclaimable(self, key: str) -> bool:
        """Poisoned, but long enough ago that no stale write can still commit."""
        with self._indeterminate_lock:
            reclaim_at = self.indeterminate.get(key)
        return reclaim_at is not None and time.monotonic() >= reclaim_at

    def _is_indeterminate(self, key: str) -> bool:
        with self._indeterminate_lock:
            return key in self.indeterminate

    def _do_reclaim(self, node: Node, key: str, value: str) -> None:
        """Write a known value to a poisoned key and, if acknowledged, trust it again.

        Sound only because the reclaim window has already elapsed: nothing can
        still be retrying the write that poisoned this key, so an acknowledged
        write now is genuinely the last one.
        """
        try:
            response = self.cluster.put(node, key, value, timeout=self.request_timeout)
        except requests.exceptions.RequestException as exc:
            self.stats.bump("unknown")
            self._mark_indeterminate(key, f"reclaim transport error: {exc}")
            return

        if response.status_code != 200:
            if response.status_code in (502, 500, 504):
                self.stats.bump("unknown")
                self._mark_indeterminate(key, f"reclaim HTTP {response.status_code}")
            else:
                self.stats.bump("rejected")
            return

        with self._indeterminate_lock:
            self.indeterminate.pop(key, None)
        self.stats.bump("reclaimed")
        self.journal.record(
            op=OP_RECLAIM, key=key, value=value, ts=time.time(), node=node.service
        )

    def _do_write(self, node: Node, key: str, value: str) -> None:
        if self._is_indeterminate(key):
            return
        try:
            response = self.cluster.put(node, key, value, timeout=self.request_timeout)
        except requests.exceptions.RequestException as exc:
            # No response at all. The entry may still be in the log.
            self.stats.bump("unknown")
            self._mark_indeterminate(key, f"write transport error: {exc}")
            return

        if response.status_code == 200:
            self.stats.bump("acked_writes")
            self.journal.record(
                op=OP_SET, key=key, value=value, ts=time.time(), node=node.service
            )
            return

        if response.status_code in (502, 500, 504):
            # 502 here is "the propose failed", which in this codebase covers both
            # "never committed" and "committed but the reply was lost". Cannot
            # distinguish from the client, so do not pretend to.
            self.stats.bump("unknown")
            self._mark_indeterminate(key, f"write HTTP {response.status_code}")
            return

        # 503 (no leader yet), 4xx: a definite refusal. The key is untouched.
        self.stats.bump("rejected")

    def _do_delete(self, node: Node, key: str) -> None:
        if self._is_indeterminate(key):
            return
        try:
            response = self.cluster.delete(node, key, timeout=self.request_timeout)
        except requests.exceptions.RequestException as exc:
            self.stats.bump("unknown")
            self._mark_indeterminate(key, f"delete transport error: {exc}")
            return

        if response.status_code == 200:
            self.stats.bump("acked_deletes")
            self.journal.record(
                op=OP_DELETE, key=key, ts=time.time(), node=node.service
            )
            return

        if response.status_code in (502, 500, 504):
            self.stats.bump("unknown")
            self._mark_indeterminate(key, f"delete HTTP {response.status_code}")
            return

        self.stats.bump("rejected")

    def _do_read(self, node: Node, key: str) -> None:
        """Reads are load, not evidence.

        A default read is served from the local store and may be stale, so a miss
        here means nothing and is counted rather than journaled. The checker does
        its own reads, linearizably, at the end.
        """
        try:
            response = self.cluster.get(node, key, timeout=self.request_timeout)
        except requests.exceptions.RequestException:
            self.stats.bump("unknown")
            return

        if response.status_code == 200:
            self.stats.bump("reads_ok")
        elif response.status_code == 404:
            self.stats.bump("reads_missing")
        else:
            self.stats.bump("rejected")


def replay_journal(path: Path) -> tuple[dict[str, str | None], set[str]]:
    """Fold a journal into ``{key: expected_value_or_None}`` plus indeterminate keys.

    ``None`` means "acknowledged as deleted, must read 404". A key in the returned
    set has no expected value at all — only the requirement that the nodes agree.

    Order is file order, which is acknowledgement order. That is sound only
    because each key has a single writer (see the module docstring): within one
    client, the request that was acknowledged second was issued second.
    """
    expected: dict[str, str | None] = {}
    indeterminate: set[str] = set()

    # Order matters and the state machine is small: a key is poisoned by an
    # INDETERMINATE record and only an explicit RECLAIM clears it. An ordinary SET
    # while poisoned is NOT enough — it could be overtaken by the very write whose
    # outcome was unknown. The client only emits RECLAIM once that is impossible.
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: malformed journal line: {exc}")

            op = record.get("op")
            key = record.get("key")
            if not key:
                continue

            if op == OP_INDETERMINATE:
                indeterminate.add(key)
                expected.pop(key, None)
            elif op == OP_RECLAIM:
                indeterminate.discard(key)
                expected[key] = record.get("value")
            elif key in indeterminate:
                continue  # poisoned; a plain SET/DELETE does not restore trust
            elif op == OP_SET:
                expected[key] = record.get("value")
            elif op == OP_DELETE:
                expected[key] = None

    return expected, indeterminate
