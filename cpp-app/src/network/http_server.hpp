#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../commands/kv_command.hpp"
#include "../common/log.hpp"
#include "../common/metrics.hpp"
#include "../config/config.hpp"
#include "../raft/raft_client.hpp"
#include "../storage/kv_store.hpp"
#include "http_request.hpp"
#include "thread_pool.hpp"

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
 * @brief Prefix on a transient failure the client should simply retry.
 *
 * Emitted by the sidecar when it knows who the leader is but could not reach it
 * — the normal state of the world for a second or two during a failover. Mapped
 * to 503, not 502: a routine leader change must not look fatal to a client that
 * retries on 503 and gives up on 502.
 */
inline constexpr const char *kUnavailablePrefix = "unavailable:";

/**
 * @brief Read consistency modes for GET (R4.4).
 *
 * `local` is the default and the pre-Phase-4 behavior: answered from this
 * node's own store, which on a follower may be stale. `linearizable` goes
 * through the sidecar, which forwards to the leader and runs Barrier +
 * VerifyLeader there. The default stays `local` because it is the cheap one and
 * changing it would silently make every existing client pay for consensus.
 */
inline constexpr const char *kConsistencyLocal = "local";
inline constexpr const char *kConsistencyLinearizable = "linearizable";

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
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Content Too Large";
    case 415:
      return "Unsupported Media Type";
    case 431:
      return "Request Header Fields Too Large";
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
    // R5.4: every request is counted and timed, by ROUTE and status class.
    // Route rather than raw path: /kv/{key} would otherwise create one series
    // per key, which is the classic way to blow up a Prometheus instance.
    const auto started = std::chrono::steady_clock::now();
    const std::string route = route_label(request);
    const HttpResponse response = route_request(request);

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    metrics::Registry::global()
        .histogram("raftkv_http_request_duration_seconds",
                   "Time to serve one HTTP request.", {{"route", route}})
        .observe(elapsed);
    metrics::Registry::global()
        .counter(
            "raftkv_http_requests_total",
            "HTTP requests served, by route and status class.",
            {{"route", route}, {"status", status_class(response.status_code)}})
        .inc();
    return response;
  }

private:
  /**
   * @brief The route this request matched, for use as a metric label.
   *
   * Deliberately NOT the path: /kv/{key} carries the key, and labelling by it
   * would create an unbounded number of time series — one per key ever written.
   * Unrecognized paths collapse to "other" for the same reason.
   */
  [[nodiscard]] static std::string route_label(const HttpRequest &request) {
    if (request.path.rfind(kKvPathPrefix, 0) == 0) {
      return request.method + " /kv/{key}";
    }
    if (request.path == "/insert-val" || request.path == "/get-val" ||
        request.path == "/metrics") {
      return request.method + " " + request.path;
    }
    return "other";
  }

  /** @brief 2xx/4xx/5xx, which is the granularity worth alerting on. */
  [[nodiscard]] static std::string status_class(int status_code) {
    return std::to_string(status_code / 100) + "xx";
  }

public:
  /** @brief Routing, separated from the instrumentation wrapper above. */
  [[nodiscard]] HttpResponse route_request(const HttpRequest &request) const {
    // R5.4: the scrape endpoint. Before the others because it must work even
    // when the store or the sidecar does not.
    if (request.method == "GET" && request.path == "/metrics") {
      HttpResponse response =
          HttpResponse::ok(metrics::Registry::global().render());
      // Prometheus' own content type; text/plain would also be accepted but
      // being explicit is what makes promtool happy.
      response.content_type = "text/plain; version=0.0.4; charset=utf-8";
      return response;
    }

    // R4.6: the REST surface. Checked before the legacy routes because it is
    // the one clients should be using.
    if (request.path.rfind(kKvPathPrefix, 0) == 0) {
      return handle_kv(request);
    }

    // R4.8: deprecated aliases, kept for one release so existing clients and
    // test_client.py keep working. They go through exactly the same propose and
    // read paths — only the routing differs.
    //
    // ONE DELIBERATE DIFFERENCE: these do NOT percent-decode. /kv/{key} decodes
    // its path, so `/kv/hello%20world` addresses "hello world", while
    // `/get-val?key=hello%20world` addresses the literal "hello%20world". The
    // legacy routes are left exactly as they were on purpose: decoding them now
    // would silently change which key an existing client reaches (a key
    // containing a literal '%' or '+' would move), and they are scheduled for
    // removal anyway. New clients should use /kv/{key}.
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

  /** @brief Prefix of the REST surface; everything after it is the key. */
  static constexpr const char *kKvPathPrefix = "/kv/";

  /**
   * @brief Route and serve PUT/GET/DELETE /kv/{key} (R4.6).
   *
   * The key is percent-decoded, so `/kv/hello%20world` addresses the key
   * "hello world" and `/kv/a%2Fb` addresses "a/b" rather than being mistaken
   * for a nested path. A malformed escape is a 400 rather than a guess — see
   * url_decode.
   */
  [[nodiscard]] HttpResponse handle_kv(const HttpRequest &request) const {
    const std::string raw_key =
        request.path.substr(std::string(kKvPathPrefix).size());

    // decode_plus = false: in a PATH a '+' is a literal plus. Decoding it as a
    // space here would make /kv/a+b and /kv/a%20b the same key, which they are
    // not.
    const std::optional<std::string> key = url_decode(raw_key, false);
    if (!key.has_value()) {
      return HttpResponse::json_error(
          400, "malformed percent-encoding in the request path");
    }
    if (key->empty()) {
      return HttpResponse::json_error(400, "key must not be empty");
    }

    if (request.method == "PUT") {
      return handle_kv_put(request, *key);
    }
    if (request.method == "GET") {
      return handle_kv_get(request, *key);
    }
    if (request.method == "DELETE") {
      return handle_kv_delete(*key);
    }
    return HttpResponse::json_error(
        405, "method not allowed on /kv/{key}: use PUT, GET or DELETE");
  }

  /**
   * @brief PUT /kv/{key} - the request body IS the value.
   *
   * Any media type is accepted and the bytes are stored verbatim: a value is
   * opaque to this store, so insisting on a Content-Type would be ceremony. The
   * body may be empty — storing an empty value is legitimate and distinct from
   * the key being absent.
   *
   * R4.7: the raft payload is built with KVCommand::encode_set, the same
   * encoder the legacy route's clients use and the same one the WAL uses, so a
   * new entry is byte-identical to an old one for the same command.
   */
  [[nodiscard]] HttpResponse handle_kv_put(const HttpRequest &request,
                                           const std::string &key) const {
    if (request.bad_content_length) {
      return HttpResponse::json_error(400, "malformed Content-Length");
    }
    return propose_and_map(KVCommand::encode_set(key, request.body));
  }

  /** @brief DELETE /kv/{key}. */
  [[nodiscard]] HttpResponse handle_kv_delete(const std::string &key) const {
    return propose_and_map(KVCommand::encode_delete(key));
  }

  /** @brief GET /kv/{key}, honoring ?consistency= exactly as /get-val does. */
  [[nodiscard]] HttpResponse handle_kv_get(const HttpRequest &request,
                                           const std::string &key) const {
    const std::map<std::string, std::string> params = request.query_params();
    const auto consistency = params.find("consistency");
    const std::string mode =
        consistency == params.end() ? kConsistencyLocal : consistency->second;

    if (mode == kConsistencyLinearizable) {
      return handle_linearizable_get(key);
    }
    if (mode != kConsistencyLocal) {
      return HttpResponse::json_error(
          400, "consistency must be \"local\" or \"linearizable\"");
    }

    const std::optional<std::string> value = store_.get(key);
    if (!value) {
      return HttpResponse::json_error(404, "key not found");
    }
    return HttpResponse::ok(*value);
  }

  /**
   * @brief Propose an encoded command and map the outcome onto a status code.
   *
   * Shared by the REST and legacy write paths so there is one place that
   * decides what a propose failure means to a client.
   */
  [[nodiscard]] HttpResponse propose_and_map(const std::string &payload) const {
    const ProposeResult result = raft_client_.propose(payload);
    if (result.success) {
      return HttpResponse::json(200, "{\"ok\":true}");
    }
    return failure_response(result.error);
  }

  /**
   * @brief Map a sidecar failure string onto a status code.
   *
   * Three shapes, and the distinction is the whole point of Phase 1's error
   * vocabulary:
   *   - not_leader:<addr>  -> 503 naming an address to retry against;
   *   - unavailable:<why>  -> 503, transient, retry the same node shortly;
   *   - anything else      -> 502, the client cannot fix this by retrying.
   */
  [[nodiscard]] static HttpResponse failure_response(const std::string &error) {
    const std::optional<std::string> leader = not_leader_address(error);
    if (leader.has_value()) {
      return HttpResponse::json(503, not_leader_body(*leader));
    }
    const std::string unavailable(kUnavailablePrefix);
    if (error.rfind(unavailable, 0) == 0) {
      return HttpResponse::json_error(503, error.substr(unavailable.size()));
    }
    return HttpResponse::json_error(502, error);
  }

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
    return failure_response(result.error);
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

    // R4.4: consistency=local (default) | linearizable.
    //
    // `local` is what every read did before Phase 4 and still does: served
    // straight from this node's store, which on a follower can be arbitrarily
    // stale. That is now an explicit choice rather than an unmentioned
    // property.
    const auto consistency = params.find("consistency");
    const std::string mode =
        consistency == params.end() ? kConsistencyLocal : consistency->second;

    if (mode == kConsistencyLinearizable) {
      return handle_linearizable_get(it->second);
    }
    if (mode != kConsistencyLocal) {
      return HttpResponse::json_error(
          400, "consistency must be \"local\" or \"linearizable\"");
    }

    const std::optional<std::string> value = store_.get(it->second);
    if (!value) {
      return HttpResponse::json_error(404, "key not found");
    }
    return HttpResponse::ok(*value);
  }

  /**
   * @brief Serve a read that reflects every acknowledged write (R4.5).
   *
   * Delegates to the sidecar, which forwards to the leader when this node is
   * not it, and there runs Barrier + VerifyLeader before reading its own store.
   * None of that logic belongs here — this layer only maps the outcome onto a
   * status code, and it maps it the same way a failed propose is mapped so a
   * client sees one consistent error vocabulary.
   */
  [[nodiscard]] HttpResponse
  handle_linearizable_get(const std::string &key) const {
    const ReadResult result = raft_client_.read(key);

    if (!result.ok) {
      return failure_response(result.error);
    }

    if (!result.found) {
      return HttpResponse::json_error(404, "key not found");
    }
    return HttpResponse::ok(result.value);
  }

  /**
   * @brief The 503 body naming the leader a client should retry against.
   *
   * Shared by the write path and the linearizable-read path so both speak the
   * same error vocabulary. The address is escaped because it originates outside
   * this process, and it can legitimately be empty during an election.
   */
  [[nodiscard]] static std::string not_leader_body(const std::string &leader) {
    return "{\"error\":\"not leader\",\"leader\":\"" + json_escape(leader) +
           "\"}";
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
  HttpServer(int port, KVHttpHandler handler, RequestLimits limits = {},
             size_t workers = 0)
      : port_(port), handler_(std::move(handler)), limits_(limits),
        pool_(workers == 0 ? ThreadPool::default_workers() : workers) {
    setup_socket();
  }

  ~HttpServer() { stop(); }

  // Non-copyable
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  /**
   * @brief Start the server and run the accept loop.
   *
   * This method blocks indefinitely, accepting and handling connections.
   */
  void run() {
    log::info(log::kComponentHttp, "listening",
              {log::field("port", static_cast<long long>(port_)),
               log::field("workers", pool_.size())});

    while (!stopping_.load(std::memory_order_relaxed)) {
      const int client_socket = accept(server_fd_, nullptr, nullptr);
      if (client_socket < 0) {
        // stop() closes the listen socket to break us out of accept(), so an
        // error here is expected during shutdown and is not worth reporting.
        if (stopping_.load(std::memory_order_relaxed)) {
          break;
        }
        // EINTR and friends: a transient accept failure must not kill the loop.
        // Spinning on a permanently broken listener is the lesser evil compared
        // to a node that silently stops accepting connections.
        continue;
      }

      // R4.10: hand the connection to a worker instead of serving it inline.
      // Serving inline is what let one slow client hold the whole HTTP surface.
      if (!pool_.submit(
              [this, client_socket] { handle_connection(client_socket); })) {
        // Pool is shutting down and refused the task. WE still own this fd, so
        // close it here — dropping it would leak a descriptor per connection.
        close(client_socket);
      }
    }
  }

  /**
   * @brief Stop accepting connections and let in-flight ones finish (R4.10).
   *
   * Closing the listen socket is what unblocks the accept() the run loop is
   * parked in; a flag alone would leave it waiting for a connection that may
   * never come. Workers then drain the queue and join, so every accepted
   * connection is still answered.
   */
  void stop() {
    request_stop();
    // Joining the workers is the part that must NOT happen in a signal handler,
    // which is why it lives here and not in request_stop().
    pool_.stop();
    if (server_fd_ >= 0) {
      close(server_fd_);
      server_fd_ = -1;
    }
  }

  /**
   * @brief Ask the accept loop to stop, using only async-signal-safe calls.
   *
   * Safe to call from a POSIX signal handler, which is the whole reason it is
   * separate from stop(). Only two things happen here: an atomic store, and
   * shutdown(2) on the listen socket. Both are async-signal-safe.
   *
   * stop() joins the worker pool, and joining threads from a signal handler is
   * not safe — it takes locks the interrupted thread may already hold, so the
   * process can deadlock or die without ever running the handler's own code.
   * That was not theoretical: the first version of the SIGTERM path called
   * stop() directly from the handler and produced no output at all before the
   * process went away.
   *
   * shutdown() rather than close(): closing the descriptor here would race a
   * worker still using it, and shutdown() is what actually unblocks a thread
   * parked in accept(). run() then sees the flag and returns, and main does the
   * real cleanup from normal context.
   */
  void request_stop() {
    stopping_.store(true, std::memory_order_relaxed);
    const int fd = server_fd_;
    if (fd >= 0) {
      ::shutdown(fd, SHUT_RDWR);
    }
  }

private:
  int port_;
  int server_fd_ = -1;
  KVHttpHandler handler_;

  /** @brief Inbound request bounds (R4.9). */
  RequestLimits limits_;

  /**
   * @brief Worker pool serving accepted connections (R4.10).
   *
   * Everything a worker touches must be thread-safe: PersistentKVStore locks
   * internally, and GrpcRaftClient shares one gRPC channel (channels are
   * thread-safe) with a per-call ClientContext. See R4.11.
   */
  ThreadPool pool_;

  /** @brief Set by stop() to break the accept loop. */
  std::atomic<bool> stopping_{false};

  static constexpr size_t kBufferSize = 4096;

  /** @brief Write a response, ignoring a peer that has already gone away. */
  static void send_response(int client_socket, const HttpResponse &response) {
    const std::string serialized = response.to_string();
    send(client_socket, serialized.c_str(), serialized.size(), 0);
  }

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
    std::string raw_request;

    // R4.9: read until the header terminator actually arrives, instead of
    // assuming it fits in the first packet. It usually does, but "usually" is
    // not a parsing rule — a client is free to dribble headers one byte per
    // segment, and the old single-recv version simply failed to parse those.
    //
    // Bounded by max_header_bytes so a client that never sends the terminator
    // cannot make this buffer without limit.
    size_t header_end = std::string::npos;
    while (true) {
      header_end = raw_request.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        break;
      }
      if (raw_request.size() > limits_.max_header_bytes) {
        send_response(client_socket, HttpResponse::json_error(
                                         431, "request headers too large"));
        close(client_socket);
        return;
      }
      const ssize_t n = recv(client_socket, buffer.data(), buffer.size(), 0);
      if (n <= 0) {
        // Peer gave up or closed mid-headers. Nothing to answer.
        close(client_socket);
        return;
      }
      raw_request.append(buffer.data(), static_cast<size_t>(n));
    }

    auto parsed_request = HttpRequestParser::parse(raw_request);
    if (!parsed_request) {
      close(client_socket);
      return;
    }

    // Refuse an oversized body from its declared length, before reading it.
    // Reading it first and then rejecting would let a client spend this
    // process's memory to earn a 413.
    if (parsed_request->content_length > 0 &&
        static_cast<size_t>(parsed_request->content_length) >
            limits_.max_body_bytes) {
      send_response(client_socket,
                    HttpResponse::json_error(413, "request body too large"));
      close(client_socket);
      return;
    }

    // Read the remaining body.
    //
    // The `> 0` guard is belt-and-braces against a negative content_length
    // reaching here: the cast below is to size_t, so a negative value becomes
    // enormous and this loop would never terminate. HttpRequestParser already
    // rejects negatives into bad_content_length, but this loop runs BEFORE the
    // handler gets a chance to turn that into a 400, so it must be safe on its
    // own.
    while (parsed_request->content_length > 0 &&
           parsed_request->body.size() <
               static_cast<size_t>(parsed_request->content_length)) {
      const ssize_t n = recv(client_socket, buffer.data(), buffer.size(), 0);
      if (n <= 0)
        break;
      parsed_request->body.append(buffer.data(), static_cast<size_t>(n));
      // A body longer than advertised is also capped: content_length bounds the
      // loop, but a pipelined follow-up request would arrive in the same read.
      if (parsed_request->body.size() > limits_.max_body_bytes) {
        send_response(client_socket,
                      HttpResponse::json_error(413, "request body too large"));
        close(client_socket);
        return;
      }
    }

    // Handle request and send response
    HttpResponse response = handler_.handle(*parsed_request);
    std::string response_str = response.to_string();
    send(client_socket, response_str.c_str(), response_str.size(), 0);

    close(client_socket);
  }
};

} // namespace kvdb
