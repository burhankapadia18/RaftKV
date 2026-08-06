/**
 * @file http_request_test.cpp
 * @brief Unit tests for HttpRequestParser / HttpRequest (spec R0.12).
 *
 * The parser is hand-rolled and deliberately thin. These tests pin what it
 * ACTUALLY does today, including the sharp edges:
 *   - query parsing stops at the first segment without '=' and does no
 *     URL-decoding (Phase 4 R4.6 adds url_decode).
 * Most of this is still the "before" picture rather than a wish list.
 *
 * `request.headers` USED to be pinned as permanently empty. The auth phase
 * populates it (client credentials arrive in a header), so that assertion was
 * flipped rather than deleted — see PopulatesHeadersKeyedByLowercasedName
 * below.
 *
 * The one guarantee here that is a deliberate, already-delivered property
 * rather than an observation is totality: parse() never throws. A
 * Content-Length it cannot read is reported through
 * HttpRequest::bad_content_length, because an exception escaping this function
 * would take the whole process down (R1.8).
 */

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

#include "network/http_request.hpp"

namespace kvdb {
namespace {

/** @brief Parse and fail the test if the parser rejected the input. */
HttpRequest parse_ok(const std::string &raw) {
  std::optional<HttpRequest> parsed = HttpRequestParser::parse(raw);
  EXPECT_TRUE(parsed.has_value()) << "parser rejected the request";
  return parsed.value_or(HttpRequest{});
}

// --- Header/body framing --------------------------------------------------

TEST(HttpRequestParserTest, ReturnsNulloptWithoutBlankLineTerminator) {
  EXPECT_FALSE(HttpRequestParser::parse("").has_value());
  EXPECT_FALSE(HttpRequestParser::parse("GET /get-val HTTP/1.1\r\nHost: x\r\n")
                   .has_value());
  // Only CRLFCRLF terminates headers; a bare LF blank line is not accepted.
  EXPECT_FALSE(HttpRequestParser::parse("GET /get-val HTTP/1.1\nHost: x\n\n")
                   .has_value());
}

TEST(HttpRequestParserTest, ParsesMethodPathAndQueryString) {
  const HttpRequest request = parse_ok("GET /get-val?key=name HTTP/1.1\r\n"
                                       "Host: localhost:8080\r\n"
                                       "\r\n");

  EXPECT_EQ(request.method, "GET");
  EXPECT_EQ(request.path, "/get-val");
  EXPECT_EQ(request.query_string, "key=name");
  EXPECT_EQ(request.body, "");
}

TEST(HttpRequestParserTest, ParsesPathWithoutQueryString) {
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "\r\n");

  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.path, "/insert-val");
  EXPECT_EQ(request.query_string, "");
  EXPECT_TRUE(request.query_params().empty());
}

TEST(HttpRequestParserTest, ParsesRequestLineWithoutHttpVersion) {
  // The version token is never read, so its absence is not an error.
  const HttpRequest request = parse_ok("GET /get-val\r\n\r\n");

  EXPECT_EQ(request.method, "GET");
  EXPECT_EQ(request.path, "/get-val");
}

TEST(HttpRequestParserTest, EmptyRequestLineStillParses) {
  // CURRENT BEHAVIOR: an empty request line is not rejected, it just yields an
  // empty method and path (which then falls through to 404 in KVHttpHandler).
  const HttpRequest request = parse_ok("\r\n\r\n");

  EXPECT_EQ(request.method, "");
  EXPECT_EQ(request.path, "");
  EXPECT_EQ(request.body, "");
}

TEST(HttpRequestParserTest, PopulatesHeadersKeyedByLowercasedName) {
  // WAS PINNED EMPTY before the auth phase: parse() scanned for Content-Length
  // and Content-Type and dropped every other line, which is why no credential
  // header could reach the handler. Names are lowercased (field names are
  // case-insensitive); values keep their bytes and their case.
  const HttpRequest request = parse_ok("GET /get-val HTTP/1.1\r\n"
                                       "Host: localhost\r\n"
                                       "X-Trace-Id: AbC123\r\n"
                                       "AUTHORIZATION: Basic YWxpY2U6cHc=\r\n"
                                       "\r\n");

  EXPECT_EQ(request.headers.size(), 3u);
  EXPECT_EQ(request.header("host"), "localhost");
  EXPECT_EQ(request.header("x-trace-id"), "AbC123");
  EXPECT_EQ(request.header("authorization"), "Basic YWxpY2U6cHc=");
  // Absent headers read as empty rather than throwing or inserting.
  EXPECT_EQ(request.header("x-absent"), "");
  EXPECT_EQ(request.headers.count("x-absent"), 0u);
}

TEST(HttpRequestParserTest, TrimsPaddingAndTheTrailingCarriageReturn) {
  // Header lines keep their '\r' (std::getline splits on '\n' only). A value
  // that is USED as bytes rather than parsed as a number must not carry it: a
  // base64 credential with a trailing CR does not decode, and the failure looks
  // like a wrong password.
  const HttpRequest request =
      parse_ok("GET /get-val HTTP/1.1\r\n"
               "Authorization:  \tBasic YWxpY2U6cHc= \r\n"
               "\r\n");

  EXPECT_EQ(request.header("authorization"), "Basic YWxpY2U6cHc=");
}

TEST(HttpRequestParserTest, KeepsValueBytesVerbatimIncludingColons) {
  // Only the FIRST colon separates name from value, so a value may contain
  // colons — base64 padding, a host:port, an absolute URL.
  const HttpRequest request = parse_ok("GET /get-val HTTP/1.1\r\n"
                                       "Host: localhost:8080\r\n"
                                       "X-Url: http://example.com:9000/a\r\n"
                                       "\r\n");

  EXPECT_EQ(request.header("host"), "localhost:8080");
  EXPECT_EQ(request.header("x-url"), "http://example.com:9000/a");
}

TEST(HttpRequestParserTest, SkipsLinesThatAreNotHeaders) {
  // A line with no colon is not a header. Nothing is stored for it, and it does
  // not abort the parse of the lines around it.
  const HttpRequest request = parse_ok("GET /get-val HTTP/1.1\r\n"
                                       "garbage-without-a-colon\r\n"
                                       ": empty name\r\n"
                                       "Host: localhost\r\n"
                                       "\r\n");

  EXPECT_EQ(request.headers.size(), 1u);
  EXPECT_EQ(request.header("host"), "localhost");
}

TEST(HttpRequestParserTest, LastValueWinsForARepeatedHeader) {
  const HttpRequest request = parse_ok("GET /get-val HTTP/1.1\r\n"
                                       "X-Trace-Id: first\r\n"
                                       "X-Trace-Id: second\r\n"
                                       "\r\n");

  EXPECT_EQ(request.header("x-trace-id"), "second");
}

TEST(HttpRequestParserTest, NameMatchingIsExactNotSubstring) {
  // REGRESSION GUARD. The old parser asked whether the whole lowercased LINE
  // contained "content-length:" / "content-type:" anywhere, so a header merely
  // named like one hijacked both flags: X-Content-Type below used to set
  // is_msgpack, and X-Content-Length used to set the body length. A field name
  // is the text before the first colon and nothing else.
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "X-Content-Type: application/msgpack\r\n"
                                       "X-Content-Length: 99\r\n"
                                       "\r\n");

  EXPECT_FALSE(request.is_msgpack);
  EXPECT_EQ(request.content_length, 0);
  EXPECT_FALSE(request.bad_content_length);
  // They are still captured under their own names.
  EXPECT_EQ(request.header("x-content-length"), "99");
}

TEST(HttpRequestParserTest, BodyIsEverythingAfterTheBlankLine) {
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "Content-Length: 11\r\n"
                                       "\r\n"
                                       "hello=world");

  EXPECT_EQ(request.body, "hello=world");
  EXPECT_EQ(request.content_length, 11);
}

TEST(HttpRequestParserTest, BinaryBodyIsPreservedByteForByte) {
  // MsgPack bodies contain NUL bytes; the parser is length-based, not
  // NUL-terminated, so they survive.
  const std::string body("\x82\x00\xff\x0a", 4);
  const HttpRequest request =
      parse_ok(std::string("POST /insert-val HTTP/1.1\r\n"
                           "Content-Type: application/msgpack\r\n"
                           "Content-Length: 4\r\n"
                           "\r\n") +
               body);

  EXPECT_EQ(request.body, body);
  EXPECT_EQ(request.body.size(), 4u);
  EXPECT_TRUE(request.is_msgpack);
}

TEST(HttpRequestParserTest, BodyShorterThanContentLengthStillParses) {
  // The parser never enforces Content-Length - HttpServer::handle_connection
  // is what tops the body up with further recv() calls. parse() succeeds with
  // whatever arrived in the first packet.
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "Content-Length: 10\r\n"
                                       "\r\n"
                                       "abc");

  EXPECT_EQ(request.content_length, 10);
  EXPECT_EQ(request.body, "abc");
  EXPECT_LT(request.body.size(), static_cast<size_t>(request.content_length));
}

// --- Content-Length -------------------------------------------------------

TEST(HttpRequestParserTest, MissingContentLengthDefaultsToZero) {
  const HttpRequest request = parse_ok("GET /get-val?key=name HTTP/1.1\r\n"
                                       "Host: localhost\r\n"
                                       "\r\n");

  EXPECT_EQ(request.content_length, 0);
  // Absent is not malformed: nothing to parse, nothing to complain about.
  EXPECT_FALSE(request.bad_content_length);
}

TEST(HttpRequestParserTest, ContentLengthIsCaseInsensitiveAndIgnoresPadding) {
  // The whole line is lowercased before matching, and std::stoi skips the
  // leading space (and stops at the trailing '\r' when there is one - header
  // lines keep their CR because std::getline only splits on '\n').
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "CONTENT-LENGTH: 7\r\n"
                                       "Host: localhost\r\n"
                                       "\r\n"
                                       "1234567");

  EXPECT_EQ(request.content_length, 7);
  EXPECT_FALSE(request.bad_content_length);
}

TEST(HttpRequestParserTest, GarbageContentLengthIsFlaggedNotThrown) {
  // GUARANTEE (R1.8): parse() is total. A Content-Length it cannot read sets
  // bad_content_length, leaves content_length at 0 and still returns a request
  // — KVHttpHandler is what turns the flag into 400 {"error":"malformed
  // Content-Length"}.
  //
  // This must not regress into an exception. It used to be one, and it was a
  // remote kill switch rather than a dropped request: std::stoi threw out of
  // parse(), unwound through HttpServer::handle_connection (leaking the client
  // fd) and out of run() into main.cpp's `catch (const std::exception&)`, which
  // printed "Fatal error: stoi" and returned 1; entrypoint.sh's `wait -n` then
  // took the container down. Verified against a live cluster: one
  // unauthenticated `Content-Length: abc` moved a node to Exited(1).
  const HttpRequest garbage = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "Content-Length: abc\r\n"
                                       "\r\n");
  EXPECT_TRUE(garbage.bad_content_length);
  EXPECT_EQ(garbage.content_length, 0);
  // The rest of the request is still parsed, so the handler has enough to
  // answer with a real status line instead of hanging up.
  EXPECT_EQ(garbage.method, "POST");
  EXPECT_EQ(garbage.path, "/insert-val");

  // An empty value is flagged the same way (std::invalid_argument).
  const HttpRequest empty = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                     "Content-Length:\r\n"
                                     "\r\n");
  EXPECT_TRUE(empty.bad_content_length);
  EXPECT_EQ(empty.content_length, 0);

  // ...and so is a value too wide for an int, which is the other throw
  // std::stoi can produce (std::out_of_range).
  const HttpRequest huge = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                    "Content-Length: 99999999999999999999\r\n"
                                    "\r\n");
  EXPECT_TRUE(huge.bad_content_length);
  EXPECT_EQ(huge.content_length, 0);
}

TEST(HttpRequestParserTest, NegativeContentLengthIsFlagged) {
  // A negative length does NOT throw — std::stoi("-1") happily returns -1 — so
  // the exception handling above does not cover it. It has to be rejected
  // explicitly, and the reason is severe:
  //
  // HttpServer::handle_connection tops the body up with
  //   while (body.size() < static_cast<size_t>(content_length))
  // and static_cast<size_t>(-1) is 18446744073709551615. The loop can never be
  // satisfied, so it blocks in recv() until the peer disconnects. The accept
  // loop is single-threaded, so ONE client that sends this header and keeps the
  // socket open takes the node's whole HTTP surface offline — while the
  // container still reports healthy, so nothing restarts it.
  //
  // Verified against a live cluster before the guard existed: node3 stopped
  // answering every request for as long as the socket was held, reported
  // "Up 10 minutes" throughout, and recovered only when the client hung up.
  for (const char *value : {"-1", "-1000", "-2147483648"}) {
    const std::string raw = std::string("POST /insert-val HTTP/1.1\r\n"
                                        "Content-Length: ") +
                            value + "\r\n\r\n";
    const HttpRequest request = parse_ok(raw);
    EXPECT_TRUE(request.bad_content_length) << "value=" << value;
    EXPECT_EQ(request.content_length, 0) << "value=" << value;
  }
}

TEST(HttpRequestParserTest, RepeatedContentLengthIsFlagged) {
  // Two Content-Length headers mean the sender and this server may disagree
  // about where the body ends — the shape of a request-smuggling attempt. RFC
  // 9110 says reject; picking one is exactly the guess that lets a proxy and an
  // origin frame the same bytes differently. Note this is NOT the general
  // last-wins rule the header map uses: acting on a length demands agreement.
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "Content-Length: 3\r\n"
                                       "Content-Length: 11\r\n"
                                       "\r\n"
                                       "abc");

  EXPECT_TRUE(request.bad_content_length);
  EXPECT_EQ(request.content_length, 0);
}

// --- Content-Type ---------------------------------------------------------

TEST(HttpRequestParserTest, DetectsMsgpackContentType) {
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "Content-Type: application/msgpack\r\n"
                                       "\r\n");

  EXPECT_TRUE(request.is_msgpack);
}

TEST(HttpRequestParserTest, MsgpackDetectionIsCaseInsensitive) {
  // The field name is lowercased before lookup and the value is lowercased
  // before the media-type test, so both are matched case-insensitively.
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "CONTENT-TYPE: Application/MsgPack\r\n"
                                       "\r\n");

  EXPECT_TRUE(request.is_msgpack);
}

TEST(HttpRequestParserTest, MsgpackDetectionAllowsMediaTypeParameters) {
  // A parameterized media type still qualifies, which is why the value test is
  // a substring match — tests/e2e/contracts.py depends on this.
  const HttpRequest request =
      parse_ok("POST /insert-val HTTP/1.1\r\n"
               "Content-Type: application/msgpack; charset=binary\r\n"
               "\r\n");

  EXPECT_TRUE(request.is_msgpack);
}

TEST(HttpRequestParserTest, OtherContentTypesAreNotMsgpack) {
  const HttpRequest json = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                    "Content-Type: application/json\r\n"
                                    "\r\n");
  EXPECT_FALSE(json.is_msgpack);

  const HttpRequest none = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                    "Host: localhost\r\n"
                                    "\r\n");
  EXPECT_FALSE(none.is_msgpack);

  // The media type is read from the Content-Type header only, so an Accept
  // header naming msgpack does not flip the flag.
  const HttpRequest accept_only = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                           "Accept: application/msgpack\r\n"
                                           "\r\n");
  EXPECT_FALSE(accept_only.is_msgpack);
}

// --- query_params() -------------------------------------------------------

TEST(HttpRequestTest, QueryParamsEmptyQueryString) {
  HttpRequest request;
  request.query_string = "";

  EXPECT_TRUE(request.query_params().empty());
}

TEST(HttpRequestTest, QueryParamsSingleParameter) {
  HttpRequest request;
  request.query_string = "key=name";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.at("key"), "name");
}

TEST(HttpRequestTest, QueryParamsMultipleParameters) {
  HttpRequest request;
  request.query_string = "key=name&limit=10&debug=true";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 3u);
  EXPECT_EQ(params.at("key"), "name");
  EXPECT_EQ(params.at("limit"), "10");
  EXPECT_EQ(params.at("debug"), "true");
}

TEST(HttpRequestTest, QueryParamsEmptyValueAndTrailingAmpersand) {
  HttpRequest request;
  request.query_string = "key=&limit=10&";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 2u);
  EXPECT_EQ(params.at("key"), "");
  EXPECT_EQ(params.at("limit"), "10");
}

TEST(HttpRequestTest, QueryParamsValueMayContainEquals) {
  HttpRequest request;
  request.query_string = "key=a=b";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.at("key"), "a=b"); // split at the first '=' only
}

TEST(HttpRequestTest, QueryParamsStopsAtTrailingSegmentWithoutEquals) {
  // CURRENT BEHAVIOR, pinned: the loop breaks as soon as it cannot find another
  // '=', so a valueless trailing segment is dropped silently.
  HttpRequest request;
  request.query_string = "key=name&flag";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.at("key"), "name");
  EXPECT_EQ(params.count("flag"), 0u);
}

TEST(HttpRequestTest, QueryParamsSwallowsAmpersandIntoKeyOfLeadingBareSegment) {
  // CURRENT BEHAVIOR, pinned: the key is taken from the segment start to the
  // next '=' without ever looking for an intervening '&', so a leading
  // valueless segment is glued onto the following key.
  HttpRequest request;
  request.query_string = "flag&key=name";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.count("key"), 0u);
  EXPECT_EQ(params.at("flag&key"), "name");
}

TEST(HttpRequestTest, QueryParamsDoesNotUrlDecode) {
  // CURRENT BEHAVIOR, pinned: percent-escapes and '+' are passed through raw,
  // so "hello world" is not reachable via the HTTP API today. Phase 4 (R4.6)
  // adds url_decode.
  HttpRequest request;
  request.query_string = "key=hello%20world";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.at("key"), "hello%20world");
}

TEST(HttpRequestTest, QueryParamsLastValueWinsForDuplicateKeys) {
  HttpRequest request;
  request.query_string = "key=first&key=second";

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.at("key"), "second");
}

TEST(HttpRequestParserTest, QueryParamsFromAParsedRequest) {
  const HttpRequest request =
      parse_ok("GET /get-val?key=user%3A1&mode=local HTTP/1.1\r\n"
               "\r\n");

  const std::map<std::string, std::string> params = request.query_params();

  ASSERT_EQ(params.size(), 2u);
  EXPECT_EQ(params.at("key"), "user%3A1");
  EXPECT_EQ(params.at("mode"), "local");
}

// --- R4.6: url_decode -----------------------------------------------------

TEST(UrlDecodeTest, LeavesUnreservedCharactersAlone) {
  const std::optional<std::string> out = url_decode("plain-key_123.txt");
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(*out, "plain-key_123.txt");
}

TEST(UrlDecodeTest, DecodesPercentEscapes) {
  EXPECT_EQ(url_decode("hello%20world").value_or("<none>"), "hello world");
  // A key containing a slash must survive: without decoding it would look like
  // a nested path and address a different key.
  EXPECT_EQ(url_decode("a%2Fb").value_or("<none>"), "a/b");
  EXPECT_EQ(url_decode("%3D").value_or("<none>"), "=");
  // Lower and upper case hex digits are both legal.
  EXPECT_EQ(url_decode("%7e").value_or("<none>"), "~");
  EXPECT_EQ(url_decode("%7E").value_or("<none>"), "~");
}

TEST(UrlDecodeTest, DecodesNulAndHighBytes) {
  // Values and keys have been binary-safe since Phase 2; the URL layer must not
  // be the thing that reintroduces a truncation.
  const std::optional<std::string> nul = url_decode("a%00b");
  ASSERT_TRUE(nul.has_value());
  EXPECT_EQ(nul->size(), 3u);
  EXPECT_EQ(*nul, std::string("a\0b", 3));

  const std::optional<std::string> high = url_decode("%FF%FE");
  ASSERT_TRUE(high.has_value());
  EXPECT_EQ(high->size(), 2u);
  EXPECT_EQ(static_cast<unsigned char>((*high)[0]), 0xFFu);
}

TEST(UrlDecodeTest, RejectsMalformedEscapesRatherThanGuessing) {
  // Passing these through as literal text would let "%zz" and a genuinely
  // percent-encoded key collide, so they are refused and the handler answers
  // 400.
  EXPECT_FALSE(url_decode("%zz").has_value());
  EXPECT_FALSE(url_decode("%4").has_value());  // truncated
  EXPECT_FALSE(url_decode("%").has_value());   // bare percent
  EXPECT_FALSE(url_decode("ok%").has_value()); // trailing
  EXPECT_FALSE(url_decode("a%2").has_value()); // one hex digit short
  EXPECT_FALSE(url_decode("%g0").has_value());
}

TEST(UrlDecodeTest, PlusIsLiteralInPathsAndSpaceInQueries) {
  // The distinction is real: /kv/a+b and /kv/a%20b are DIFFERENT keys, while in
  // a query string "a+b" conventionally means "a b".
  EXPECT_EQ(url_decode("a+b", false).value_or("<none>"), "a+b");
  EXPECT_EQ(url_decode("a+b", true).value_or("<none>"), "a b");
}

TEST(UrlDecodeTest, EmptyInputDecodesToEmpty) {
  const std::optional<std::string> out = url_decode("");
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->empty());
}

} // namespace
} // namespace kvdb
