#pragma once

#include <algorithm>
#include <map>
#include <sstream>
#include <string>

namespace kvdb {

/**
 * @brief Parsed HTTP request structure.
 *
 * Immutable value object representing a parsed HTTP request.
 */
struct HttpRequest {
  std::string method;
  std::string path;
  std::string query_string;
  std::map<std::string, std::string> headers;
  std::string body;
  bool is_msgpack = false;
  int content_length = 0;

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

    // Parse headers
    std::string line;
    std::getline(header_stream, line); // Skip first line (already parsed)

    while (std::getline(header_stream, line)) {
      // Convert to lowercase for case-insensitive matching
      std::string lower_line = line;
      std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(),
                     ::tolower);

      if (lower_line.find("content-length:") != std::string::npos) {
        size_t colon = line.find(':');
        request.content_length = std::stoi(line.substr(colon + 1));
      }

      if (lower_line.find("content-type:") != std::string::npos &&
          lower_line.find("application/msgpack") != std::string::npos) {
        request.is_msgpack = true;
      }
    }

    return request;
  }
};

} // namespace kvdb
