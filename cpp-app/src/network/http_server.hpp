#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../auth/auth_engine.hpp"
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
 * @brief The challenge sent with a 401, naming the scheme a client should use.
 *
 * RFC 9110 requires a 401 to carry WWW-Authenticate; without it a client has
 * been told "authenticate" and not told how. Basic (not Bearer): the credential
 * is a user name and password, and the management API's bearer token is a
 * different thing guarding a different surface.
 */
inline constexpr const char *kBasicChallenge = "Basic realm=\"raftkv\"";

/**
 * @brief HTTP response builder utility.
 */
struct HttpResponse {
  int status_code = 200;
  std::string body;
  std::string content_type = kTextContentType;

  /**
   * @brief Extra header lines, emitted verbatim after the fixed ones.
   *
   * Exists for WWW-Authenticate. Names and values are NOT escaped or validated,
   * because every value written here is a compile-time constant in this file;
   * do not put attacker-influenced text in one without adding that check, since
   * a CRLF in a value would be response splitting.
   */
  std::vector<std::pair<std::string, std::string>> extra_headers;

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
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
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
    for (const auto &header : extra_headers) {
      response += "\r\n";
      response += header.first;
      response += ": ";
      response += header.second;
    }
    response += "\r\n\r\n";
    response += body;
    return response;
  }

  /**
   * @brief 200 carrying a raw stored value as plain text.
   *
   * The trailing {} is the (empty) extra_headers list. It is spelled out
   * because this aggregate is initialised positionally and -Wextra warns on a
   * member left out — the build treats new warnings as errors to fix.
   */
  [[nodiscard]] static HttpResponse ok(const std::string &body) {
    return HttpResponse{200, body, kTextContentType, {}};
  }

  /** @brief A JSON body with an explicit status code. */
  [[nodiscard]] static HttpResponse json(int status_code,
                                         const std::string &body) {
    return HttpResponse{status_code, body, kJsonContentType, {}};
  }

  /** @brief The standard error envelope: {"error":"<escaped message>"}. */
  [[nodiscard]] static HttpResponse json_error(int status_code,
                                               const std::string &message) {
    return json(status_code, "{\"error\":\"" + json_escape(message) + "\"}");
  }

  /**
   * @brief 401 with the Basic challenge attached.
   *
   * 401 means "you presented no usable credential"; 403 means "the credential
   * you presented was rejected, or does not permit this". Only the first is an
   * invitation to try again, so only the first carries a challenge — the same
   * split the management API makes (go-sidecar/internal/management/auth.go).
   */
  [[nodiscard]] static HttpResponse unauthorized(const std::string &message) {
    HttpResponse response = json_error(401, message);
    response.extra_headers.emplace_back("WWW-Authenticate", kBasicChallenge);
    return response;
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
   * @param auth Authenticator. Injected behind IAuthEngine like the other two
   *             dependencies, so the handler's routing and status codes can be
   *             tested without a store or a cluster.
   */
  KVHttpHandler(IRaftClient &raft_client, const IKVStore &store,
                const auth::IAuthEngine &auth)
      : raft_client_(raft_client), store_(store), auth_(auth) {}

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
    // Same reasoning as /kv/{key}: the user name must not become a label.
    if (request.path.rfind(kAuthUsersPrefix, 0) == 0) {
      return request.method + " /auth/users/{name}";
    }
    if (request.path == kWhoamiPath || request.path == "/insert-val" ||
        request.path == "/get-val" || request.path == "/metrics") {
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

    // AUTHENTICATE ONCE, here, for every route below. Doing it per-handler is
    // how a new route ends up unauthenticated by omission.
    //
    // /metrics above is deliberately outside this gate: the secure profile's
    // Caddy proxy health-checks it, so requiring a credential there would drop
    // every node out of the load-balancer rotation. It exposes counters and
    // latencies, never keys or values.
    auth::AuthContext identity = auth::AuthContext::unrestricted();
    if (auth_.enabled()) {
      const auth::AuthOutcome outcome = auth_.authenticate(
          request.header(auth::kAuthorizationHeader), identity);
      if (outcome == auth::AuthOutcome::kNoCredentials) {
        return HttpResponse::unauthorized("authentication required");
      }
      if (outcome != auth::AuthOutcome::kOk) {
        // One message for unknown user, disabled user and wrong password: see
        // AuthOutcome. Distinguishing them enumerates accounts.
        return HttpResponse::json_error(403, "invalid credentials");
      }
    }

    // The user-management surface. Before /kv/ — the prefixes cannot collide,
    // but keeping the admin API first makes the order of checks obvious.
    if (request.path.rfind(kAuthPathPrefix, 0) == 0) {
      return handle_auth(request, identity);
    }

    // R4.6: the REST surface. Checked before the legacy routes because it is
    // the one clients should be using.
    if (request.path.rfind(kKvPathPrefix, 0) == 0) {
      return handle_kv(request, identity);
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
      return handle_insert(request, identity);
    }
    if (request.method == "GET" && request.path == "/get-val") {
      return handle_get(request, identity);
    }
    return HttpResponse::json_error(404, "not found");
  }

private:
  IRaftClient &raft_client_;
  const IKVStore &store_;
  const auth::IAuthEngine &auth_;

  /** @brief Prefix of the REST surface; everything after it is the key. */
  static constexpr const char *kKvPathPrefix = "/kv/";

  /** @brief Prefix of the user-management surface. */
  static constexpr const char *kAuthPathPrefix = "/auth/";

  /** @brief Prefix of the user CRUD routes; everything after it is the name. */
  static constexpr const char *kAuthUsersPrefix = "/auth/users/";

  /** @brief "Who am I, and what may I do?" */
  static constexpr const char *kWhoamiPath = "/auth/whoami";

  /**
   * @brief Refuse a data-route key that belongs to the reserved key space.
   *
   * UNCONDITIONAL — enforced whether or not auth is enabled, and enforced
   * against the bootstrap admin too. Two reasons, both concrete:
   *   - READS are covered as well as writes, because a local read of
   *     `__sys:user:alice` would hand out her salt and password hash to anyone
   *     holding the read class;
   *   - user records are reachable only through /auth/*, which never serializes
   *     a hash, so there is no legitimate caller for this and no exemption
   * worth having.
   *
   * @return A 403 response when @p key is reserved, otherwise nullopt.
   */
  [[nodiscard]] static std::optional<HttpResponse>
  reject_reserved_key(const std::string &key) {
    if (!auth::is_reserved_key(key)) {
      return std::nullopt;
    }
    return HttpResponse::json_error(
        403, std::string("keys under \"") + auth::kSysPrefix +
                 "\" are reserved; use /auth/users/{name}");
  }

  /**
   * @brief Check a class and a key pattern, returning a 403 when either fails.
   *
   * Both halves are needed and neither implies the other: the class says what
   * kind of operation is permitted, the pattern says on which keys. One message
   * covers both, because telling a caller which of the two it failed tells it
   * about an ACL it is not entitled to know.
   */
  [[nodiscard]] static std::optional<HttpResponse>
  authorize(const auth::AuthContext &identity, auth::CommandClass cls,
            const std::string &key) {
    if (identity.has_class(cls) && identity.key_allowed(key)) {
      return std::nullopt;
    }
    return HttpResponse::json_error(403, "permission denied");
  }

  /**
   * @brief Route and serve PUT/GET/DELETE /kv/{key} (R4.6).
   *
   * The key is percent-decoded, so `/kv/hello%20world` addresses the key
   * "hello world" and `/kv/a%2Fb` addresses "a/b" rather than being mistaken
   * for a nested path. A malformed escape is a 400 rather than a guess — see
   * url_decode.
   */
  [[nodiscard]] HttpResponse
  handle_kv(const HttpRequest &request,
            const auth::AuthContext &identity) const {
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
    // AFTER decoding, so /kv/__sys%3Auser%3Aadmin cannot slip past by spelling
    // the prefix in escapes.
    if (std::optional<HttpResponse> refusal = reject_reserved_key(*key)) {
      return *refusal;
    }

    if (request.method == "PUT") {
      if (std::optional<HttpResponse> denied =
              authorize(identity, auth::CommandClass::kWrite, *key)) {
        return *denied;
      }
      return handle_kv_put(request, *key);
    }
    if (request.method == "GET") {
      if (std::optional<HttpResponse> denied =
              authorize(identity, auth::CommandClass::kRead, *key)) {
        return *denied;
      }
      return handle_kv_get(request, *key);
    }
    if (request.method == "DELETE") {
      if (std::optional<HttpResponse> denied =
              authorize(identity, auth::CommandClass::kWrite, *key)) {
        return *denied;
      }
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
  [[nodiscard]] HttpResponse
  handle_insert(const HttpRequest &request,
                const auth::AuthContext &identity) const {
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

    // THIS ROUTE NOW LOOKS INSIDE THE BODY, which it never used to. The body is
    // a msgpack KVCommand that was previously forwarded opaquely and parsed for
    // the first time at apply time; but a key ACL cannot be enforced without
    // knowing which key is being written, so it is decoded here as well.
    //
    // Cost: one extra decode per legacy write, under the same bounded limits.
    // Benefit: /insert-val is not a hole straight through the ACLs.
    std::optional<KVCommand> command;
    try {
      command =
          KVCommand::from_msgpack(request.body.data(), request.body.size());
    } catch (const std::exception &) {
      command = std::nullopt;
    }

    if (command.has_value()) {
      // Refused for EVERYONE, auth on or off: the user-management operations
      // exist to be reachable only through /auth/users/{name}, where the admin
      // class is checked. Accepting one here would let any caller holding the
      // write class mint an administrator.
      if (is_user_operation(command->operation_type())) {
        return HttpResponse::json_error(
            400, "user-management operations are not accepted on /insert-val; "
                 "use /auth/users/{name}");
      }
      if (std::optional<HttpResponse> refusal =
              reject_reserved_key(command->key)) {
        return *refusal;
      }
      if (std::optional<HttpResponse> denied =
              authorize(identity, auth::CommandClass::kWrite, command->key)) {
        return *denied;
      }
    } else if (auth_.enabled()) {
      // A body that will not decode names no key, so there is nothing to check
      // an ACL against. With auth on that has to be a refusal — forwarding it
      // would be an unauthorized write to an unknown key. With auth OFF it is
      // still forwarded (below) and still rejected at apply time with a 502,
      // which is the documented "validation happens after commit" behaviour and
      // is pinned by the e2e suite.
      return HttpResponse::json_error(400, "request body is not a decodable "
                                           "command");
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
  [[nodiscard]] HttpResponse
  handle_get(const HttpRequest &request,
             const auth::AuthContext &identity) const {
    const std::map<std::string, std::string> params = request.query_params();
    const auto it = params.find("key");
    if (it == params.end()) {
      const std::string message = "missing required query parameter: key";
      return HttpResponse::json_error(400, message);
    }
    // This route does NOT percent-decode (see the note in route_request), so
    // the key is the literal query text — which is also what is checked here.
    if (std::optional<HttpResponse> refusal = reject_reserved_key(it->second)) {
      return *refusal;
    }
    if (std::optional<HttpResponse> denied =
            authorize(identity, auth::CommandClass::kRead, it->second)) {
      return *denied;
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
   * @brief Render a list of strings as a JSON array.
   *
   * No JSON library is linked into this binary, so every structured body is
   * hand-built; each element goes through json_escape because pattern text is
   * operator-supplied and a bare quote would produce a body no client can
   * parse.
   */
  [[nodiscard]] static std::string
  json_string_array(const std::vector<std::string> &values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
      if (i != 0) {
        out += ",";
      }
      out += "\"" + json_escape(values[i]) + "\"";
    }
    out += "]";
    return out;
  }

  /**
   * @brief Route the /auth/* surface: user CRUD and whoami.
   *
   * DEFAULT-CLOSED. With no admin password configured this whole surface
   * answers 403 rather than being open, exactly like the management API's /join
   * with no token. An unconfigured security feature must never read as "no
   * restriction".
   */
  [[nodiscard]] HttpResponse
  handle_auth(const HttpRequest &request,
              const auth::AuthContext &identity) const {
    if (!auth_.enabled()) {
      return HttpResponse::json_error(
          403, "user management is disabled; set RAFTKV_ADMIN_PASSWORD to "
               "enable authentication");
    }

    if (request.path == kWhoamiPath) {
      if (request.method != "GET") {
        return HttpResponse::json_error(405,
                                        "method not allowed on /auth/whoami: "
                                        "use GET");
      }
      // Any authenticated caller may ask about ITSELF — it learns nothing it
      // did not already prove — so this one needs no class.
      std::string body = "{\"name\":\"" + json_escape(identity.name) + "\"";
      body += ",\"classes\":" + json_string_array(held_classes(identity));
      body += ",\"patterns\":" + json_string_array(identity.patterns) + "}";
      return HttpResponse::json(200, body);
    }

    if (request.path.rfind(kAuthUsersPrefix, 0) != 0) {
      return HttpResponse::json_error(404, "not found");
    }
    if (std::optional<HttpResponse> denied = authorize_admin(identity)) {
      return *denied;
    }

    const std::optional<std::string> name = url_decode(
        request.path.substr(std::string(kAuthUsersPrefix).size()), false);
    if (!name.has_value()) {
      return HttpResponse::json_error(
          400, "malformed percent-encoding in the request path");
    }
    if (const std::optional<std::string> bad = auth::username_error(*name)) {
      return HttpResponse::json_error(400, *bad);
    }
    // The bootstrap admin is defined by configuration, not by a record, and is
    // checked before the store — so a record under that name would be dead
    // weight that looks like a live account. Refuse to create the confusion.
    if (*name == auth::kBootstrapAdminName) {
      return HttpResponse::json_error(
          400, std::string("\"") + auth::kBootstrapAdminName +
                   "\" is defined by RAFTKV_ADMIN_PASSWORD and cannot be "
                   "managed through this API");
    }

    if (request.method == "PUT") {
      return handle_user_put(request, *name);
    }
    if (request.method == "DELETE") {
      return propose_and_map(KVCommand::encode_user_del(*name));
    }
    if (request.method == "GET") {
      return handle_user_get(*name);
    }
    return HttpResponse::json_error(
        405,
        "method not allowed on /auth/users/{name}: use PUT, GET or DELETE");
  }

  /** @brief The classes @p identity holds, for a response body. */
  [[nodiscard]] static std::vector<std::string>
  held_classes(const auth::AuthContext &identity) {
    std::vector<std::string> classes;
    if (identity.read) {
      classes.emplace_back(auth::kClassRead);
    }
    if (identity.write) {
      classes.emplace_back(auth::kClassWrite);
    }
    if (identity.admin) {
      classes.emplace_back(auth::kClassAdmin);
    }
    return classes;
  }

  /** @brief 403 unless @p identity holds the admin class. */
  [[nodiscard]] static std::optional<HttpResponse>
  authorize_admin(const auth::AuthContext &identity) {
    if (identity.has_class(auth::CommandClass::kAdmin)) {
      return std::nullopt;
    }
    return HttpResponse::json_error(403, "permission denied");
  }

  /**
   * @brief PUT /auth/users/{name} — create or replace a user.
   *
   * A BLIND FULL-RECORD WRITE, never a read-modify-write. Writes are
   * at-least-once under failure (a 503 does not mean nothing was applied), so a
   * "change only the password" operation built on read-then-write would have no
   * safe retry. Replacing the whole record is idempotent: the same request
   * applied twice leaves the same user, bar the salt.
   *
   * The salt and hash are computed HERE, on the node serving the request, and
   * travel inside the committed entry — so every replica stores identical
   * bytes. Generating them during Apply would give each replica a different
   * record.
   */
  [[nodiscard]] HttpResponse handle_user_put(const HttpRequest &request,
                                             const std::string &name) const {
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

    auth::UserUpsertRequest upsert;
    try {
      upsert = auth::UserUpsertRequest::from_msgpack(request.body.data(),
                                                     request.body.size());
    } catch (const std::exception &) {
      return HttpResponse::json_error(
          400, "body must be a msgpack map of {password, enabled, classes, "
               "patterns}");
    }
    if (const std::optional<std::string> bad = upsert.validation_error()) {
      return HttpResponse::json_error(400, *bad);
    }

    auth::UserRecord record;
    try {
      record = upsert.to_record(name);
    } catch (const std::exception &e) {
      // No randomness available. Refusing is the only safe answer: a
      // predictable salt is no salt, and this must never fall back to one.
      log::error(log::kComponentHttp, "cannot generate a password salt",
                 {log::field("error", e.what())});
      return HttpResponse::json_error(500, "cannot generate a password salt");
    }

    return propose_and_map(
        KVCommand::encode_user_set(name, record.to_msgpack()));
  }

  /**
   * @brief GET /auth/users/{name} — the user's ACL, never its secret.
   *
   * The salt and the password hash are DELIBERATELY not serialized. They are
   * the only things worth stealing here, an administrator has no use for them,
   * and an endpoint that returns them turns one compromised admin credential
   * into an offline attack on every user's password.
   *
   * Served from the local store, so on a follower it may lag the leader by the
   * replication delay — the same staleness a local GET has.
   */
  [[nodiscard]] HttpResponse handle_user_get(const std::string &name) const {
    const std::optional<std::string> stored =
        store_.get(auth::user_storage_key(name));
    if (!stored.has_value()) {
      return HttpResponse::json_error(404, "user not found");
    }

    auth::UserRecord record;
    try {
      record = auth::UserRecord::from_msgpack(stored->data(), stored->size());
    } catch (const std::exception &) {
      // Apply validates before storing, so this means the bytes were corrupted
      // after the fact. Reporting it as a server error is honest; reporting 404
      // would hide a real problem.
      return HttpResponse::json_error(500, "stored user record is unreadable");
    }

    std::string body = "{\"name\":\"" + json_escape(record.name) + "\"";
    body += ",\"enabled\":";
    body += record.enabled ? "true" : "false";
    body += ",\"classes\":" + json_string_array(record.classes);
    body += ",\"patterns\":" + json_string_array(record.patterns) + "}";
    return HttpResponse::json(200, body);
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
