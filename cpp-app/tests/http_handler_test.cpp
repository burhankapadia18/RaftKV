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
 * KVHttpHandler takes IRaftClient and IKVStore by reference, so the whole
 * surface is reachable with two small doubles - no sockets, no gRPC, no disk.
 */

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

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

  ReadResult read_result = ReadResult::miss();
  std::string last_read_key;
  int read_calls = 0;
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

class KVHttpHandlerTest : public ::testing::Test {
protected:
  KVHttpHandlerTest() : handler_(raft_, store_) {}

  FakeRaftClient raft_;
  FakeKVStore store_;
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

} // namespace
} // namespace kvdb
