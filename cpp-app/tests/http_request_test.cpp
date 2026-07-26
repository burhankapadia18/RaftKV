/**
 * @file http_request_test.cpp
 * @brief Unit tests for HttpRequestParser / HttpRequest (spec R0.12).
 *
 * The parser is hand-rolled and deliberately thin. These tests pin what it
 * ACTUALLY does today, including the sharp edges:
 *   - `request.headers` is declared but never filled in.
 *   - a garbage Content-Length propagates a std::invalid_argument out of
 *     parse(), which no handler catches short of main()'s fatal catch-all —
 *     i.e. it kills the whole node (Phase 1 R1.8 wraps the stoi).
 *   - query parsing stops at the first segment without '=' and does no
 *     URL-decoding (Phase 4 R4.6 adds url_decode).
 * Nothing here is a wish list; it is the "before" picture.
 */

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <stdexcept>
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

TEST(HttpRequestParserTest, HeadersMapIsNeverPopulated) {
  // CURRENT BEHAVIOR: parse() scans header lines only for Content-Length and
  // Content-Type; HttpRequest::headers stays empty no matter what is sent.
  const HttpRequest request = parse_ok("GET /get-val HTTP/1.1\r\n"
                                       "Host: localhost\r\n"
                                       "X-Trace-Id: abc123\r\n"
                                       "\r\n");

  EXPECT_TRUE(request.headers.empty());
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
}

TEST(HttpRequestParserTest, GarbageContentLengthThrowsOutOfParse) {
  // CURRENT BEHAVIOR (bug, pinned) — REMOTE DENIAL OF SERVICE, not just a
  // dropped request. std::stoi throws and parse() does not catch it, so the
  // exception unwinds through HttpServer::handle_connection (leaking the
  // client fd) and out of run() into main.cpp's `catch (const
  // std::exception&)`, which prints "Fatal error: stoi" and returns 1.
  // entrypoint.sh's `wait -n` then takes the container down. Verified against a
  // live cluster: one unauthenticated `Content-Length: abc` moved a node to
  // Exited(1).
  //
  // Phase 1 R1.8 owns the fix ("malformed request (bad Content-Length, empty
  // body) -> 400 - and std::stoi is wrapped so garbage no longer crashes the
  // server"). NOT Phase 4 R4.9, which is only body/header size caps.
  // When R1.8 lands, this EXPECT_THROW becomes an assertion on a 400.
  EXPECT_THROW(HttpRequestParser::parse("POST /insert-val HTTP/1.1\r\n"
                                        "Content-Length: abc\r\n"
                                        "\r\n"),
               std::invalid_argument);

  // An empty value throws the same way.
  EXPECT_THROW(HttpRequestParser::parse("POST /insert-val HTTP/1.1\r\n"
                                        "Content-Length:\r\n"
                                        "\r\n"),
               std::invalid_argument);
}

// --- Content-Type ---------------------------------------------------------

TEST(HttpRequestParserTest, DetectsMsgpackContentType) {
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "Content-Type: application/msgpack\r\n"
                                       "\r\n");

  EXPECT_TRUE(request.is_msgpack);
}

TEST(HttpRequestParserTest, MsgpackDetectionIsCaseInsensitive) {
  // The entire header line is lowercased, so both the name and the value are
  // matched case-insensitively.
  const HttpRequest request = parse_ok("POST /insert-val HTTP/1.1\r\n"
                                       "CONTENT-TYPE: Application/MsgPack\r\n"
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

  // The match requires both substrings on the SAME line, so an Accept header
  // alone does not flip the flag.
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

} // namespace
} // namespace kvdb
