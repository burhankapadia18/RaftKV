#pragma once

#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../raft/raft_client.hpp"
#include "../storage/kv_store.hpp"
#include "http_request.hpp"

namespace kvdb {

/** @brief Content-Type of a successful read: the stored bytes, verbatim. */
inline constexpr const char *kTextContentType = "text/plain; charset=utf-8";

/** @brief Content-Type of every structured body this server emits. */
inline constexpr const char *kJsonContentType = "application/json";

/** @brief The only request media type POST /insert-val accepts. */
inline constexpr const char *kMsgpackContentType = "application/msgpack";

/**
 * @brief Machine-readable prefix on a propose that failed because this node
 * is not the leader, followed by the raft leader's address.
 *
 * The address is empty during an election ("not_leader:"). The sidecar
 * (go-sidecar/internal/rpc) produces it and Phase 4 leader forwarding keys off
 * this exact prefix, so it is a contract, not a log message.
 */
inline constexpr const char *kNotLeaderPrefix = "not_leader:";

/**
 * @brief Escape a string so it can be embedded in a JSON string literal.
 *
 * Deliberately tiny - no JSON library is linked into this binary. It covers
 * everything RFC 8259 forbids raw inside a string: the two delimiters ('"' and
 * '\\') and the C0 control characters, which fall back to \\u00XX. Bytes >=
 * 0x20 pass through untouched, so UTF-8 payloads survive.
 *
 * Every error body goes through this: the propose error and the raft leader
 * address are both attacker-influenced in principle, and an unescaped quote
 * would produce a body no client can parse.
 */
[[nodiscard]] inline std::string json_escape(const std::string &input) {
  static constexpr char kHexDigits[] = "0123456789abcdef";

  std::string escaped;
  escaped.reserve(input.size());

  for (const char c : input) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (byte < 0x20) {
        escaped += "\\u00";
        escaped += kHexDigits[(byte >> 4) & 0x0f];
        escaped += kHexDigits[byte & 0x0f];
      } else {
        escaped += c;
      }
      break;
    }
  }

  return escaped;
}

/**
 * @brief HTTP response builder utility.
 */
struct HttpResponse {
  int status_code = 200;
  std::string body;
  std::string content_type = kTextContentType;

  /**
   * @brief Reason phrase for a status code (RFC 9110 section 15).
   *
   * Only the codes this server actually emits are listed; anything else is a
   * programming error, so the default is deliberately bland rather than a
   * guess.
   */
  [[nodiscard]] static const char *reason_phrase(int status_code) {
    switch (status_code) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 415:
      return "Unsupported Media Type";
    case 500:
      return "Internal Server Error";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
    }
  }

  /**
   * @brief Serialize the response to HTTP format.
   */
  [[nodiscard]] std::string to_string() const {
    std::string response = "HTTP/1.1 ";
    response += std::to_string(status_code);
    response += " ";
    response += reason_phrase(status_code);
    response += "\r\nContent-Type: ";
    response += content_type;
    response += "\r\nContent-Length: ";
    response += std::to_string(body.size());
    response += "\r\n\r\n";
    response += body;
    return response;
  }

  /** @brief 200 carrying a raw stored value as plain text. */
  [[nodiscard]] static HttpResponse ok(const std::string &body) {
    return HttpResponse{200, body, kTextContentType};
  }

  /** @brief A JSON body with an explicit status code. */
  [[nodiscard]] static HttpResponse json(int status_code,
                                         const std::string &body) {
    return HttpResponse{status_code, body, kJsonContentType};
  }

  /** @brief The standard error envelope: {"error":"<escaped message>"}. */
  [[nodiscard]] static HttpResponse json_error(int status_code,
                                               const std::string &message) {
    return json(status_code, "{\"error\":\"" + json_escape(message) + "\"}");
  }
};

/**
 * @brief HTTP request handler for the KV store API.
 *
 * Implements the business logic for handling HTTP requests.
 * Separates routing and request handling from socket management.
 */
class KVHttpHandler {
public:
  /**
   * @brief Construct the handler with dependencies.
   * @param raft_client Client for proposing commands to Raft
   * @param store Reference to the key-value store for reads
   */
  KVHttpHandler(IRaftClient &raft_client, const IKVStore &store)
      : raft_client_(raft_client), store_(store) {}

  /**
   * @brief Handle an HTTP request and return a response.
   *
   * Routing is on method and path only. The media type is checked inside
   * handle_insert so that a POST to /insert-val with the wrong Content-Type
   * gets 415 instead of silently becoming "no such route".
   */
  [[nodiscard]] HttpResponse handle(const HttpRequest &request) const {
    if (request.method == "POST" && request.path == "/insert-val") {
      return handle_insert(request);
    }
    if (request.method == "GET" && request.path == "/get-val") {
      return handle_get(request);
    }
    return HttpResponse::json_error(404, "not found");
  }

private:
  IRaftClient &raft_client_;
  const IKVStore &store_;

  /**
   * @brief POST /insert-val - validate, then propose through Raft.
   *
   * Framing is checked before semantics: a Content-Length the parser could not
   * read means the request itself is malformed (400), and there is no point
   * arguing about its media type.
   */
  [[nodiscard]] HttpResponse handle_insert(const HttpRequest &request) const {
    if (request.bad_content_length) {
      return HttpResponse::json_error(400, "malformed Content-Length");
    }
    if (!request.is_msgpack) {
      std::string body = "{\"error\":\"unsupported media type\",";
      body += "\"expected\":\"";
      body += kMsgpackContentType;
      body += "\"}";
      return HttpResponse::json(415, body);
    }
    if (request.body.empty()) {
      return HttpResponse::json_error(400, "empty request body");
    }

    const ProposeResult result = raft_client_.propose(request.body);
    if (result.success) {
      return HttpResponse::json(200, "{\"ok\":true}");
    }

    // Not-the-leader is the one failure a client can act on by itself, so it
    // gets a retryable status and the address to retry against.
    const std::optional<std::string> leader = not_leader_address(result.error);
    if (leader) {
      std::string body = "{\"error\":\"not leader\",\"leader\":\"";
      body += json_escape(*leader);
      body += "\"}";
      return HttpResponse::json(503, body);
    }
    return HttpResponse::json_error(502, result.error);
  }

  /**
   * @brief GET /get-val?key=... - served from the local store, no consensus.
   */
  [[nodiscard]] HttpResponse handle_get(const HttpRequest &request) const {
    const std::map<std::string, std::string> params = request.query_params();
    const auto it = params.find("key");
    if (it == params.end()) {
      const std::string message = "missing required query parameter: key";
      return HttpResponse::json_error(400, message);
    }

    const std::optional<std::string> value = store_.get(it->second);
    if (!value) {
      return HttpResponse::json_error(404, "key not found");
    }
    return HttpResponse::ok(*value);
  }

  /**
   * @brief Extract the leader address from a "not_leader:<address>" error.
   * @param error The error string returned by a failed propose
   * @return The address (possibly empty) if the prefix matched, else nullopt
   */
  [[nodiscard]] static std::optional<std::string>
  not_leader_address(const std::string &error) {
    const std::string prefix(kNotLeaderPrefix);
    if (error.rfind(prefix, 0) != 0) {
      return std::nullopt;
    }
    return error.substr(prefix.size());
  }
};

/**
 * @brief TCP socket-based HTTP server.
 *
 * Handles low-level socket operations and delegates request
 * handling to KVHttpHandler. Follows Single Responsibility Principle.
 */
class HttpServer {
public:
  /**
   * @brief Construct the HTTP server.
   * @param port Port to listen on
   * @param handler Request handler for processing requests
   */
  HttpServer(int port, KVHttpHandler handler)
      : port_(port), handler_(std::move(handler)) {
    setup_socket();
  }

  ~HttpServer() {
    if (server_fd_ >= 0) {
      close(server_fd_);
    }
  }

  // Non-copyable
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  /**
   * @brief Start the server and run the accept loop.
   *
   * This method blocks indefinitely, accepting and handling connections.
   */
  void run() {
    std::cout << "[HTTP] Server listening on port " << port_ << std::endl;

    while (true) {
      int client_socket = accept(server_fd_, nullptr, nullptr);
      if (client_socket >= 0) {
        handle_connection(client_socket);
      }
    }
  }

private:
  int port_;
  int server_fd_ = -1;
  KVHttpHandler handler_;

  static constexpr size_t kBufferSize = 4096;

  void setup_socket() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      throw std::runtime_error("Failed to create socket");
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0) {
      throw std::runtime_error("Failed to bind socket");
    }

    if (listen(server_fd_, 10) < 0) {
      throw std::runtime_error("Failed to listen on socket");
    }
  }

  void handle_connection(int client_socket) {
    std::vector<char> buffer(kBufferSize);

    int bytes_received = recv(client_socket, buffer.data(), buffer.size(), 0);
    if (bytes_received <= 0) {
      close(client_socket);
      return;
    }

    std::string raw_request(buffer.data(), bytes_received);

    auto parsed_request = HttpRequestParser::parse(raw_request);
    if (!parsed_request) {
      close(client_socket);
      return;
    }

    // Read remaining body if needed
    while (parsed_request->body.size() <
           static_cast<size_t>(parsed_request->content_length)) {
      int n = recv(client_socket, buffer.data(), buffer.size(), 0);
      if (n <= 0)
        break;
      parsed_request->body.append(buffer.data(), n);
    }

    // Handle request and send response
    HttpResponse response = handler_.handle(*parsed_request);
    std::string response_str = response.to_string();
    send(client_socket, response_str.c_str(), response_str.size(), 0);

    close(client_socket);
  }
};

} // namespace kvdb
