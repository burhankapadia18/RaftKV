/**
 * @file http_handler_test.cpp
 * @brief Unit tests for KVHttpHandler, HttpResponse and json_escape
 *        (spec R1.7 / R1.8).
 *
 * This is the truthfulness contract of the HTTP surface, asserted end to end
 * per row: status code, exact body, and Content-Type. Before Phase 1 every one
 * of these answered 200 with a bare word in the body ("ok", "error",
 * "Key Not Found"), and a 404 that did go out went out as the malformed status
 * line "HTTP/1.1 404 OK".
 *
 * KVHttpHandler takes IRaftClient, IKVStore and IAuthEngine by reference, so
 * the whole surface is reachable with three small doubles - no sockets, no
 * gRPC, no disk.
 *
 * The default fixture leaves auth DISABLED, which is the shipped default and
 * lets every pre-auth expectation below stand unchanged - that those rows did
 * not move is itself the assertion that enabling nothing changes nothing.
 * AuthenticatedHandlerTest at the bottom covers the auth-on surface.
 */

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "auth/auth_engine.hpp"
#include "auth/base64.hpp"
#include "auth/user_record.hpp"
#include "network/http_request.hpp"
#include "network/http_server.hpp"
#include "raft/raft_client.hpp"
#include "storage/kv_store.hpp"

namespace kvdb {
namespace {

/**
 * @brief Scriptable IRaftClient double.
 *
 * Returns whatever @c result the test set, and records the payload and the
 * call count so a test can prove that validation short-circuited before
 * anything reached consensus.
 */
class FakeRaftClient : public IRaftClient {
public:
  ProposeResult propose(const std::string &payload) override {
    ++calls;
    last_payload = payload;
    return result;
  }

  /** @brief Scriptable linearizable read (R4.5). */
  ReadResult read(const std::string &key) override {
    ++read_calls;
    last_read_key = key;
    return read_result;
  }

  ProposeResult result = ProposeResult::ok();
  std::string last_payload;
  int calls = 0;

  /** @brief Scriptable cluster status (console phase). */
  [[nodiscard]] StatusResult status() override {
    ++status_calls;
    return status_result;
  }

  ReadResult read_result = ReadResult::miss();
  std::string last_read_key;
  int read_calls = 0;

  StatusResult status_result;
  int status_calls = 0;
};

/**
 * @brief In-memory IStoreStats double.
 *
 * Separate from FakeKVStore on purpose: IStoreStats is its own seam, so a test
 * can drive the counters without touching the storage fake at all.
 */
class FakeStoreStats : public IStoreStats {
public:
  std::size_t keys = 0;
  std::size_t wal_bytes = 0;

  [[nodiscard]] std::size_t key_count() const override { return keys; }
  [[nodiscard]] std::size_t wal_size_bytes() const override {
    return wal_bytes;
  }
};

/** @brief In-memory IKVStore double - no file, no locking. */
class FakeKVStore : public IKVStore {
public:
  void set(const std::string &key, const std::string &value) override {
    entries_[key] = value;
  }

  [[nodiscard]] std::optional<std::string>
  get(const std::string &key) const override {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool remove(const std::string &key) override {
    return entries_.erase(key) > 0;
  }

  [[nodiscard]] bool contains(const std::string &key) const override {
    return entries_.count(key) > 0;
  }

  /** @brief Snapshot support (R3.2); the HTTP surface never calls these. */
  [[nodiscard]] StateMap snapshot_state() const override {
    return StateMap(entries_.begin(), entries_.end());
  }

  void restore_state(StateMap state) override {
    entries_.clear();
    entries_.insert(state.begin(), state.end());
  }

  /**
   * @brief scan_keys over the ordered map backing this fake.
   *
   * Same contract as PersistentKVStore::scan_keys, implemented independently:
   * a fake that delegated to the real store would not be a fake.
   */
  [[nodiscard]] KeyPage scan_keys(std::string_view prefix,
                                  std::string_view start,
                                  size_t limit) const override {
    KeyPage page;
    if (limit == 0) {
      return page;
    }
    const std::string from = std::string((start > prefix) ? start : prefix);
    for (auto it = entries_.lower_bound(from); it != entries_.end(); ++it) {
      const std::string &key = it->first;
      if (key.size() < prefix.size() ||
          key.compare(0, prefix.size(), prefix) != 0) {
        break;
      }
      if (page.keys.size() == limit) {
        return page;
      }
      page.keys.push_back(key);
    }
    page.reached_end = true;
    return page;
  }

private:
  std::map<std::string, std::string> entries_;
};

/** @brief A well-formed msgpack write request carrying @p body. */
HttpRequest insert_request(const std::string &body) {
  HttpRequest request;
  request.method = "POST";
  request.path = "/insert-val";
  request.is_msgpack = true;
  request.body = body;
  request.content_length = static_cast<int>(body.size());
  return request;
}

/** @brief A read request with the given raw (undecoded) query string. */
HttpRequest get_request(const std::string &query_string) {
  HttpRequest request;
  request.method = "GET";
  request.path = "/get-val";
  request.query_string = query_string;
  return request;
}

/**
 * @brief Scriptable IAuthEngine double.
 *
 * A test sets @c enabled_flag and @c identity (or @c outcome) and asserts on
 * the status code, so the handler's routing and authorization checks are
 * exercised without any hashing, any store, or any real credential.
 */
class FakeAuthEngine : public auth::IAuthEngine {
public:
  [[nodiscard]] bool enabled() const override { return enabled_flag; }

  [[nodiscard]] auth::AuthOutcome
  authenticate(const std::string &authorization,
               auth::AuthContext &out) const override {
    ++calls;
    last_authorization = authorization;
    if (outcome == auth::AuthOutcome::kOk) {
      out = identity;
    }
    return outcome;
  }

  bool enabled_flag = false;
  auth::AuthOutcome outcome = auth::AuthOutcome::kOk;
  auth::AuthContext identity = auth::AuthContext::unrestricted();
  mutable int calls = 0;
  mutable std::string last_authorization;
};

class KVHttpHandlerTest : public ::testing::Test {
protected:
  // `stats_` is a separate object from `store_` because IStoreStats is its own
  // seam; in production PersistentKVStore is passed twice, once per interface.
  KVHttpHandlerTest() : handler_(raft_, store_, auth_, stats_) {}

  FakeRaftClient raft_;
  FakeKVStore store_;
  FakeAuthEngine auth_;
  FakeStoreStats stats_;
  KVHttpHandler handler_;
};

// --- POST /insert-val: the propose outcomes -------------------------------

TEST_F(KVHttpHandlerTest, CommittedProposeReturns200Json) {
  const std::string payload("\x82\x00\xff", 3);

  const HttpResponse response = handler_.handle(insert_request(payload));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.body, "{\"ok\":true}");
  EXPECT_EQ(response.content_type, "application/json");
  // The body is forwarded byte for byte - the HTTP layer never decodes it.
  EXPECT_EQ(raft_.calls, 1);
  EXPECT_EQ(raft_.last_payload, payload);
}

TEST_F(KVHttpHandlerTest, NotLeaderReturns503NamingTheLeader) {
  raft_.result = ProposeResult::failure("not_leader:node1:8088");

  const HttpResponse response = handler_.handle(insert_request("payload"));

  EXPECT_EQ(response.status_code, 503);
  EXPECT_EQ(response.body,
            "{\"error\":\"not leader\",\"leader\":\"node1:8088\"}");
  EXPECT_EQ(response.content_type, "application/json");
}

TEST_F(KVHttpHandlerTest, NotLeaderWithNoKnownLeaderReturnsAnEmptyAddress) {
  // Mid-election the sidecar knows it is not the leader but not who is, so the
  // prefix arrives with nothing after the colon.
  raft_.result = ProposeResult::failure("not_leader:");

  const HttpResponse response = handler_.handle(insert_request("payload"));

  EXPECT_EQ(response.status_code, 503);
  EXPECT_EQ(response.body, "{\"error\":\"not leader\",\"leader\":\"\"}");
  EXPECT_EQ(response.content_type, "application/json");
}

TEST_F(KVHttpHandlerTest, OtherProposeFailureReturns502WithTheReason) {
  raft_.result = ProposeResult::failure("UNAVAILABLE: connection refused");

  const HttpResponse response = handler_.handle(insert_request("payload"));

  EXPECT_EQ(response.status_code, 502);
  EXPECT_EQ(response.body, "{\"error\":\"UNAVAILABLE: connection refused\"}");
  EXPECT_EQ(response.content_type, "application/json");
}

TEST_F(KVHttpHandlerTest, NotLeaderMustBeAPrefixNotASubstring) {
  // Phase 4 forwarding keys off the leading "not_leader:", so a message that
  // merely mentions it is an ordinary upstream failure.
  raft_.result = ProposeResult::failure("rejected (not_leader:node1:8088)");

  const HttpResponse response = handler_.handle(insert_request("payload"));

  EXPECT_EQ(response.status_code, 502);
  EXPECT_EQ(response.body, "{\"error\":\"rejected (not_leader:node1:8088)\"}");
}

TEST_F(KVHttpHandlerTest, JsonHostileProposeErrorIsEscaped) {
  // The propose error is pasted into a JSON body; a raw quote or backslash
  // would produce something no client can parse.
  raft_.result = ProposeResult::failure("boom \"x\" \\ y\n");

  const HttpResponse response = handler_.handle(insert_request("payload"));

  EXPECT_EQ(response.status_code, 502);
  EXPECT_EQ(response.body, "{\"error\":\"boom \\\"x\\\" \\\\ y\\n\"}");
}

TEST_F(KVHttpHandlerTest, JsonHostileLeaderAddressIsEscaped) {
  raft_.result = ProposeResult::failure("not_leader:no\"de:8088");

  const HttpResponse response = handler_.handle(insert_request("payload"));

  EXPECT_EQ(response.status_code, 503);
  EXPECT_EQ(response.body,
            "{\"error\":\"not leader\",\"leader\":\"no\\\"de:8088\"}");
}

// --- POST /insert-val: request validation ---------------------------------

TEST_F(KVHttpHandlerTest, WrongContentTypeReturns415AndNeverProposes) {
  HttpRequest request = insert_request("payload");
  request.is_msgpack = false;

  const HttpResponse response = handler_.handle(request);

  EXPECT_EQ(response.status_code, 415);
  EXPECT_EQ(response.body, "{\"error\":\"unsupported media type\","
                           "\"expected\":\"application/msgpack\"}");
  EXPECT_EQ(response.content_type, "application/json");
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(KVHttpHandlerTest, EmptyBodyReturns400AndNeverProposes) {
  const HttpResponse response = handler_.handle(insert_request(""));

  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(response.body, "{\"error\":\"empty request body\"}");
  EXPECT_EQ(response.content_type, "application/json");
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(KVHttpHandlerTest, MalformedContentLengthReturns400AndNeverProposes) {
  HttpRequest request = insert_request("payload");
  request.bad_content_length = true;
  request.content_length = 0;

  const HttpResponse response = handler_.handle(request);

  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(response.body, "{\"error\":\"malformed Content-Length\"}");
  EXPECT_EQ(response.content_type, "application/json");
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(KVHttpHandlerTest, GarbageContentLengthTravelsFromTheParserToA400) {
  // The R1.8 chain end to end: the header that used to kill the process now
  // comes out of the parser flagged and leaves as a 400.
  const std::optional<HttpRequest> request =
      HttpRequestParser::parse("POST /insert-val HTTP/1.1\r\n"
                               "Content-Type: application/msgpack\r\n"
                               "Content-Length: abc\r\n"
                               "\r\n"
                               "payload");
  ASSERT_TRUE(request.has_value());

  const HttpResponse response = handler_.handle(*request);

  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(response.body, "{\"error\":\"malformed Content-Length\"}");
  EXPECT_EQ(raft_.calls, 0);
}

// --- GET /get-val ---------------------------------------------------------

TEST_F(KVHttpHandlerTest, ReadHitReturns200AndTheRawValueAsPlainText) {
  store_.set("user:1", "alice");

  const HttpResponse response = handler_.handle(get_request("key=user:1"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.body, "alice");
  EXPECT_EQ(response.content_type, "text/plain; charset=utf-8");
}

TEST_F(KVHttpHandlerTest, ReadHitOnAJsonLookingValueIsStillNotEscaped) {
  // Only error bodies are JSON; a stored value comes back byte for byte, which
  // is why the read path advertises text/plain.
  store_.set("k", "{\"not\":\"ours\"}");

  const HttpResponse response = handler_.handle(get_request("key=k"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.body, "{\"not\":\"ours\"}");
  EXPECT_EQ(response.content_type, "text/plain; charset=utf-8");
}

TEST_F(KVHttpHandlerTest, ReadMissReturns404Json) {
  const HttpResponse response = handler_.handle(get_request("key=absent"));

  EXPECT_EQ(response.status_code, 404);
  EXPECT_EQ(response.body, "{\"error\":\"key not found\"}");
  EXPECT_EQ(response.content_type, "application/json");
}

TEST_F(KVHttpHandlerTest, ReadOfADeletedKeyIsAMiss) {
  store_.set("k", "v");
  ASSERT_TRUE(store_.remove("k"));

  const HttpResponse response = handler_.handle(get_request("key=k"));

  EXPECT_EQ(response.status_code, 404);
  EXPECT_EQ(response.body, "{\"error\":\"key not found\"}");
}

TEST_F(KVHttpHandlerTest, ReadWithoutTheKeyParameterReturns400) {
  // A missing parameter is a malformed request, not a missing key: 404 would
  // tell the client the key does not exist, which is not what happened.
  const HttpResponse response = handler_.handle(get_request("limit=10"));

  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(response.body,
            "{\"error\":\"missing required query parameter: key\"}");
  EXPECT_EQ(response.content_type, "application/json");
}

TEST_F(KVHttpHandlerTest, ReadWithAnEmptyKeyParameterIsALookupNotAnError) {
  // "key=" is present, just empty - that is an ordinary miss.
  const HttpResponse response = handler_.handle(get_request("key="));

  EXPECT_EQ(response.status_code, 404);
  EXPECT_EQ(response.body, "{\"error\":\"key not found\"}");
}

// --- Routing --------------------------------------------------------------

TEST_F(KVHttpHandlerTest, UnknownPathReturns404Json) {
  HttpRequest request;
  request.method = "GET";
  request.path = "/nope";

  const HttpResponse response = handler_.handle(request);

  EXPECT_EQ(response.status_code, 404);
  EXPECT_EQ(response.body, "{\"error\":\"not found\"}");
  EXPECT_EQ(response.content_type, "application/json");
}

TEST_F(KVHttpHandlerTest, WrongMethodOnAKnownPathReturns404AndNeverProposes) {
  HttpRequest request = insert_request("payload");
  request.method = "GET";

  const HttpResponse response = handler_.handle(request);

  EXPECT_EQ(response.status_code, 404);
  EXPECT_EQ(response.body, "{\"error\":\"not found\"}");
  EXPECT_EQ(raft_.calls, 0);
}

// --- HttpResponse serialization (R1.7) ------------------------------------

TEST(HttpResponseTest, ReasonPhraseIsCorrectForEveryStatusWeEmit) {
  EXPECT_STREQ(HttpResponse::reason_phrase(200), "OK");
  EXPECT_STREQ(HttpResponse::reason_phrase(201), "Created");
  EXPECT_STREQ(HttpResponse::reason_phrase(400), "Bad Request");
  EXPECT_STREQ(HttpResponse::reason_phrase(404), "Not Found");
  EXPECT_STREQ(HttpResponse::reason_phrase(415), "Unsupported Media Type");
  EXPECT_STREQ(HttpResponse::reason_phrase(500), "Internal Server Error");
  EXPECT_STREQ(HttpResponse::reason_phrase(502), "Bad Gateway");
  EXPECT_STREQ(HttpResponse::reason_phrase(503), "Service Unavailable");
  EXPECT_STREQ(HttpResponse::reason_phrase(599), "Unknown");
}

TEST(HttpResponseTest, SerializesStatusLineContentTypeLengthAndBody) {
  const HttpResponse response = HttpResponse::ok("hello");

  EXPECT_EQ(response.to_string(), "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/plain; charset=utf-8\r\n"
                                  "Content-Length: 5\r\n"
                                  "\r\n"
                                  "hello");
}

TEST(HttpResponseTest, NotFoundSerializesWithItsOwnReasonPhrase) {
  // REGRESSION (R1.7): to_string() used to hardcode " OK" for every status, so
  // this went out as the malformed status line "HTTP/1.1 404 OK".
  const HttpResponse response = HttpResponse::json_error(404, "key not found");

  EXPECT_EQ(response.to_string(), "HTTP/1.1 404 Not Found\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: 25\r\n"
                                  "\r\n"
                                  "{\"error\":\"key not found\"}");
}

TEST(HttpResponseTest, ServiceUnavailableSerializesWithItsOwnReasonPhrase) {
  const HttpResponse response =
      HttpResponse::json(503, "{\"error\":\"not leader\",\"leader\":\"n1\"}");

  EXPECT_EQ(response.to_string(),
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 36\r\n"
            "\r\n"
            "{\"error\":\"not leader\",\"leader\":\"n1\"}");
}

TEST(HttpResponseTest, ContentLengthCountsBytesNotCharacters) {
  // Bodies are binary-safe: a NUL in a stored value must not truncate the
  // response or the length header.
  const HttpResponse response = HttpResponse::ok(std::string("a\0b", 3));

  EXPECT_EQ(response.to_string(), "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/plain; charset=utf-8\r\n"
                                  "Content-Length: 3\r\n"
                                  "\r\n" +
                                      std::string("a\0b", 3));
}

// --- json_escape ----------------------------------------------------------

TEST(JsonEscapeTest, LeavesOrdinaryTextAlone) {
  EXPECT_EQ(json_escape(""), "");
  EXPECT_EQ(json_escape("not leader"), "not leader");
  EXPECT_EQ(json_escape("UNAVAILABLE: connect failed (node1:50052)"),
            "UNAVAILABLE: connect failed (node1:50052)");
}

TEST(JsonEscapeTest, EscapesDelimitersAndControlCharacters) {
  EXPECT_EQ(json_escape("\""), "\\\"");
  EXPECT_EQ(json_escape("\\"), "\\\\");
  EXPECT_EQ(json_escape("\b\f\n\r\t"), "\\b\\f\\n\\r\\t");
  EXPECT_EQ(json_escape(std::string("\x01", 1)), "\\u0001");
  EXPECT_EQ(json_escape(std::string("a\0b", 3)), "a\\u0000b");
  EXPECT_EQ(json_escape(std::string("\x1f", 1)), "\\u001f");
}

TEST(JsonEscapeTest, PassesUtf8ThroughUnchanged) {
  // Only C0 controls are escaped, so multi-byte sequences survive intact
  // instead of being mangled into per-byte \u escapes.
  const std::string utf8 = "caf\xc3\xa9";

  EXPECT_EQ(json_escape(utf8), utf8);
}

// --- R4.1/R4.5: transient vs fatal propose failures ------------------------

TEST_F(KVHttpHandlerTest, UnavailablePrefixBecomes503NotAFatal502) {
  // A routine leader failover produces "could not reach the leader" for a
  // second or two. It must not look fatal: a client that retries on 503 and
  // gives up on 502 would abandon a write the cluster is about to be able to
  // accept.
  raft_.result = ProposeResult::failure(std::string(kUnavailablePrefix) +
                                        "could not reach leader node1:8088");

  const HttpResponse response =
      handler_.handle(insert_request(std::string("\x82\x00\xff", 3)));

  EXPECT_EQ(response.status_code, 503);
  EXPECT_EQ(HttpResponse::reason_phrase(503),
            std::string("Service Unavailable"));
  // The prefix is a wire tag, not something a client should ever see.
  EXPECT_EQ(response.body.find(kUnavailablePrefix), std::string::npos)
      << "the machine-readable prefix leaked into the client-facing body: "
      << response.body;
  EXPECT_NE(response.body.find("could not reach leader"), std::string::npos);
}

TEST_F(KVHttpHandlerTest, AnUntaggedFailureIsStill502) {
  // Untagged means "the client cannot fix this by retrying" — it must stay 502,
  // or the retryable/fatal distinction the prefixes exist for is lost.
  raft_.result = ProposeResult::failure("state machine rejected the entry");

  const HttpResponse response =
      handler_.handle(insert_request(std::string("\x82\x00\xff", 3)));

  EXPECT_EQ(response.status_code, 502);
}

TEST_F(KVHttpHandlerTest, NotLeaderStillWinsOverUnavailable) {
  raft_.result =
      ProposeResult::failure(std::string(kNotLeaderPrefix) + "node2:8088");

  const HttpResponse response =
      handler_.handle(insert_request(std::string("\x82\x00\xff", 3)));

  EXPECT_EQ(response.status_code, 503);
  EXPECT_NE(response.body.find("\"leader\":\"node2:8088\""), std::string::npos);
}

// --- Request builders for the auth surface --------------------------------

namespace {

HttpRequest request_for(const std::string &method, const std::string &path) {
  HttpRequest request;
  request.method = method;
  request.path = path;
  return request;
}

HttpRequest msgpack_request(const std::string &method, const std::string &path,
                            const std::string &body) {
  HttpRequest request = request_for(method, path);
  request.is_msgpack = true;
  request.body = body;
  request.content_length = static_cast<int>(body.size());
  return request;
}

/** @brief The msgpack body of a PUT /auth/users/{name}. */
std::string upsert_body(const std::string &password,
                        const std::vector<std::string> &classes,
                        const std::vector<std::string> &patterns,
                        bool enabled = true) {
  auth::UserUpsertRequest request;
  request.password = password;
  request.enabled = enabled;
  request.classes = classes;
  request.patterns = patterns;
  msgpack::sbuffer buffer;
  msgpack::pack(buffer, request);
  return std::string(buffer.data(), buffer.size());
}

auth::AuthContext identity_with(const std::string &name,
                                const std::vector<std::string> &classes,
                                const std::vector<std::string> &patterns) {
  auth::AuthContext context;
  context.name = name;
  for (const std::string &cls : classes) {
    context.read = context.read || cls == auth::kClassRead;
    context.write = context.write || cls == auth::kClassWrite;
    context.admin = context.admin || cls == auth::kClassAdmin;
  }
  context.patterns = patterns;
  return context;
}

} // namespace

// --- Auth DISABLED: the unconditional rules -------------------------------
//
// These three hold with no admin password configured, and each is a deliberate
// behaviour change to the auth-off surface (recorded in the CHANGELOG). All are
// unreachable by any client that existed before: the `__sys:` prefix was never
// documented as usable, and USER_SET/USER_DEL were not operations.

TEST_F(KVHttpHandlerTest, AuthRoutesAreClosedWhenAuthIsDisabled) {
  // DEFAULT-CLOSED, like /join with no management token. An unconfigured
  // security feature must not read as "no restriction".
  for (const std::string &method :
       {std::string("GET"), std::string("PUT"), std::string("DELETE")}) {
    const HttpResponse response =
        handler_.handle(request_for(method, "/auth/users/alice"));
    EXPECT_EQ(response.status_code, 403) << "method " << method;
    EXPECT_NE(response.body.find("disabled"), std::string::npos);
  }
  EXPECT_EQ(handler_.handle(request_for("GET", "/auth/whoami")).status_code,
            403);
  EXPECT_EQ(raft_.calls, 0) << "nothing may reach consensus";
}

TEST_F(KVHttpHandlerTest, ReservedKeysAreRefusedOnEveryDataRouteWithAuthOff) {
  store_.set("__sys:user:alice", "secret-record-bytes");

  // Writes.
  EXPECT_EQ(
      handler_.handle(request_for("PUT", "/kv/__sys:user:alice")).status_code,
      403);
  EXPECT_EQ(handler_.handle(request_for("DELETE", "/kv/__sys:user:alice"))
                .status_code,
            403);
  // READS TOO — this is the one that matters. A local read of a user record
  // would hand out the salt and the password hash.
  const HttpResponse read =
      handler_.handle(request_for("GET", "/kv/__sys:user:alice"));
  EXPECT_EQ(read.status_code, 403);
  EXPECT_EQ(read.body.find("secret-record-bytes"), std::string::npos);
  EXPECT_EQ(handler_.handle(get_request("key=__sys:user:alice")).status_code,
            403);

  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(KVHttpHandlerTest, PercentEncodedReservedPrefixIsAlsoRefused) {
  // The check runs AFTER url_decode, so the prefix cannot be smuggled in
  // escapes. "__sys%3Auser%3Aadmin" decodes to "__sys:user:admin".
  const HttpResponse response =
      handler_.handle(request_for("PUT", "/kv/__sys%3Auser%3Aadmin"));

  EXPECT_EQ(response.status_code, 403);
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(KVHttpHandlerTest, KeysMerelyContainingTheMarkerAreStillOrdinary) {
  // Only a prefix is reserved; refusing more would take key names clients may
  // already use.
  EXPECT_EQ(handler_.handle(request_for("PUT", "/kv/app:__sys:ok")).status_code,
            200);
  EXPECT_EQ(raft_.calls, 1);
}

TEST_F(KVHttpHandlerTest, UserOpsOnInsertValAreRefusedEvenWithAuthOff) {
  // Closes the smuggling route: /insert-val forwards a raw command, so without
  // this check anyone able to POST could mint an administrator.
  for (const std::string &payload :
       {KVCommand::encode_user_set("evil", "record"),
        KVCommand::encode_user_del("admin")}) {
    const HttpResponse response = handler_.handle(insert_request(payload));
    EXPECT_EQ(response.status_code, 400);
    EXPECT_NE(response.body.find("user-management"), std::string::npos);
  }
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(KVHttpHandlerTest, AnUndecodableBodyIsStillForwardedWhenAuthIsOff) {
  // PINNED: "validation happens after commit" for a malformed body. It commits,
  // Apply rejects it, and the client sees 502. Auth-on refuses it up front
  // instead (see the authenticated fixture) — that asymmetry is deliberate and
  // documented, because a body naming no key cannot be checked against an ACL.
  raft_.result = ProposeResult::failure("malformed payload: bad msgpack");

  const HttpResponse response = handler_.handle(insert_request("not msgpack"));

  EXPECT_EQ(response.status_code, 502);
  EXPECT_EQ(raft_.calls, 1) << "the body must still reach consensus";
}

TEST_F(KVHttpHandlerTest, TheAuthenticatorIsNotEvenConsultedWhenDisabled) {
  handler_.handle(request_for("GET", "/kv/k"));
  handler_.handle(insert_request(KVCommand::encode_set("k", "v")));

  EXPECT_EQ(auth_.calls, 0);
}

// --- Auth ENABLED ---------------------------------------------------------

class AuthenticatedHandlerTest : public ::testing::Test {
protected:
  AuthenticatedHandlerTest() : handler_(raft_, store_, auth_, stats_) {
    auth_.enabled_flag = true;
    // Default caller: a full-access non-admin, so a test that cares about the
    // admin class has to grant it explicitly.
    auth_.identity =
        identity_with("alice", {auth::kClassRead, auth::kClassWrite}, {"*"});
  }

  FakeRaftClient raft_;
  FakeKVStore store_;
  FakeAuthEngine auth_;
  FakeStoreStats stats_;
  KVHttpHandler handler_;
};

TEST_F(AuthenticatedHandlerTest, MissingCredentialIs401WithAChallenge) {
  auth_.outcome = auth::AuthOutcome::kNoCredentials;

  const HttpResponse response = handler_.handle(request_for("GET", "/kv/k"));

  EXPECT_EQ(response.status_code, 401);
  EXPECT_EQ(HttpResponse::reason_phrase(401), "Unauthorized");
  // RFC 9110: a 401 without WWW-Authenticate tells a client to authenticate
  // without telling it how.
  ASSERT_EQ(response.extra_headers.size(), 1u);
  EXPECT_EQ(response.extra_headers[0].first, "WWW-Authenticate");
  EXPECT_EQ(response.extra_headers[0].second, "Basic realm=\"raftkv\"");
  // ...and it must actually reach the wire.
  EXPECT_NE(
      response.to_string().find("WWW-Authenticate: Basic realm=\"raftkv\""),
      std::string::npos);
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(AuthenticatedHandlerTest, RejectedCredentialIs403WithNoChallenge) {
  auth_.outcome = auth::AuthOutcome::kBadCredentials;

  const HttpResponse response = handler_.handle(request_for("GET", "/kv/k"));

  EXPECT_EQ(response.status_code, 403);
  EXPECT_EQ(HttpResponse::reason_phrase(403), "Forbidden");
  // No challenge: 403 is not an invitation to retry, and the same body covers
  // unknown user / disabled user / wrong password so the endpoint cannot be
  // used to enumerate accounts.
  EXPECT_TRUE(response.extra_headers.empty());
  EXPECT_NE(response.body.find("invalid credentials"), std::string::npos);
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(AuthenticatedHandlerTest, TheAuthorizationHeaderIsWhatGetsChecked) {
  HttpRequest request = request_for("GET", "/kv/k");
  request.headers["authorization"] = "Basic YWxpY2U6cHc=";
  store_.set("k", "v");

  handler_.handle(request);

  EXPECT_EQ(auth_.calls, 1);
  EXPECT_EQ(auth_.last_authorization, "Basic YWxpY2U6cHc=");
}

TEST_F(AuthenticatedHandlerTest, MetricsStaysOpenAndIsNeverAuthenticated) {
  // The secure profile's Caddy proxy health-checks /metrics. Requiring a
  // credential here drops every node out of the load-balancer rotation, so this
  // exemption is load-bearing rather than laziness. It exposes counters and
  // latencies, never keys or values.
  auth_.outcome = auth::AuthOutcome::kNoCredentials;

  const HttpResponse response = handler_.handle(request_for("GET", "/metrics"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(auth_.calls, 0);
}

TEST_F(AuthenticatedHandlerTest, ReadOnlyUserMayReadButNotWrite) {
  auth_.identity = identity_with("reader", {auth::kClassRead}, {"*"});
  store_.set("k", "v");

  EXPECT_EQ(handler_.handle(request_for("GET", "/kv/k")).status_code, 200);
  EXPECT_EQ(handler_.handle(get_request("key=k")).status_code, 200);

  const HttpResponse denied = handler_.handle(request_for("PUT", "/kv/k"));
  EXPECT_EQ(denied.status_code, 403);
  EXPECT_NE(denied.body.find("permission denied"), std::string::npos);
  EXPECT_EQ(handler_.handle(request_for("DELETE", "/kv/k")).status_code, 403);
  EXPECT_EQ(handler_.handle(insert_request(KVCommand::encode_set("k", "v")))
                .status_code,
            403);
  EXPECT_EQ(raft_.calls, 0) << "a denied write must not reach consensus";
}

TEST_F(AuthenticatedHandlerTest, WriteOnlyUserMayNotRead) {
  auth_.identity = identity_with("writer", {auth::kClassWrite}, {"*"});
  store_.set("k", "v");

  EXPECT_EQ(handler_.handle(request_for("PUT", "/kv/k")).status_code, 200);
  EXPECT_EQ(handler_.handle(request_for("GET", "/kv/k")).status_code, 403);
  EXPECT_EQ(handler_.handle(get_request("key=k")).status_code, 403);
}

TEST_F(AuthenticatedHandlerTest, KeyPatternsAreEnforcedOnEveryDataRoute) {
  auth_.identity =
      identity_with("scoped", {auth::kClassRead, auth::kClassWrite}, {"app:*"});
  store_.set("app:1", "in-scope");
  store_.set("other:1", "out-of-scope");

  EXPECT_EQ(handler_.handle(request_for("PUT", "/kv/app:1")).status_code, 200);
  EXPECT_EQ(handler_.handle(request_for("GET", "/kv/app:1")).status_code, 200);

  const HttpResponse denied =
      handler_.handle(request_for("GET", "/kv/other:1"));
  EXPECT_EQ(denied.status_code, 403);
  // The out-of-scope value must not leak in the refusal body.
  EXPECT_EQ(denied.body.find("out-of-scope"), std::string::npos);
  EXPECT_EQ(handler_.handle(request_for("PUT", "/kv/other:1")).status_code,
            403);
  EXPECT_EQ(handler_.handle(request_for("DELETE", "/kv/other:1")).status_code,
            403);
  EXPECT_EQ(handler_.handle(get_request("key=other:1")).status_code, 403);
}

TEST_F(AuthenticatedHandlerTest, InsertValIsCheckedAgainstTheKeyInsideTheBody) {
  // The legacy route forwards a raw command, so the ACL can only be applied by
  // decoding the body. Without this the route is a hole straight through the
  // key patterns.
  auth_.identity = identity_with("scoped", {auth::kClassWrite}, {"app:*"});

  EXPECT_EQ(handler_.handle(insert_request(KVCommand::encode_set("app:1", "v")))
                .status_code,
            200);
  EXPECT_EQ(
      handler_.handle(insert_request(KVCommand::encode_set("other:1", "v")))
          .status_code,
      403);
  EXPECT_EQ(handler_.handle(insert_request(KVCommand::encode_delete("other:1")))
                .status_code,
            403);
  EXPECT_EQ(raft_.calls, 1) << "only the in-scope write may be proposed";
}

TEST_F(AuthenticatedHandlerTest, AnUndecodableBodyIsRefusedWhenAuthIsOn) {
  // A body naming no key cannot be checked against a key pattern, so forwarding
  // it would be an unauthorized write to an unknown key. Auth-off keeps the old
  // commit-then-502 behaviour; see the note on that test.
  const HttpResponse response = handler_.handle(insert_request("not msgpack"));

  EXPECT_EQ(response.status_code, 400);
  EXPECT_NE(response.body.find("not a decodable command"), std::string::npos);
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(AuthenticatedHandlerTest, ReservedKeysAreRefusedEvenForAnAdmin) {
  // No exemption for anyone: user records are reachable only through /auth/*,
  // which never serializes a hash.
  auth_.identity = identity_with(
      "admin", {auth::kClassRead, auth::kClassWrite, auth::kClassAdmin}, {"*"});
  store_.set("__sys:user:bob", "record");

  EXPECT_EQ(
      handler_.handle(request_for("GET", "/kv/__sys:user:bob")).status_code,
      403);
  EXPECT_EQ(
      handler_.handle(request_for("PUT", "/kv/__sys:user:bob")).status_code,
      403);
}

// --- The user-management API ----------------------------------------------

class AdminHandlerTest : public AuthenticatedHandlerTest {
protected:
  AdminHandlerTest() {
    auth_.identity = identity_with(
        "admin", {auth::kClassRead, auth::kClassWrite, auth::kClassAdmin},
        {"*"});
  }
};

TEST_F(AuthenticatedHandlerTest, NonAdminIsRefusedTheUserManagementApi) {
  // alice holds read+write but not admin. The classes are independent: a data
  // account has no business creating accounts.
  EXPECT_EQ(handler_.handle(request_for("GET", "/auth/users/bob")).status_code,
            403);
  EXPECT_EQ(handler_
                .handle(msgpack_request(
                    "PUT", "/auth/users/bob",
                    upsert_body("bob-password", {auth::kClassRead}, {"*"})))
                .status_code,
            403);
  EXPECT_EQ(
      handler_.handle(request_for("DELETE", "/auth/users/bob")).status_code,
      403);
  EXPECT_EQ(raft_.calls, 0) << "a refused admin call must not reach consensus";
}

TEST_F(AuthenticatedHandlerTest, WhoamiNeedsNoClassAndReportsTheCallersAcl) {
  auth_.identity = identity_with("scoped", {auth::kClassRead}, {"app:*"});

  const HttpResponse response =
      handler_.handle(request_for("GET", "/auth/whoami"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.content_type, "application/json");
  EXPECT_NE(response.body.find("\"name\":\"scoped\""), std::string::npos);
  EXPECT_NE(response.body.find("\"classes\":[\"read\"]"), std::string::npos);
  EXPECT_NE(response.body.find("\"patterns\":[\"app:*\"]"), std::string::npos);
}

TEST_F(AdminHandlerTest, PutUserProposesAUserSetCarryingAUsableRecord) {
  const HttpResponse response = handler_.handle(msgpack_request(
      "PUT", "/auth/users/bob",
      upsert_body("bob-password", {auth::kClassRead}, {"app:*"})));

  EXPECT_EQ(response.status_code, 200);
  ASSERT_EQ(raft_.calls, 1);

  // Decode what was actually proposed: the op, the bare name as the key, and a
  // record whose stored hash verifies against the password that was sent.
  const KVCommand proposed = KVCommand::from_msgpack(raft_.last_payload.data(),
                                                     raft_.last_payload.size());
  EXPECT_EQ(proposed.op, "USER_SET");
  EXPECT_EQ(proposed.key, "bob");
  const auth::UserRecord record = auth::UserRecord::from_msgpack(
      proposed.value.data(), proposed.value.size());
  EXPECT_EQ(record.name, "bob");
  EXPECT_EQ(record.version, 1);
  EXPECT_TRUE(record.enabled);
  EXPECT_EQ(record.classes, std::vector<std::string>{auth::kClassRead});
  EXPECT_EQ(record.patterns, std::vector<std::string>{"app:*"});
  EXPECT_FALSE(record.validation_error().has_value());
  EXPECT_EQ(record.pw_sha256_hex,
            auth::hash_password(record.salt_hex, "bob-password"))
      << "the stored hash must verify against the submitted password";
}

TEST_F(AdminHandlerTest, TheSubmittedPasswordNeverAppearsInTheProposedBytes) {
  handler_.handle(msgpack_request("PUT", "/auth/users/bob",
                                  upsert_body("uniquely-identifiable-password",
                                              {auth::kClassRead}, {"*"})));

  ASSERT_EQ(raft_.calls, 1);
  // The cleartext must not reach the raft log — it is replicated to every node
  // and written to every WAL, and log entries outlive the request by design.
  EXPECT_EQ(raft_.last_payload.find("uniquely-identifiable-password"),
            std::string::npos);
}

TEST_F(AdminHandlerTest, PutUserRequiresMsgpackAndAValidBody) {
  HttpRequest not_msgpack = request_for("PUT", "/auth/users/bob");
  not_msgpack.body = "{}";
  EXPECT_EQ(handler_.handle(not_msgpack).status_code, 415);

  EXPECT_EQ(handler_.handle(msgpack_request("PUT", "/auth/users/bob", ""))
                .status_code,
            400);
  EXPECT_EQ(
      handler_.handle(msgpack_request("PUT", "/auth/users/bob", "not msgpack"))
          .status_code,
      400);

  // A password too short to survive an offline attack on the stored digest.
  const HttpResponse short_password = handler_.handle(
      msgpack_request("PUT", "/auth/users/bob",
                      upsert_body("short", {auth::kClassRead}, {"*"})));
  EXPECT_EQ(short_password.status_code, 400);
  EXPECT_NE(short_password.body.find("8 bytes"), std::string::npos);

  // An unknown class is a typo that would otherwise silently grant nothing.
  EXPECT_EQ(handler_
                .handle(msgpack_request(
                    "PUT", "/auth/users/bob",
                    upsert_body("bob-password", {"superuser"}, {"*"})))
                .status_code,
            400);

  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(AdminHandlerTest, TheBootstrapAdminCannotBeManagedThroughTheApi) {
  // It is defined by RAFTKV_ADMIN_PASSWORD and checked before the store, so a
  // record under that name would be dead weight that looks like a live account.
  const HttpResponse put = handler_.handle(msgpack_request(
      "PUT", "/auth/users/admin",
      upsert_body("new-admin-password", {auth::kClassAdmin}, {"*"})));

  EXPECT_EQ(put.status_code, 400);
  EXPECT_NE(put.body.find("RAFTKV_ADMIN_PASSWORD"), std::string::npos);
  EXPECT_EQ(
      handler_.handle(request_for("DELETE", "/auth/users/admin")).status_code,
      400);
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(AdminHandlerTest, AnUnusableUserNameIsRejectedBeforeConsensus) {
  for (const std::string &path :
       {std::string("/auth/users/has%20space"),
        std::string("/auth/users/star*"), std::string("/auth/users/")}) {
    const HttpResponse response = handler_.handle(msgpack_request(
        "PUT", path, upsert_body("some-password", {auth::kClassRead}, {"*"})));
    EXPECT_EQ(response.status_code, 400) << "path " << path;
  }
  EXPECT_EQ(raft_.calls, 0);
}

TEST_F(AdminHandlerTest, DeleteUserProposesAUserDel) {
  const HttpResponse response =
      handler_.handle(request_for("DELETE", "/auth/users/bob"));

  EXPECT_EQ(response.status_code, 200);
  ASSERT_EQ(raft_.calls, 1);
  const KVCommand proposed = KVCommand::from_msgpack(raft_.last_payload.data(),
                                                     raft_.last_payload.size());
  EXPECT_EQ(proposed.op, "USER_DEL");
  EXPECT_EQ(proposed.key, "bob");
}

TEST_F(AdminHandlerTest, GetUserReturnsTheAclAndNeverTheSecret) {
  auth::UserUpsertRequest upsert;
  upsert.password = "bob-password";
  upsert.classes = {auth::kClassRead};
  upsert.patterns = {"app:*"};
  const auth::UserRecord stored = upsert.to_record("bob");
  store_.set(auth::user_storage_key("bob"), stored.to_msgpack());

  const HttpResponse response =
      handler_.handle(request_for("GET", "/auth/users/bob"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_NE(response.body.find("\"name\":\"bob\""), std::string::npos);
  EXPECT_NE(response.body.find("\"enabled\":true"), std::string::npos);
  EXPECT_NE(response.body.find("\"classes\":[\"read\"]"), std::string::npos);
  EXPECT_NE(response.body.find("\"patterns\":[\"app:*\"]"), std::string::npos);
  // THE LOAD-BEARING ASSERTION. An endpoint that returned these turns one
  // compromised admin credential into an offline attack on every password.
  EXPECT_EQ(response.body.find(stored.salt_hex), std::string::npos);
  EXPECT_EQ(response.body.find(stored.pw_sha256_hex), std::string::npos);
  EXPECT_EQ(response.body.find("salt"), std::string::npos);
}

TEST_F(AdminHandlerTest, GetUserIs404WhenThereIsNoSuchUser) {
  const HttpResponse response =
      handler_.handle(request_for("GET", "/auth/users/nobody"));

  EXPECT_EQ(response.status_code, 404);
  EXPECT_NE(response.body.find("user not found"), std::string::npos);
}

TEST_F(AdminHandlerTest, PatternsWithQuotesAreEscapedInTheResponse) {
  // Pattern text is operator-supplied and reaches a hand-built JSON body; an
  // unescaped quote produces a body no client can parse.
  auth::UserUpsertRequest upsert;
  upsert.password = "bob-password";
  upsert.patterns = {"a\"b"};
  store_.set(auth::user_storage_key("bob"),
             upsert.to_record("bob").to_msgpack());

  const HttpResponse response =
      handler_.handle(request_for("GET", "/auth/users/bob"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_NE(response.body.find("a\\\"b"), std::string::npos);
}

TEST_F(AdminHandlerTest, UserRoutesUseAWrongMethodAndUnknownPathCorrectly) {
  EXPECT_EQ(handler_.handle(request_for("POST", "/auth/users/bob")).status_code,
            405);
  EXPECT_EQ(handler_.handle(request_for("POST", "/auth/whoami")).status_code,
            405);
  EXPECT_EQ(handler_.handle(request_for("GET", "/auth/nonsense")).status_code,
            404);
}

TEST_F(AdminHandlerTest, AdminWritesInheritTheStandardFailureVocabulary) {
  // The admin API proposes through the same path as a data write, so a follower
  // answers 503 naming the leader rather than inventing a new error shape.
  raft_.result =
      ProposeResult::failure(std::string(kNotLeaderPrefix) + "node1:8088");

  const HttpResponse response = handler_.handle(
      msgpack_request("PUT", "/auth/users/bob",
                      upsert_body("bob-password", {auth::kClassRead}, {"*"})));

  EXPECT_EQ(response.status_code, 503);
  EXPECT_NE(response.body.find("\"leader\":\"node1:8088\""), std::string::npos);
}

// --- Metric labels --------------------------------------------------------

TEST_F(AdminHandlerTest, TheUserNameNeverBecomesAMetricLabel) {
  // Labelling by path would create one time series per user name — the same
  // unbounded-cardinality mistake /kv/{key} avoids. Asserted through the
  // rendered exposition, which is where it would actually show up.
  handler_.handle(request_for("GET", "/auth/users/some-unique-user-name"));

  const std::string exposition = metrics::Registry::global().render();
  EXPECT_EQ(exposition.find("some-unique-user-name"), std::string::npos);
  EXPECT_NE(exposition.find("/auth/users/{name}"), std::string::npos);
}

// --- GET /kv (list) --------------------------------------------------------
//
// Every key in the body is PERCENT-ENCODED. Keys are arbitrary bytes; a JSON
// string is Unicode text, so a key holding a raw 0x80 would produce a body no
// parser accepts -- and json_escape() would not save it, because it passes
// bytes >= 0x20 straight through (correct for UTF-8, wrong for arbitrary).

namespace {

/** @brief GET /kv with the given raw (undecoded) query string. */
HttpRequest list_request(const std::string &query_string) {
  HttpRequest request;
  request.method = "GET";
  request.path = "/kv";
  request.query_string = query_string;
  return request;
}

} // namespace

TEST_F(KVHttpHandlerTest, KvListReturnsPercentEncodedKeysInOrder) {
  store_.set("app:b", "2");
  store_.set("app:a", "1");
  store_.set("other", "3");

  const HttpResponse response = handler_.handle(list_request("prefix=app%3A"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.content_type, kJsonContentType);
  EXPECT_EQ(response.body, "{\"keys\":[\"app%3Aa\",\"app%3Ab\"]}");
}

TEST_F(KVHttpHandlerTest, KvListOmitsCursorAtTheEndOfTheRange) {
  store_.set("k1", "v");
  const HttpResponse response = handler_.handle(list_request("prefix=k"));
  // No next_cursor: a client pages until the field is ABSENT, never by
  // comparing the returned count against limit.
  EXPECT_EQ(response.body, "{\"keys\":[\"k1\"]}");
}

TEST_F(KVHttpHandlerTest, KvListPaginatesWithAnOpaquePosition) {
  for (const char *key : {"k1", "k2", "k3"}) {
    store_.set(key, "v");
  }

  const HttpResponse first = handler_.handle(list_request("prefix=k&limit=2"));
  EXPECT_EQ(first.status_code, 200);
  // The position is the last emitted key plus a NUL, so it reveals only a key
  // the caller has already been shown.
  EXPECT_EQ(first.body, "{\"keys\":[\"k1\",\"k2\"],\"next_cursor\":\"k2%00\"}");

  const HttpResponse second =
      handler_.handle(list_request("prefix=k&limit=2&cursor=k2%00"));
  EXPECT_EQ(second.body, "{\"keys\":[\"k3\"]}");
}

TEST_F(KVHttpHandlerTest, KvListEncodesKeysThatAreNotValidUtf8) {
  store_.set(std::string("k\x80\x01", 3), "v");
  const HttpResponse response = handler_.handle(list_request("prefix=k"));
  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.body, "{\"keys\":[\"k%80%01\"]}");
}

TEST_F(KVHttpHandlerTest, KvListNeverRevealsAReservedKeyOrPosition) {
  store_.set("__sys:user:alice", "record");
  store_.set("a", "v");

  const HttpResponse response = handler_.handle(list_request("limit=1"));
  EXPECT_EQ(response.status_code, 200);
  // The reserved range is jumped over as a range, so neither the keys array
  // nor the cursor can name a user record.
  EXPECT_EQ(response.body, "{\"keys\":[\"a\"]}");
  EXPECT_EQ(response.body.find("__sys"), std::string::npos);
}

TEST_F(KVHttpHandlerTest, KvListRefusesAReservedPrefix) {
  const HttpResponse response =
      handler_.handle(list_request("prefix=__sys%3A"));
  EXPECT_EQ(response.status_code, 403);
  EXPECT_NE(response.body.find("are reserved"), std::string::npos);
}

TEST_F(KVHttpHandlerTest, KvListRejectsABadLimit) {
  EXPECT_EQ(handler_.handle(list_request("limit=0")).status_code, 400);
  EXPECT_EQ(handler_.handle(list_request("limit=-1")).status_code, 400);
  EXPECT_EQ(handler_.handle(list_request("limit=abc")).status_code, 400);
  EXPECT_EQ(handler_.handle(list_request("limit=abc")).body,
            "{\"error\":\"limit must be an integer between 1 and 500\"}");
}

TEST_F(KVHttpHandlerTest, KvListClampsAnOverLargeLimit) {
  // A cap is not a client error.
  EXPECT_EQ(handler_.handle(list_request("limit=100000")).status_code, 200);
}

TEST_F(KVHttpHandlerTest, KvListRejectsMalformedPercentEncoding) {
  const HttpResponse response = handler_.handle(list_request("prefix=%zz"));
  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(response.body,
            "{\"error\":\"malformed percent-encoding in the query "
            "string\"}");
}

TEST_F(KVHttpHandlerTest, KvListDoesNotCollideWithTheSingleKeyRoute) {
  // "/kv" has no trailing slash; "/kv/" is still the empty-key 400.
  EXPECT_EQ(handler_.handle(request_for("GET", "/kv/")).status_code, 400);
}

TEST_F(KVHttpHandlerTest, KvListRejectsANonGetMethod) {
  HttpRequest request = list_request("");
  request.method = "POST";
  EXPECT_EQ(handler_.handle(request).status_code, 405);
}

TEST_F(AuthenticatedHandlerTest, KvListRequiresTheReadClass) {
  auth_.identity = identity_with("writer", {auth::kClassWrite}, {"*"});

  const HttpResponse response = handler_.handle(list_request("prefix=app%3A"));
  EXPECT_EQ(response.status_code, 403);
  EXPECT_EQ(response.body, "{\"error\":\"permission denied\"}");
}

TEST_F(AuthenticatedHandlerTest,
       KvListAppliesKeyPatternsAndRequiresACoveredPrefix) {
  store_.set("app:mine", "v");
  store_.set("other:theirs", "v");
  auth_.identity = identity_with("scoped", {auth::kClassRead}, {"app:*"});

  // Inside the allowance: fine.
  const HttpResponse allowed = handler_.handle(list_request("prefix=app%3A"));
  EXPECT_EQ(allowed.status_code, 200);
  EXPECT_EQ(allowed.body, "{\"keys\":[\"app%3Amine\"]}");

  // Outside it: refused, rather than scanning keys it may not see. This is
  // what keeps a filtered-out key name out of the returned position.
  const HttpResponse refused = handler_.handle(list_request("prefix=other%3A"));
  EXPECT_EQ(refused.status_code, 403);
  EXPECT_EQ(refused.body,
            "{\"error\":\"prefix must fall within your permitted key "
            "patterns\"}");

  // And an unrestricted scan is refused for the same reason.
  EXPECT_EQ(handler_.handle(list_request("")).status_code, 403);
}

// --- GET /cluster/status ---------------------------------------------------

namespace {

/** @brief A StatusResult the sidecar answered successfully. */
StatusResult healthy_status() {
  StatusResult status;
  status.ok = true;
  status.node_id = "node1";
  status.state = "Leader";
  return status;
}

} // namespace

TEST_F(KVHttpHandlerTest, ClusterStatusRendersRaftAndLocalState) {
  raft_.status_result = StatusResult{};
  raft_.status_result.ok = true;
  raft_.status_result.node_id = "node1";
  raft_.status_result.state = "Leader";
  raft_.status_result.term = 4;
  raft_.status_result.leader_id = "node1";
  raft_.status_result.leader_addr = "node1:8088";
  raft_.status_result.first_log_index = 1;
  raft_.status_result.last_log_index = 118;
  raft_.status_result.applied_index = 117;
  raft_.status_result.commit_index = 118;
  raft_.status_result.last_snapshot_index = 0;
  raft_.status_result.peers = {RaftPeer{"node1", "node1:8088", "Voter"},
                               RaftPeer{"node2", "node2:8088", "Voter"}};
  stats_.keys = 42;
  stats_.wal_bytes = 9310;

  const HttpResponse response =
      handler_.handle(request_for("GET", "/cluster/status"));

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.content_type, kJsonContentType);
  EXPECT_EQ(response.body,
            "{\"node_id\":\"node1\",\"state\":\"Leader\",\"term\":4,"
            "\"leader_id\":\"node1\",\"leader_addr\":\"node1:8088\","
            "\"peers\":["
            "{\"id\":\"node1\",\"address\":\"node1:8088\","
            "\"suffrage\":\"Voter\"},"
            "{\"id\":\"node2\",\"address\":\"node2:8088\","
            "\"suffrage\":\"Voter\"}],"
            "\"first_log_index\":1,\"last_log_index\":118,"
            "\"applied_index\":117,\"commit_index\":118,"
            "\"last_snapshot_index\":0,"
            "\"key_count\":42,\"wal_bytes\":9310,"
            "\"auth_enabled\":false}");
}

TEST_F(KVHttpHandlerTest, ClusterStatusIs502WhenTheSidecarIsUnreachable) {
  raft_.status_result = StatusResult::failure("sidecar unreachable");

  const HttpResponse response =
      handler_.handle(request_for("GET", "/cluster/status"));

  // 502, not 200-with-zeros. A dashboard rendering "term 0, no peers, not
  // leader" for a node whose sidecar is merely unreachable looks exactly like
  // a cluster that has lost quorum, which is worse than an error.
  EXPECT_EQ(response.status_code, 502);
  EXPECT_EQ(response.body, "{\"error\":\"sidecar unreachable\"}");
}

TEST_F(KVHttpHandlerTest, ClusterStatusReportsAPartialFailure) {
  raft_.status_result = healthy_status();
  raft_.status_result.state = "Follower";
  raft_.status_result.partial_error = "failed to read first log index";

  const HttpResponse response =
      handler_.handle(request_for("GET", "/cluster/status"));
  EXPECT_EQ(response.status_code, 200);
  EXPECT_NE(response.body.find("\"error\":\"failed to read first log index\""),
            std::string::npos);
}

TEST_F(KVHttpHandlerTest, ClusterStatusOmitsTheErrorFieldWhenHealthy) {
  raft_.status_result = healthy_status();

  const HttpResponse response =
      handler_.handle(request_for("GET", "/cluster/status"));
  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.body.find("\"error\""), std::string::npos);
}

TEST_F(KVHttpHandlerTest, ClusterStatusRejectsANonGetMethod) {
  raft_.status_result = healthy_status();

  const HttpResponse response =
      handler_.handle(request_for("POST", "/cluster/status"));
  EXPECT_EQ(response.status_code, 405);
}

TEST_F(KVHttpHandlerTest, ClusterStatusReportsAuthEnabled) {
  // The console shows an "unauthenticated" banner off this field, so it must
  // reflect the engine's real configuration rather than the request.
  raft_.status_result = healthy_status();
  EXPECT_NE(handler_.handle(request_for("GET", "/cluster/status"))
                .body.find("\"auth_enabled\":false"),
            std::string::npos);
}

TEST_F(AuthenticatedHandlerTest, ClusterStatusRequiresTheReadClassOnly) {
  // read-class, no key pattern check: it addresses no key, and requiring admin
  // would hide the overview page from the users most likely to open it.
  raft_.status_result = healthy_status();

  auth_.identity = identity_with("reader", {auth::kClassRead}, {"nothing:*"});
  EXPECT_EQ(handler_.handle(request_for("GET", "/cluster/status")).status_code,
            200);
  // ...and it reports auth as ON, unlike the fixture above.
  EXPECT_NE(handler_.handle(request_for("GET", "/cluster/status"))
                .body.find("\"auth_enabled\":true"),
            std::string::npos);

  auth_.identity = identity_with("writer", {auth::kClassWrite}, {"*"});
  EXPECT_EQ(handler_.handle(request_for("GET", "/cluster/status")).status_code,
            403);

  auth_.outcome = auth::AuthOutcome::kNoCredentials;
  EXPECT_EQ(handler_.handle(request_for("GET", "/cluster/status")).status_code,
            401);
}

} // namespace
} // namespace kvdb
