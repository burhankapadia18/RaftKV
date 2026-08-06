#pragma once

#include <cstdint>
#include <fcntl.h>
#include <msgpack.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "../storage/atomic_file.hpp"
#include "sha256.hpp"

/**
 * @file user_record.hpp
 * @brief The persisted shape of a user, and the reserved key space it lives in.
 */

namespace kvdb {
namespace auth {

/**
 * @brief Prefix of every key this system owns rather than a client.
 *
 * Enforced in TWO places, and both are load-bearing:
 *   - KVHttpHandler refuses any data route naming a key under it, reads
 *     included (a local read of a user record would hand out password hashes);
 *   - StateMachineService::Apply refuses a plain SET/DELETE under it, which is
 *     what stops the unauthenticated sidecar port (50052) from rewriting the
 *     user table with an ordinary write.
 *
 * Two underscores and a colon: keys are arbitrary bytes in this store, so no
 * prefix can be truly unavailable to clients. This one is refused rather than
 * escaped, so a client that used it before now gets a 403 — see the CHANGELOG.
 */
inline constexpr const char *kSysPrefix = "__sys:";

/** @brief Where a user record is stored: kUserKeyPrefix + user name. */
inline constexpr const char *kUserKeyPrefix = "__sys:user:";

/** @brief Command classes a user may hold. Independent, not a hierarchy. */
inline constexpr const char *kClassRead = "read";
inline constexpr const char *kClassWrite = "write";
inline constexpr const char *kClassAdmin = "admin";

/** @brief True when @p key belongs to the reserved system key space. */
[[nodiscard]] inline bool is_reserved_key(const std::string &key) {
  return key.rfind(kSysPrefix, 0) == 0;
}

/**
 * @brief Explain why @p name is not a usable user name.
 *
 * The character set is deliberately narrow — [A-Za-z0-9_.-] — for two concrete
 * reasons, not tidiness:
 *   - RFC 7617 splits a Basic credential at the FIRST colon, so a name
 *     containing ':' cannot be authenticated unambiguously;
 *   - the storage key is kUserKeyPrefix + name, so restricting the name is what
 *     guarantees a name cannot be crafted to land outside its own key.
 *
 * Checked at the HTTP boundary AND again in Apply: the second check is a pure
 * function of the committed entry's bytes, so every replica reaches the same
 * verdict, which is what keeps a crafted propose from creating a name the HTTP
 * layer would never have accepted.
 */
[[nodiscard]] inline std::optional<std::string>
username_error(const std::string &name) {
  static constexpr size_t kMaxNameBytes = 128;

  if (name.empty()) {
    return "user name must not be empty";
  }
  if (name.size() > kMaxNameBytes) {
    return "user name must be at most 128 bytes";
  }
  for (const char c : name) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                         c == '-';
    if (!allowed) {
      return "user name may only contain letters, digits, '_', '.' and '-'";
    }
  }
  return std::nullopt;
}

/** @brief Convenience wrapper over username_error(). */
[[nodiscard]] inline bool valid_username(const std::string &name) {
  return !username_error(name).has_value();
}

/** @brief The store key holding @p name's record. */
[[nodiscard]] inline std::string user_storage_key(const std::string &name) {
  return std::string(kUserKeyPrefix) + name;
}

/**
 * @brief Salted SHA-256 of a password, as hex.
 *
 * The salt is joined with ':' rather than hex-decoded and prepended, which
 * needs no decoder and is equally effective: salt_hex is fixed-width hex, so
 * the concatenation is unambiguous and no two (salt, password) pairs collide.
 *
 * See sha256.hpp for why this is SHA-256 and not a memory-hard KDF, and what
 * that means for an attacker holding the store's bytes.
 */
[[nodiscard]] inline std::string hash_password(const std::string &salt_hex,
                                               const std::string &password) {
  return sha256_hex(salt_hex + ":" + password);
}

/**
 * @brief 16 fresh random bytes as 32 hex characters.
 *
 * From /dev/urandom, not rand() or a time seed: a predictable salt is no salt,
 * because it lets one precomputed table cover every deployment. Throws rather
 * than falling back to something weaker — a caller that cannot get randomness
 * must fail the request, and KVHttpHandler turns this into a 500.
 *
 * Called only on the node serving the admin request, never inside Apply: the
 * salt is part of the committed bytes, so every replica must store the same
 * one. Generating it during Apply would give each replica a different record.
 */
[[nodiscard]] inline std::string generate_salt_hex() {
  static constexpr size_t kSaltBytes = 16;
  static constexpr char kHexDigits[] = "0123456789abcdef";

  fileio::FdGuard fd(::open("/dev/urandom", O_RDONLY));
  if (!fd.valid()) {
    throw std::runtime_error("cannot open /dev/urandom");
  }

  unsigned char bytes[kSaltBytes];
  size_t filled = 0;
  while (filled < kSaltBytes) {
    const ssize_t n = ::read(fd.get(), bytes + filled, kSaltBytes - filled);
    if (n <= 0) {
      // A short read is normal and is topped up; 0 or -1 is not, and guessing
      // would mean silently using fewer random bytes than intended.
      throw std::runtime_error("short read from /dev/urandom");
    }
    filled += static_cast<size_t>(n);
  }

  std::string hex;
  hex.reserve(kSaltBytes * 2);
  for (const unsigned char byte : bytes) {
    hex += kHexDigits[(byte >> 4) & 0x0f];
    hex += kHexDigits[byte & 0x0f];
  }
  return hex;
}

/**
 * @brief A user, as replicated through Raft and held in the store.
 *
 * Carried as msgpack in KVCommand::value, NOT as new KVCommand fields. That
 * keeps MSGPACK_DEFINE_MAP(op, key, value) exactly as it was, so every raft
 * entry and WAL record ever written stays decodable — the field set of that
 * struct is simultaneously a wire format and an on-disk format.
 */
struct UserRecord {
  /**
   * @brief Format version. Only 1 exists; anything else is REJECTED.
   *
   * Rejected rather than best-effort decoded, and deterministically so (every
   * replica refuses the same entry). Half-understanding a future record would
   * mean enforcing an ACL that is not the one the operator wrote — strictly
   * worse than refusing it. A record with no version field at all decodes as 1,
   * which is correct: version 1 is what existed before the field was ever
   * omitted.
   */
  std::int64_t version = 1;

  /** @brief The user name. Must equal the command's key — see Apply. */
  std::string name;

  /** @brief 32 hex characters from generate_salt_hex(). */
  std::string salt_hex;

  /** @brief hash_password(salt_hex, password), 64 hex characters. */
  std::string pw_sha256_hex;

  /** @brief A disabled user authenticates as nobody but keeps its ACL. */
  bool enabled = true;

  /** @brief Held command classes: any of "read", "write", "admin". */
  std::vector<std::string> classes;

  /**
   * @brief Glob patterns naming the keys this user may touch.
   *
   * An EMPTY list denies every key. That is the safe direction for the default:
   * a record that lost its patterns grants nothing rather than everything.
   */
  std::vector<std::string> patterns;

  MSGPACK_DEFINE_MAP(version, name, salt_hex, pw_sha256_hex, enabled, classes,
                     patterns);

  /** @brief Current format version written by this build. */
  static constexpr std::int64_t kCurrentVersion = 1;

  /**
   * @brief Structural limits applied while decoding — see KVCommand's note.
   *
   * Same threat, same reasoning: these bytes arrive over Raft and off disk, and
   * the decode happens on every replica for an already-committed entry. Tighter
   * than KVCommand's because a user record is small; 64 KiB of strings and 64
   * array entries is far more than any real ACL and far less than a useful
   * allocation bomb.
   */
  static constexpr size_t kMaxMapEntries = 64;
  static constexpr size_t kMaxArrayEntries = 64;
  static constexpr size_t kMaxStringBytes = 64u * 1024;
  static constexpr size_t kMaxDepth = 4;

  /** @brief Serialize to the msgpack map form. */
  [[nodiscard]] std::string to_msgpack() const {
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, *this);
    return std::string(buffer.data(), buffer.size());
  }

  /**
   * @brief Decode a record, refusing oversized or deeply nested input.
   * @throws std::exception on malformed or over-large msgpack.
   */
  [[nodiscard]] static UserRecord from_msgpack(const char *data, size_t size) {
    UserRecord record;

    const msgpack::unpack_limit limit(kMaxArrayEntries, kMaxMapEntries,
                                      kMaxStringBytes, kMaxStringBytes,
                                      kMaxStringBytes, kMaxDepth);

    size_t offset = 0;
    msgpack::object_handle oh = msgpack::unpack(
        data, size, offset, MSGPACK_NULLPTR, MSGPACK_NULLPTR, limit);
    oh.get().convert(record);
    return record;
  }

  /**
   * @brief Explain why this record must not be installed.
   *
   * Every rule here is a pure function of the record's own bytes, so all
   * replicas agree — that is what makes a rejection at Apply time deterministic
   * rather than a divergence.
   */
  [[nodiscard]] std::optional<std::string> validation_error() const {
    if (version != kCurrentVersion) {
      return "unsupported user record version: " + std::to_string(version);
    }
    if (const std::optional<std::string> bad = username_error(name)) {
      return *bad;
    }
    if (salt_hex.size() != 32) {
      return "salt must be 32 hex characters";
    }
    if (pw_sha256_hex.size() != Sha256::kDigestBytes * 2) {
      return "password hash must be 64 hex characters";
    }
    if (!is_lowercase_hex(salt_hex) || !is_lowercase_hex(pw_sha256_hex)) {
      return "salt and password hash must be lowercase hex";
    }
    if (classes.size() > kMaxArrayEntries ||
        patterns.size() > kMaxArrayEntries) {
      return "too many classes or patterns";
    }
    for (const std::string &cls : classes) {
      if (cls != kClassRead && cls != kClassWrite && cls != kClassAdmin) {
        return "unknown command class: \"" + cls + "\"";
      }
    }
    return std::nullopt;
  }

  /** @brief True when this user holds @p cls. */
  [[nodiscard]] bool has_class(const std::string &cls) const {
    for (const std::string &held : classes) {
      if (held == cls) {
        return true;
      }
    }
    return false;
  }

private:
  [[nodiscard]] static bool is_lowercase_hex(const std::string &text) {
    for (const char c : text) {
      const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      if (!ok) {
        return false;
      }
    }
    return true;
  }
};

/**
 * @brief The admin API's request body for creating or replacing a user.
 *
 * msgpack, like every other body this server accepts: no JSON parser is linked
 * in, and the e2e suite already speaks msgpack.
 *
 * This is a REQUEST shape, distinct from UserRecord: it carries a cleartext
 * password and no salt, because the salt and hash are computed server-side. A
 * client never sends and never sees a hash.
 */
struct UserUpsertRequest {
  std::string password;
  bool enabled = true;
  std::vector<std::string> classes;
  std::vector<std::string> patterns;

  MSGPACK_DEFINE_MAP(password, enabled, classes, patterns);

  [[nodiscard]] static UserUpsertRequest from_msgpack(const char *data,
                                                      size_t size) {
    UserUpsertRequest request;

    const msgpack::unpack_limit limit(
        UserRecord::kMaxArrayEntries, UserRecord::kMaxMapEntries,
        UserRecord::kMaxStringBytes, UserRecord::kMaxStringBytes,
        UserRecord::kMaxStringBytes, UserRecord::kMaxDepth);

    size_t offset = 0;
    msgpack::object_handle oh = msgpack::unpack(
        data, size, offset, MSGPACK_NULLPTR, MSGPACK_NULLPTR, limit);
    oh.get().convert(request);
    return request;
  }

  /**
   * @brief Explain why this request cannot be turned into a record.
   *
   * A minimum password length is enforced here rather than left to the
   * operator: this hash is not memory-hard, so a short password is brute-forced
   * from the store's bytes in seconds.
   */
  [[nodiscard]] std::optional<std::string> validation_error() const {
    static constexpr size_t kMinPasswordBytes = 8;

    if (password.size() < kMinPasswordBytes) {
      return "password must be at least 8 bytes";
    }
    if (classes.size() > UserRecord::kMaxArrayEntries ||
        patterns.size() > UserRecord::kMaxArrayEntries) {
      return "at most 64 classes and 64 patterns";
    }
    for (const std::string &cls : classes) {
      if (cls != kClassRead && cls != kClassWrite && cls != kClassAdmin) {
        return "unknown command class: \"" + cls + "\"";
      }
    }
    for (const std::string &pattern : patterns) {
      if (pattern.size() > UserRecord::kMaxStringBytes) {
        return "pattern is too long";
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Build the record to replicate, with a fresh salt.
   * @throws std::runtime_error if randomness is unavailable.
   */
  [[nodiscard]] UserRecord to_record(const std::string &name) const {
    UserRecord record;
    record.version = UserRecord::kCurrentVersion;
    record.name = name;
    record.salt_hex = generate_salt_hex();
    record.pw_sha256_hex = hash_password(record.salt_hex, password);
    record.enabled = enabled;
    record.classes = classes;
    record.patterns = patterns;
    return record;
  }
};

} // namespace auth
} // namespace kvdb
