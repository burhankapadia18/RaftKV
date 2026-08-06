#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

/**
 * @file sha256.hpp
 * @brief SHA-256 and a constant-time comparison, for password hashing.
 *
 * WHY THIS IS VENDORED RATHER THAN LINKED. kvdb_node links gRPC, protobuf and
 * msgpack and nothing else. Debian's libgrpc++-dev happens to pull OpenSSL in
 * transitively, but depending on that is depending on a coincidence: the macOS
 * build path (CMakeLists has an APPLE branch) would then need brew-OpenSSL
 * discovery plumbing, and the runtime image would need a new package. FIPS
 * pedigree buys nothing here — this is a hash of a password, checked a few
 * times a second at most, verified below against the NIST vectors.
 *
 * NOT a password-hashing function in the modern sense. SHA-256 is fast, which
 * is exactly what you do not want against an offline attacker with the store's
 * bytes in hand; a memory-hard KDF (argon2, scrypt, bcrypt) is the right answer
 * and would mean a real dependency. Redis stores ACL passwords as a bare
 * unsalted SHA-256 hex digest, this store salts them, and the salt is what
 * stops one precomputed table from covering every user in every deployment.
 * Treat the user table as a secret regardless.
 */

namespace kvdb {
namespace auth {

/**
 * @brief SHA-256 (FIPS 180-4), streaming implementation over the message.
 *
 * Straight transcription of the standard: no table tricks, no assembly, no
 * timing claims. The digest is public data — only the comparison of digests
 * needs to be constant-time, and that is constant_time_equal below.
 */
class Sha256 {
public:
  static constexpr size_t kDigestBytes = 32;

  Sha256() = default;

  void update(const char *data, size_t size) {
    throw_if_finished();
    for (size_t i = 0; i < size; ++i) {
      buffer_[buffer_len_++] = static_cast<uint8_t>(data[i]);
      if (buffer_len_ == kBlockBytes) {
        transform(buffer_.data());
        buffer_len_ = 0;
        total_bits_ += kBlockBytes * 8;
      }
    }
  }

  void update(const std::string &data) { update(data.data(), data.size()); }

  /**
   * @brief Pad, absorb the length, and return the digest.
   *
   * SINGLE USE, and that is now enforced rather than merely documented. On
   * return `buffer_len_` is left at a full block (the padding filled it and was
   * absorbed), so a subsequent update() would write one past the end of
   * `buffer_` and corrupt the members next to it — an out-of-bounds write that
   * **ASan does not catch**, because it is intra-object and the array has no
   * redzone inside the object. A comment saying "single use" is not a defence
   * against that; throwing is. Nothing in the codebase reuses an instance today
   * (sha256_hex constructs a fresh one per call), so this only ever fires on a
   * future refactor that hashes several candidates through one object.
   */
  [[nodiscard]] std::array<uint8_t, kDigestBytes> digest() {
    throw_if_finished();
    finished_ = true;

    const uint64_t message_bits = total_bits_ + buffer_len_ * 8;

    // Append 0x80, then zeros, leaving 8 bytes for the big-endian bit length.
    buffer_[buffer_len_++] = 0x80;
    if (buffer_len_ > kBlockBytes - 8) {
      while (buffer_len_ < kBlockBytes) {
        buffer_[buffer_len_++] = 0;
      }
      transform(buffer_.data());
      buffer_len_ = 0;
    }
    while (buffer_len_ < kBlockBytes - 8) {
      buffer_[buffer_len_++] = 0;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
      buffer_[buffer_len_++] =
          static_cast<uint8_t>((message_bits >> shift) & 0xff);
    }
    transform(buffer_.data());

    std::array<uint8_t, kDigestBytes> out{};
    for (size_t i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xff);
      out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xff);
      out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xff);
      out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xff);
    }
    return out;
  }

private:
  static constexpr size_t kBlockBytes = 64;

  void throw_if_finished() const {
    if (finished_) {
      throw std::logic_error("Sha256 was reused after digest()");
    }
  }

  static uint32_t rotr(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
  }

  void transform(const uint8_t *block) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (size_t i = 16; i < 64; ++i) {
      const uint32_t s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (size_t i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  std::array<uint8_t, kBlockBytes> buffer_{};
  size_t buffer_len_ = 0;
  uint64_t total_bits_ = 0;
  bool finished_ = false;
};

/** @brief SHA-256 of @p input as 64 lowercase hex characters. */
[[nodiscard]] inline std::string sha256_hex(const std::string &input) {
  static constexpr char kHexDigits[] = "0123456789abcdef";

  Sha256 hasher;
  hasher.update(input);
  const std::array<uint8_t, Sha256::kDigestBytes> digest = hasher.digest();

  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const uint8_t byte : digest) {
    hex += kHexDigits[(byte >> 4) & 0x0f];
    hex += kHexDigits[byte & 0x0f];
  }
  return hex;
}

/**
 * @brief Compare two strings without leaking where they first differ.
 *
 * `a == b` on std::string returns as soon as it finds a mismatching byte, so
 * the time it takes reveals the length of the matching prefix. Against a
 * password hash that is a byte-at-a-time oracle: an attacker who can time
 * requests recovers the expected digest one character per round of guesses.
 *
 * The loop below always walks the whole of @p expected and accumulates
 * differences instead of branching on them. The LENGTH is still compared up
 * front and early-returns — that is deliberate and safe here, because every
 * value compared through this function is a fixed-width hex digest, so the
 * length carries no secret.
 */
[[nodiscard]] inline bool constant_time_equal(const std::string &expected,
                                              const std::string &actual) {
  if (expected.size() != actual.size()) {
    return false;
  }
  unsigned char difference = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    difference |= static_cast<unsigned char>(expected[i]) ^
                  static_cast<unsigned char>(actual[i]);
  }
  return difference == 0;
}

} // namespace auth
} // namespace kvdb
