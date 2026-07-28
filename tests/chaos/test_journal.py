"""Unit tests for the journal fold — the harness's own logic, no cluster needed.

    pytest tests/chaos/test_journal.py -v

Worth testing separately because ``replay_journal`` decides what the checker
will assert, so a bug here does not produce a wrong answer — it produces a
checker that asserts nothing, which looks exactly like a passing run. The
poisoning rules in particular are subtle enough to get wrong quietly.

Not part of ``pytest tests/e2e`` (pytest.ini pins ``testpaths``), because it
belongs to the chaos harness rather than the product's e2e contract.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from load import OP_DELETE, OP_INDETERMINATE, OP_RECLAIM, OP_SET, replay_journal  # noqa: E402


def write_journal(tmp_path: Path, records: list[dict]) -> Path:
    path = tmp_path / "journal.jsonl"
    with path.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record) + "\n")
    return path


def test_last_write_wins(tmp_path):
    path = write_journal(tmp_path, [
        {"op": OP_SET, "key": "a", "value": "1"},
        {"op": OP_SET, "key": "a", "value": "2"},
        {"op": OP_SET, "key": "b", "value": "x"},
    ])

    expected, indeterminate = replay_journal(path)

    assert expected == {"a": "2", "b": "x"}
    assert indeterminate == set()


def test_delete_expects_absence(tmp_path):
    path = write_journal(tmp_path, [
        {"op": OP_SET, "key": "a", "value": "1"},
        {"op": OP_DELETE, "key": "a"},
    ])

    expected, _ = replay_journal(path)

    assert expected == {"a": None}, "an acknowledged DELETE must expect a 404, not a value"


def test_delete_then_write_expects_the_value(tmp_path):
    path = write_journal(tmp_path, [
        {"op": OP_DELETE, "key": "a"},
        {"op": OP_SET, "key": "a", "value": "back"},
    ])

    expected, _ = replay_journal(path)

    assert expected == {"a": "back"}


def test_indeterminate_removes_an_earlier_acknowledgement(tmp_path):
    """The key was acknowledged, then a later write's outcome was unknown.

    The store may now hold either value, so the checker must claim neither.
    Keeping the earlier expectation would fail on correct behavior.
    """
    path = write_journal(tmp_path, [
        {"op": OP_SET, "key": "a", "value": "1"},
        {"op": OP_INDETERMINATE, "key": "a", "reason": "write HTTP 502"},
    ])

    expected, indeterminate = replay_journal(path)

    assert expected == {}
    assert indeterminate == {"a"}


def test_a_plain_write_does_not_clear_poisoning(tmp_path):
    """This is the rule that is easy to get wrong, and unsound to relax.

    A write issued right after an unknown outcome can be OVERTAKEN by the very
    write whose fate is unknown: both are in flight, and raft commits them in
    whatever order they arrive. So an acknowledged SET is not by itself evidence
    that the key now holds that value. Only a RECLAIM — which the client emits
    only after the in-flight window has provably elapsed — restores trust.
    """
    path = write_journal(tmp_path, [
        {"op": OP_INDETERMINATE, "key": "a", "reason": "timeout"},
        {"op": OP_SET, "key": "a", "value": "hopeful"},
    ])

    expected, indeterminate = replay_journal(path)

    assert expected == {}, "a plain SET must not un-poison a key"
    assert indeterminate == {"a"}


def test_reclaim_restores_an_exact_expectation(tmp_path):
    path = write_journal(tmp_path, [
        {"op": OP_INDETERMINATE, "key": "a", "reason": "timeout"},
        {"op": OP_RECLAIM, "key": "a", "value": "known"},
    ])

    expected, indeterminate = replay_journal(path)

    assert expected == {"a": "known"}
    assert indeterminate == set()


def test_poisoning_after_a_reclaim_poisons_again(tmp_path):
    path = write_journal(tmp_path, [
        {"op": OP_INDETERMINATE, "key": "a", "reason": "timeout"},
        {"op": OP_RECLAIM, "key": "a", "value": "known"},
        {"op": OP_SET, "key": "a", "value": "later"},
        {"op": OP_INDETERMINATE, "key": "a", "reason": "another timeout"},
    ])

    expected, indeterminate = replay_journal(path)

    assert expected == {}
    assert indeterminate == {"a"}


def test_delete_while_poisoned_is_ignored(tmp_path):
    path = write_journal(tmp_path, [
        {"op": OP_INDETERMINATE, "key": "a", "reason": "timeout"},
        {"op": OP_DELETE, "key": "a"},
    ])

    expected, indeterminate = replay_journal(path)

    assert expected == {}, "a DELETE cannot be trusted while a write may be in flight"
    assert indeterminate == {"a"}


def test_independent_keys_do_not_interfere(tmp_path):
    path = write_journal(tmp_path, [
        {"op": OP_SET, "key": "a", "value": "1"},
        {"op": OP_INDETERMINATE, "key": "b", "reason": "timeout"},
        {"op": OP_SET, "key": "c", "value": "3"},
    ])

    expected, indeterminate = replay_journal(path)

    assert expected == {"a": "1", "c": "3"}
    assert indeterminate == {"b"}


def test_blank_lines_are_tolerated(tmp_path):
    path = tmp_path / "journal.jsonl"
    path.write_text('{"op":"set","key":"a","value":"1"}\n\n\n', encoding="utf-8")

    expected, _ = replay_journal(path)

    assert expected == {"a": "1"}


def test_malformed_line_raises_rather_than_being_skipped(tmp_path):
    """A journal that cannot be parsed must stop the run, not shrink it quietly.

    Skipping a bad line would silently drop keys from the expectation set, which
    is indistinguishable from a passing check.
    """
    path = tmp_path / "journal.jsonl"
    path.write_text('{"op":"set","key":"a","value":"1"}\nnot json\n', encoding="utf-8")

    try:
        replay_journal(path)
    except ValueError as exc:
        assert "malformed journal line" in str(exc)
        assert ":2:" in str(exc), "the error should name the offending line"
    else:
        raise AssertionError("a malformed journal line was silently ignored")
