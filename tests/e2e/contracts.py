"""The HTTP contract the C++ engine actually implements today.

Single source of truth for the strings this suite asserts on, imported by both
``conftest.py`` and ``test_cluster.py`` so the two can never drift apart. It is a
plain module rather than constants living in ``conftest.py`` because importing
``conftest`` by name is fragile once a second ``conftest.py`` exists anywhere in
the tree.

Every value here is copied from ``cpp-app/src/network/http_server.hpp``. Most of
them encode behavior that is **wrong on purpose** for now: ``KVHttpHandler``
answers HTTP 200 for every outcome and reports failure in the body. Phase 1
(truthful errors) replaces these with real status codes and structured bodies —
when it does, this file and the assertions that use it change together.
"""

from __future__ import annotations

# Request side ---------------------------------------------------------------

# The C++ handler routes POST /insert-val only when `request.is_msgpack` is set,
# which happens when a single header line contains both "content-type:" and
# "application/msgpack" as case-insensitive *substrings* — not an exact value
# match, so "Content-Type: application/msgpack; charset=utf-8" also routes (see
# MsgpackDetectionIsCaseInsensitive in cpp-app/tests/http_request_test.cpp).
# Anything without that pairing falls through to 404.
MSGPACK_CONTENT_TYPE = "application/msgpack"

# Response bodies ------------------------------------------------------------
#
# All three are returned via `HttpResponse::ok(...)`, i.e. with status 200.

#: A propose that Raft committed. Only the leader can produce this.
BODY_OK = "ok"

#: A propose that failed. Today this is indistinguishable between "this node is
#: not the leader" (the common case — there is no leader forwarding) and "the
#: sidecar is unreachable". Phase 1 gives it a status code, Phase 4 removes the
#: not-the-leader case by forwarding.
BODY_ERROR = "error"

#: Returned both for a key that was never written and for one that was deleted,
#: and also when the `key` query parameter is missing entirely.
BODY_KEY_NOT_FOUND = "Key Not Found"
