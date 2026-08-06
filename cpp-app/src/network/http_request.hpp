#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace kvdb {

/**
 * @brief Percent-decode a URL component (R4.6).
 *
 * Returns nullopt on a malformed escape rather than guessing: "%zz" and a
 * trailing "%4" are not "probably fine", they mean the client and this server
 * disagree about the bytes being named, and silently keeping the literal text
 * would let two different requests address the same key.
 *
 * @param in           The raw component.
 * @param decode_plus  Treat '+' as a space. True for query strings (where
 *                     application/x-www-form-urlencoded says so), false for
 *                     path segments (where '+' is a literal plus). Getting this
 *                     backwards silently corrupts any key containing a '+'.
 * @return The decoded bytes, or nullopt if @p in contains a malformed escape.
 */
[[nodiscard]] inline std::optional<std::string>
url_decode(const std::string &in, bool decode_plus = false) {
  static constexpr auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };

  std::string out;
  out.reserve(in.size());

  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '+' && decode_plus) {
      out += ' ';
      continue;
    }
    if (c != '%') {
      out += c;
      continue;
    }

    // Needs exactly two more characters, both hex. Checking the length first is
    // what keeps this from reading past the end on a trailing "%".
    if (i + 2 >= in.size()) {
      return std::nullopt;
    }
    const int hi = hex_value(in[i + 1]);
    const int lo = hex_value(in[i + 2]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out += static_cast<char>((hi << 4) | lo);
    i += 2;
  }

  return out;
}

/**
 * @brief Lowercase an ASCII string, for case-insensitive header matching.
 *
 * The cast through unsigned char is not decoration: std::tolower takes an int
 * that must be representable as unsigned char, and passing a negative value —
 * which any byte >= 0x80 becomes on a platform with signed char — is undefined
 * behaviour. Header values carry arbitrary bytes, and this build runs under
 * UBSan in CI.
 */
[[nodiscard]] inline std::string ascii_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return text;
}

/**
 * @brief Strip leading and trailing optional whitespace from a header value.
 *
 * RFC 9110 allows SP/HTAB around a field value. The trailing CR is stripped
 * here too, because the parser splits header lines with std::getline on '\n'
 * and every line therefore keeps its '\r'. Leaving it attached would corrupt
 * any value used as bytes rather than parsed as a number — a base64
 * Authorization credential with a trailing CR does not decode.
 */
[[nodiscard]] inline std::string trim_header_value(const std::string &value) {
  const auto is_padding = [](char c) {
    return c == ' ' || c == '\t' || c == '\r';
  };

  size_t begin = 0;
  while (begin < value.size() && is_padding(value[begin])) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin && is_padding(value[end - 1])) {
    --end;
  }
  return value.substr(begin, end - begin);
}

/**
 * @brief Parsed HTTP request structure.
 *
 * Immutable value object representing a parsed HTTP request.
 */
struct HttpRequest {
  std::string method;
  std::string path;
  std::string query_string;

  /**
   * @brief Every header line, keyed by LOWERCASED field name.
   *
   * Values keep their original bytes and case — a base64 credential and a
   * quoted ETag both mean different things after case folding. Match names
   * against a lowercase literal, or go through header() below.
   *
   * Populated since the auth phase. It was declared but never filled in
   * before, which is why response content negotiation and any credential
   * header were impossible; @c is_msgpack and @c content_length are now
   * derived from this map rather than scanned for independently.
   */
  std::map<std::string, std::string> headers;
  std::string body;
  bool is_msgpack = false;
  int content_length = 0;

  /**
   * @brief Set when a Content-Length header was present but unparseable.
   *
   * @c content_length stays 0 in that case. The handler answers 400 rather
   * than guessing at a body length.
   */
  bool bad_content_length = false;

  /**
   * @brief One header value, or "" when absent.
   * @param lowercase_name Field name in lowercase — the map's key form.
   */
  [[nodiscard]] std::string header(const std::string &lowercase_name) const {
    const auto it = headers.find(lowercase_name);
    return it == headers.end() ? std::string() : it->second;
  }

  /**
   * @brief Parse query parameters from the query string.
   * @return Map of key-value pairs from the query string
   */
  [[nodiscard]] std::map<std::string, std::string> query_params() const {
    std::map<std::string, std::string> params;
    size_t start = 0;

    while (start < query_string.size()) {
      size_t eq_pos = query_string.find('=', start);
      if (eq_pos == std::string::npos)
        break;

      size_t amp_pos = query_string.find('&', eq_pos);
      std::string key = query_string.substr(start, eq_pos - start);
      std::string value = query_string.substr(
          eq_pos + 1,
          (amp_pos == std::string::npos ? query_string.size() : amp_pos) -
              (eq_pos + 1));

      params[key] = value;
      start = (amp_pos == std::string::npos) ? std::string::npos : amp_pos + 1;
    }

    return params;
  }
};

/**
 * @brief HTTP request parser.
 *
 * Parses raw HTTP request data into an HttpRequest structure.
 * Handles header parsing, body extraction, and query string separation.
 */
class HttpRequestParser {
public:
  /**
   * @brief Parse a raw HTTP request string.
   *
   * @param raw_request The complete HTTP request as a string
   * @return Parsed HttpRequest object, or nullopt if parsing fails
   */
  [[nodiscard]] static std::optional<HttpRequest>
  parse(const std::string &raw_request) {
    HttpRequest request;

    // Find header/body boundary
    size_t header_end = raw_request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      return std::nullopt;
    }

    std::string headers = raw_request.substr(0, header_end);
    request.body = raw_request.substr(header_end + 4);

    // Parse request line (method and path)
    std::istringstream header_stream(headers);
    std::string full_path;
    header_stream >> request.method >> full_path;

    // Separate path from query string
    size_t query_pos = full_path.find('?');
    if (query_pos != std::string::npos) {
      request.path = full_path.substr(0, query_pos);
      request.query_string = full_path.substr(query_pos + 1);
    } else {
      request.path = full_path;
    }

    // Parse headers into request.headers, keyed by lowercased field name.
    //
    // Names are matched EXACTLY after lowercasing. The previous version asked
    // whether the whole line contained "content-length:" anywhere, which also
    // matched a header merely NAMED like one — `X-Content-Type:
    // application/msgpack` flipped is_msgpack, and any `...-Content-Length:`
    // set the body length. A field name is the text before the first colon and
    // nothing else.
    std::string line;
    std::getline(header_stream, line); // Skip first line (already parsed)

    size_t content_length_count = 0;

    while (std::getline(header_stream, line)) {
      const size_t colon = line.find(':');
      if (colon == std::string::npos) {
        // Not a header line (a bare CR from the terminator, or a continuation
        // line — obs-fold is deprecated and not supported). Nothing to store.
        continue;
      }

      const std::string name = ascii_lower(line.substr(0, colon));
      if (name.empty()) {
        continue;
      }
      const std::string value = trim_header_value(line.substr(colon + 1));

      if (name == "content-length") {
        ++content_length_count;
      }
      // Last occurrence wins, which is what the old scan effectively did too.
      request.headers[name] = value;
    }

    // Content-Length: a header the server acts on, so it is derived here rather
    // than left for each caller to re-parse.
    const auto content_length = request.headers.find("content-length");
    if (content_length != request.headers.end()) {
      // std::stoi throws on garbage ("abc"), on an empty value and on
      // anything wider than an int. parse() must stay total: an exception
      // here would unwind out of the accept loop and take the process down,
      // which makes a single unauthenticated header a remote kill switch.
      try {
        request.content_length = std::stoi(content_length->second);
      } catch (const std::invalid_argument &) {
        request.bad_content_length = true;
        request.content_length = 0;
      } catch (const std::out_of_range &) {
        request.bad_content_length = true;
        request.content_length = 0;
      }
      // A NEGATIVE length parses fine — std::stoi("-1") just returns -1 — so
      // catching exceptions alone is not enough. HttpServer compares
      // body.size() against static_cast<size_t>(content_length), and
      // (size_t)-1 is 18446744073709551615: the body top-up loop would never
      // be satisfied and would block in recv() forever. Because the accept
      // loop is single-threaded, one client holding that socket open wedges
      // the node's entire HTTP surface while the container still reports
      // healthy. Verified against a live cluster before this guard existed.
      if (request.content_length < 0) {
        request.bad_content_length = true;
        request.content_length = 0;
      }
      // Two Content-Length headers mean the sender and this server may not
      // agree on where the body ends, which is the shape of a request
      // smuggling attempt. RFC 9110 says reject; "last one wins" is exactly
      // the guess that makes a proxy and an origin disagree.
      if (content_length_count > 1) {
        request.bad_content_length = true;
        request.content_length = 0;
      }
    }

    // The media type may carry parameters ("application/msgpack; charset=..."),
    // so this stays a substring test — but only within the Content-Type VALUE.
    request.is_msgpack = ascii_lower(request.header("content-type"))
                             .find("application/msgpack") != std::string::npos;

    return request;
  }
};

} // namespace kvdb
