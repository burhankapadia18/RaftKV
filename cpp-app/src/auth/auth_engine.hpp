#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../config/config.hpp"
#include "../network/http_request.hpp"
#include "../storage/kv_store.hpp"
#include "base64.hpp"
#include "glob.hpp"
#include "sha256.hpp"
#include "user_record.hpp"

/**
 * @file auth_engine.hpp
 * @brief Authentication (who is this?) and authorization (may they?) for the
 *        client HTTP API.
 */

namespace kvdb {
namespace auth {

/** @brief The header a Basic credential arrives in, in the parser's key form.
 */
inline constexpr const char *kAuthorizationHeader = "authorization";

/** @brief The scheme this server accepts. Compared case-insensitively. */
inline constexpr const char *kBasicScheme = "basic";

/**
 * @brief The bootstrap admin's name.
 *
 * A VIRTUAL user: it exists whenever RAFTKV_ADMIN_PASSWORD is set and is
 * checked before the store is consulted. That ordering is the point — it
 * removes a bootstrap race that would otherwise have no good answer. A cluster
 * whose first admin had to be created through Raft could not create it, because
 * creating a user requires an admin. It is the same idea as a database's
 * bootstrap superuser: configured out-of-band, not stored in the data it
 * administers.
 *
 * Because the check precedes the store lookup, a record smuggled in at
 * `__sys:user:admin` is shadowed and cannot take the name over.
 */
inline constexpr const char *kBootstrapAdminName = "admin";

/** @brief What a request is trying to do, as an ACL category. */
enum class CommandClass { kRead, kWrite, kAdmin };

/**
 * @brief The result of examining a credential.
 *
 * The three-way split is the 401/403 distinction, decided here rather than in
 * the handler so both surfaces cannot drift:
 *   - kNoCredentials  -> 401 + WWW-Authenticate. Nothing usable was presented,
 *                        so inviting a retry is the correct answer.
 *   - kBadCredentials -> 403. Something was presented and rejected.
 * Deliberately the SAME outcome for an unknown user, a disabled user and a
 * wrong password: distinguishing them turns the endpoint into a
 * user-enumeration oracle.
 */
enum class AuthOutcome { kOk, kNoCredentials, kBadCredentials };

/**
 * @brief An authenticated identity and what it may touch.
 *
 * Value object. Held only for the duration of one request — there are no
 * sessions, because the secure profile puts a round-robin proxy in front of
 * three nodes and there is nowhere to keep one.
 */
struct AuthContext {
  std::string name;
  bool read = false;
  bool write = false;
  bool admin = false;

  /** @brief Glob patterns; an empty list denies every key. */
  std::vector<std::string> patterns;

  /** @brief Full data access under the name "-", for when auth is disabled. */
  [[nodiscard]] static AuthContext unrestricted() {
    AuthContext context;
    context.name = "-";
    context.read = true;
    context.write = true;
    // NOT admin: with auth off there is no way to authenticate an
    // administrator, so the user-management API must answer 403 rather than let
    // anyone in.
    context.admin = false;
    context.patterns = {"*"};
    return context;
  }

  [[nodiscard]] bool has_class(CommandClass cls) const {
    switch (cls) {
    case CommandClass::kRead:
      return read;
    case CommandClass::kWrite:
      return write;
    case CommandClass::kAdmin:
      return admin;
    }
    return false;
  }

  /** @brief True when any held pattern matches @p key. */
  [[nodiscard]] bool key_allowed(const std::string &key) const {
    for (const std::string &pattern : patterns) {
      if (glob_match(pattern, key)) {
        return true;
      }
    }
    return false;
  }
};

/**
 * @brief Authenticator seam, so KVHttpHandler can be tested without a store.
 *
 * Mirrors IKVStore / IRaftClient: pure virtual, virtual destructor, `I` prefix,
 * injected by reference.
 */
class IAuthEngine {
public:
  virtual ~IAuthEngine() = default;

  /** @brief False when no admin password is configured; auth is then bypassed.
   */
  [[nodiscard]] virtual bool enabled() const = 0;

  /**
   * @brief Identify the caller from an Authorization header value.
   * @param authorization The raw header value, "" when the header was absent.
   * @param out           Filled in only when the result is kOk.
   */
  [[nodiscard]] virtual AuthOutcome
  authenticate(const std::string &authorization, AuthContext &out) const = 0;
};

/**
 * @brief The real authenticator: bootstrap admin from config, everyone else
 * from the replicated store.
 *
 * Reads go straight through IKVStore::get on every request. No cache, and none
 * is needed — it is an in-memory hash lookup — but the absence of one is also
 * what makes a revocation take effect as soon as the delete is applied locally.
 *
 * KNOWN AND ACCEPTED: on a FOLLOWER the lookup can be stale, because a
 * follower's store trails the leader by the replication lag. So a user deleted
 * a moment ago may still authenticate on one node for that long. It is bounded
 * by replication, not unbounded, and it is the same propagation delay any
 * replicated ACL has.
 */
class AuthEngine final : public IAuthEngine {
public:
  /**
   * @param store   Where user records live. Read-only from here — user changes
   *                go through Raft, never through this class.
   * @param options The configured bootstrap admin password.
   * @throws std::runtime_error if randomness is unavailable while auth is on.
   *
   * The admin password is hashed here with a PER-PROCESS random salt and the
   * cleartext is not retained. That is not about the on-disk threat (there is
   * no on-disk copy) — it keeps the password out of this long-lived object, so
   * a core dump or a stray log of the engine cannot spill it, and it makes the
   * admin path use the same comparison shape as the stored-user path.
   */
  AuthEngine(const IKVStore &store, const AuthOptions &options)
      : store_(store), enabled_(options.enabled) {
    if (enabled_) {
      admin_salt_hex_ = generate_salt_hex();
      admin_pw_hash_ = hash_password(admin_salt_hex_, options.admin_password);
    }
  }

  [[nodiscard]] bool enabled() const override { return enabled_; }

  [[nodiscard]] AuthOutcome authenticate(const std::string &authorization,
                                         AuthContext &out) const override {
    if (!enabled_) {
      // The handler short-circuits before calling this; answering consistently
      // anyway keeps the two paths from disagreeing if that ever changes.
      out = AuthContext::unrestricted();
      return AuthOutcome::kOk;
    }

    const std::optional<std::string> credential =
        decode_basic_credential(authorization);
    if (!credential.has_value()) {
      return AuthOutcome::kNoCredentials;
    }

    // RFC 7617: the credential is user-id ":" password, split at the FIRST
    // colon, so a password may contain colons and a user name may not. That is
    // also why username_error() forbids ':'.
    const size_t colon = credential->find(':');
    if (colon == std::string::npos) {
      return AuthOutcome::kNoCredentials;
    }
    const std::string name = credential->substr(0, colon);
    const std::string password = credential->substr(colon + 1);

    if (!valid_username(name)) {
      // Not distinguishable from "no such user": a name this server would never
      // store cannot be confirmed absent without leaking the naming rule.
      return reject_with_constant_work(password);
    }

    if (name == kBootstrapAdminName) {
      if (!constant_time_equal(admin_pw_hash_,
                               hash_password(admin_salt_hex_, password))) {
        return AuthOutcome::kBadCredentials;
      }
      out = admin_context();
      return AuthOutcome::kOk;
    }

    const std::optional<std::string> stored =
        store_.get(user_storage_key(name));
    if (!stored.has_value()) {
      return reject_with_constant_work(password);
    }

    UserRecord record;
    try {
      record = UserRecord::from_msgpack(stored->data(), stored->size());
    } catch (const std::exception &) {
      // A record that will not decode authenticates nobody. It cannot normally
      // exist — Apply validates before storing — but "cannot normally exist" is
      // not a reason to treat it as a valid user.
      return AuthOutcome::kBadCredentials;
    }
    if (record.validation_error().has_value() || record.name != name) {
      return AuthOutcome::kBadCredentials;
    }
    if (!record.enabled) {
      // Still hash, so a disabled user is not distinguishable by timing from an
      // enabled one with the wrong password.
      return reject_with_constant_work(password);
    }
    if (!constant_time_equal(record.pw_sha256_hex,
                             hash_password(record.salt_hex, password))) {
      return AuthOutcome::kBadCredentials;
    }

    out = context_from(record);
    return AuthOutcome::kOk;
  }

private:
  /**
   * @brief Extract and decode the base64 payload of a Basic credential.
   * @return The decoded "user:password" bytes, or nullopt if the header was
   *         absent, used another scheme, or did not decode.
   */
  [[nodiscard]] static std::optional<std::string>
  decode_basic_credential(const std::string &authorization) {
    const std::string scheme(kBasicScheme);
    // "Basic " — the scheme token, a space, then the credential.
    if (authorization.size() <= scheme.size() + 1) {
      return std::nullopt;
    }
    if (ascii_lower(authorization.substr(0, scheme.size())) != scheme) {
      return std::nullopt;
    }
    if (authorization[scheme.size()] != ' ') {
      return std::nullopt;
    }
    return base64_decode(
        trim_header_value(authorization.substr(scheme.size() + 1)));
  }

  /**
   * @brief Reject, having done the hashing work a real check would.
   *
   * An unknown or disabled user that returned immediately would take measurably
   * less time than a known one with a wrong password — a user-enumeration
   * oracle, where an attacker learns which names exist without ever
   * authenticating. Hashing against a throwaway salt costs one SHA-256 and
   * removes the dominant term of that difference. The result is discarded on
   * purpose; the volatile write keeps a compiler from deciding the call is
   * dead.
   *
   * IT DOES NOT MAKE THE TWO PATHS BIT-FOR-BIT EQUAL IN TIME, and this comment
   * used to overstate that. A name that IS present additionally costs a small
   * msgpack decode plus validation (below) that a name that is absent does not,
   * so a residual differential remains. It is sub-microsecond against an HTTP
   * round trip and almost certainly buried in network and scheduler jitter, but
   * it is not zero — do not treat this function as a complete guarantee, and if
   * that ever matters, close it by decoding a dummy record here too rather than
   * by editing this note.
   */
  [[nodiscard]] AuthOutcome
  reject_with_constant_work(const std::string &password) const {
    static volatile unsigned char sink = 0;
    const std::string digest = hash_password(admin_salt_hex_, password);
    sink = static_cast<unsigned char>(digest.empty() ? 0 : digest[0]);
    (void)sink;
    return AuthOutcome::kBadCredentials;
  }

  /** @brief The bootstrap admin holds every class over every key. */
  [[nodiscard]] static AuthContext admin_context() {
    AuthContext context;
    context.name = kBootstrapAdminName;
    context.read = true;
    context.write = true;
    context.admin = true;
    context.patterns = {"*"};
    return context;
  }

  [[nodiscard]] static AuthContext context_from(const UserRecord &record) {
    AuthContext context;
    context.name = record.name;
    context.read = record.has_class(kClassRead);
    context.write = record.has_class(kClassWrite);
    context.admin = record.has_class(kClassAdmin);
    context.patterns = record.patterns;
    return context;
  }

  const IKVStore &store_;
  bool enabled_;
  std::string admin_salt_hex_;
  std::string admin_pw_hash_;
};

} // namespace auth
} // namespace kvdb
