"""The HTTP contract the C++ engine implements, expressed as data.

Single source of truth for the status codes, media types and message strings
this suite asserts on, imported by both ``conftest.py`` and ``test_cluster.py``
so the two can never drift apart. It is a plain module rather than constants
living in ``conftest.py`` because importing ``conftest`` by name is fragile once
a second ``conftest.py`` exists anywhere in the tree.

Every value here is copied from ``cpp-app/src/network/http_server.hpp``. Phase 1
("truthful errors") retired the previous *200-for-everything* contract: a
failure now carries a real status code, the matching reason phrase on the status
line, and a JSON body naming what went wrong. Changing any of it means changing
the C++ handler, this file and the README API tables in the same commit.

The full contract:

``POST /insert-val``
    ==============================  ======  =========================================================
    Outcome                         Status  Body (``application/json`` unless noted)
    ==============================  ======  =========================================================
    committed                       200     ``{"ok":true}``
    this node is not the leader     503     ``{"error":"not leader","leader":"<raft addr>"}``
    propose failed some other way   502     ``{"error":"<reason>"}``
    Content-Type is not msgpack     415     ``{"error":"unsupported media type","expected":"..."}``
    empty body                      400     ``{"error":"empty request body"}``
    unparseable Content-Length      400     ``{"error":"malformed Content-Length"}``
    ==============================  ======  =========================================================

``GET /get-val?key=...``
    ==============================  ======  =========================================================
    key present                     200     the stored value verbatim, ``text/plain; charset=utf-8``
    key absent                      404     ``{"error":"key not found"}``
    no ``key`` query parameter      400     ``{"error":"missing required query parameter: key"}``
    ==============================  ======  =========================================================

Anything else: 404 ``{"error":"not found"}``.
"""

from __future__ import annotations

import re

# Request side ---------------------------------------------------------------

# The C++ handler treats a request as msgpack when the Content-Type header's
# VALUE contains "application/msgpack" as a case-insensitive substring -- not an
# exact match, so "Content-Type: application/msgpack; charset=utf-8" also
# qualifies (see MsgpackDetectionAllowsMediaTypeParameters in
# cpp-app/tests/http_request_test.cpp). The field NAME is matched exactly: the
# parser used to scan whole header lines for the substring "content-type:", so a
# header merely named like one (X-Content-Type) hijacked the flag. Anything else
# reaching POST /insert-val is a 415.
MSGPACK_CONTENT_TYPE = "application/msgpack"

# Response media types -------------------------------------------------------

#: Every structured body (success envelope and every error) is JSON.
JSON_CONTENT_TYPE = "application/json"

#: A successful read returns the stored bytes verbatim, not wrapped in JSON.
TEXT_CONTENT_TYPE = "text/plain; charset=utf-8"

# Status codes ---------------------------------------------------------------
#
# Named because the tests now assert on them: a bare `== 503` in an assertion
# reads as a magic number, and these are the only codes the handler can emit.

HTTP_OK = 200
HTTP_BAD_REQUEST = 400
#: No usable credential was presented. Carries WWW-Authenticate; a retry may work.
HTTP_UNAUTHORIZED = 401
#: A credential was presented and rejected, or does not permit this. No retry.
HTTP_FORBIDDEN = 403
HTTP_NOT_FOUND = 404
HTTP_UNSUPPORTED_MEDIA_TYPE = 415
HTTP_PAYLOAD_TOO_LARGE = 413
HTTP_HEADERS_TOO_LARGE = 431
HTTP_METHOD_NOT_ALLOWED = 405
HTTP_BAD_GATEWAY = 502
HTTP_SERVICE_UNAVAILABLE = 503

#: Mirrors ``HttpResponse::reason_phrase``. The status line used to say ``OK``
#: for every code (``HTTP/1.1 404 OK``); R1.7 fixed that, so the reason phrase
#: is now worth asserting on -- it is the cheapest possible detector for a
#: regression back to the hardcoded phrase.
REASON_PHRASES = {
    HTTP_OK: "OK",
    201: "Created",
    HTTP_BAD_REQUEST: "Bad Request",
    HTTP_UNAUTHORIZED: "Unauthorized",
    HTTP_FORBIDDEN: "Forbidden",
    HTTP_NOT_FOUND: "Not Found",
    HTTP_UNSUPPORTED_MEDIA_TYPE: "Unsupported Media Type",
    500: "Internal Server Error",
    HTTP_BAD_GATEWAY: "Bad Gateway",
    HTTP_SERVICE_UNAVAILABLE: "Service Unavailable",
}

# Response bodies ------------------------------------------------------------

#: The success envelope for a committed write, parsed. Asserting on the parsed
#: object rather than the raw bytes keeps the tests from breaking over
#: whitespace while still pinning the shape.
WRITE_OK_BODY = {"ok": True}

#: The same thing byte for byte, as the C++ handler emits it. Used where the
#: exact serialization matters (raw-socket assertions).
WRITE_OK_BYTES = '{"ok":true}'

# The ``error`` member of the JSON envelope, one constant per failure the
# handler can produce.
ERROR_NOT_LEADER = "not leader"
ERROR_KEY_NOT_FOUND = "key not found"
ERROR_MISSING_KEY_PARAM = "missing required query parameter: key"
ERROR_EMPTY_BODY = "empty request body"
ERROR_MALFORMED_CONTENT_LENGTH = "malformed Content-Length"
ERROR_UNSUPPORTED_MEDIA_TYPE = "unsupported media type"
ERROR_NOT_FOUND = "not found"
ERROR_BODY_TOO_LARGE = "request body too large"
ERROR_HEADERS_TOO_LARGE = "request headers too large"
ERROR_EMPTY_KEY = "key must not be empty"
ERROR_BAD_CONSISTENCY = 'consistency must be "local" or "linearizable"'
ERROR_METHOD_NOT_ALLOWED_KV = "method not allowed on /kv/{key}: use PUT, GET or DELETE"

# Client authentication (the auth phase). Mirrored from KVHttpHandler; the
# 401/403 split is the contract, not an implementation detail:
#   401 = nothing usable was presented -> carries the challenge, retry may work
#   403 = presented and rejected, or insufficient -> no challenge, no retry
ERROR_AUTH_REQUIRED = "authentication required"
#: ONE message for unknown user, disabled user and wrong password. Reporting them
#: apart would make the endpoint a user-enumeration oracle.
ERROR_INVALID_CREDENTIALS = "invalid credentials"
ERROR_PERMISSION_DENIED = "permission denied"
ERROR_USER_NOT_FOUND = "user not found"

#: The challenge on every 401. Without it a client is told to authenticate and
#: not told how (RFC 9110).
WWW_AUTHENTICATE_HEADER = "WWW-Authenticate"
WWW_AUTHENTICATE_BASIC = 'Basic realm="raftkv"'

#: The reserved key space. Refused on every DATA route — reads included, because
#: a local read of a user record would return its salt and password hash — and
#: refused again at apply time, which is what stops the unauthenticated sidecar
#: port from rewriting the user table with an ordinary SET.
SYS_KEY_PREFIX = "__sys:"
USER_KEY_PREFIX = "__sys:user:"

#: The bootstrap admin, defined by RAFTKV_ADMIN_PASSWORD rather than by a record,
#: and checked before the store — so it cannot be created, changed or deleted
#: through the API, and a planted record under this name is shadowed.
BOOTSTRAP_ADMIN = "admin"

#: Command classes. Independent, NOT a hierarchy: admin does not imply read.
CLASS_READ = "read"
CLASS_WRITE = "write"
CLASS_ADMIN = "admin"

#: Minimum password length enforced by UserUpsertRequest::validation_error.
MIN_PASSWORD_BYTES = 8

# Phase 4 request caps, mirrored from RequestLimits in cpp-app/src/config/
# config.hpp. Duplicated here on purpose: these tests exist to catch the caps
# changing, so reading them from the implementation would make the test agree
# with whatever the code does — which is the opposite of a contract test.
MAX_BODY_BYTES = 1 * 1024 * 1024
MAX_HEADER_BYTES = 32 * 1024

#: Shape of the ``leader`` member of a 503 body. It comes from raft's
#: ``LeaderWithID`` via ``ProposeResponse.error`` ("not_leader:<addr>"), so it is
#: the peer's **Raft** address -- ``node1:8088`` under docker-compose.yml, where
#: entrypoint.sh advertises the container hostname on the raft port. It is
#: emphatically *not* the node's HTTP base URL, and it is empty while no leader
#: is known, which is why the tests match a shape instead of a literal.
RAFT_ADDRESS_RE = re.compile(r"^[^\s:]+:[0-9]+$")
