#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../raft/raft_client.hpp"
#include "../storage/kv_store.hpp"
#include "http_request.hpp"

namespace kvdb {

/**
 * @brief HTTP response builder utility.
 */
struct HttpResponse {
  int status_code = 200;
  std::string body;

  /**
   * @brief Serialize the response to HTTP format.
   */
  [[nodiscard]] std::string to_string() const {
    return "HTTP/1.1 " + std::to_string(status_code) +
           " OK\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
  }

  static HttpResponse ok(const std::string &body) {
    return HttpResponse{200, body};
  }

  static HttpResponse not_found(const std::string &body = "404 Not Found") {
    return HttpResponse{404, body};
  }

  static HttpResponse error(const std::string &body = "Internal Server Error") {
    return HttpResponse{500, body};
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
   */
  [[nodiscard]] HttpResponse handle(const HttpRequest &request) const {
    if (request.method == "POST" && request.path == "/insert-val" &&
        request.is_msgpack) {
      return handle_insert(request);
    } else if (request.method == "GET" && request.path == "/get-val") {
      return handle_get(request);
    }
    return HttpResponse::not_found();
  }

private:
  IRaftClient &raft_client_;
  const IKVStore &store_;

  [[nodiscard]] HttpResponse handle_insert(const HttpRequest &request) const {
    bool success = raft_client_.propose(request.body);
    return HttpResponse::ok(success ? "ok" : "error");
  }

  [[nodiscard]] HttpResponse handle_get(const HttpRequest &request) const {
    auto params = request.query_params();
    auto it = params.find("key");
    if (it == params.end()) {
      return HttpResponse::ok("Key Not Found");
    }

    auto value = store_.get(it->second);
    if (value) {
      return HttpResponse::ok(*value);
    }
    return HttpResponse::ok("Key Not Found");
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
