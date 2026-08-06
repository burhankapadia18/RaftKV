#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

/**
 * @file base64.hpp
 * @brief Strict base64, for the credential in an HTTP Basic Authorization
 *        header (RFC 4648 alphabet, RFC 7617 credential encoding).
 */

namespace kvdb {
namespace auth {

/**
 * @brief Decode standard base64, refusing anything malformed.
 *
 * STRICT ON PURPOSE, the same argument as url_decode: a decoder that skips
 * characters it does not recognise makes several distinct inputs decode to the
 * same credential, and one of them may be the one an attacker can reach. So a
 * character outside the alphabet, a length that is not a multiple of four,
 * padding in the middle, and a lone leftover character are all nullopt rather
 * than a best guess. Whitespace is NOT accepted either — the header value has
 * already been trimmed by the parser, and interior whitespace in a credential
 * is not something a conforming client sends.
 *
 * @return The decoded bytes, or nullopt if @p input is not valid base64.
 */
[[nodiscard]] inline std::optional<std::string>
base64_decode(const std::string &input) {
  const auto sextet = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return -1;
  };

  if (input.size() % 4 != 0) {
    return std::nullopt;
  }

  std::string out;
  out.reserve(input.size() / 4 * 3);

  for (size_t i = 0; i < input.size(); i += 4) {
    // Padding is legal only in the last group, and only as "=" or "==".
    size_t padding = 0;
    if (input[i + 3] == '=') {
      ++padding;
      if (input[i + 2] == '=') {
        ++padding;
      }
    }
    if (padding != 0 && i + 4 != input.size()) {
      return std::nullopt;
    }

    int values[4];
    for (size_t j = 0; j < 4; ++j) {
      if (j >= 4 - padding) {
        values[j] = 0;
        continue;
      }
      values[j] = sextet(input[i + j]);
      if (values[j] < 0) {
        return std::nullopt;
      }
    }

    const uint32_t group = (static_cast<uint32_t>(values[0]) << 18) |
                           (static_cast<uint32_t>(values[1]) << 12) |
                           (static_cast<uint32_t>(values[2]) << 6) |
                           static_cast<uint32_t>(values[3]);

    out += static_cast<char>((group >> 16) & 0xff);
    if (padding < 2) {
      out += static_cast<char>((group >> 8) & 0xff);
    }
    if (padding < 1) {
      out += static_cast<char>(group & 0xff);
    }
  }

  return out;
}

/**
 * @brief Encode bytes as standard base64 with padding.
 *
 * Used by the tests to build credentials, and by nothing on the request path —
 * this server never emits base64.
 */
[[nodiscard]] inline std::string base64_encode(const std::string &input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve((input.size() + 2) / 3 * 4);

  for (size_t i = 0; i < input.size(); i += 3) {
    const size_t remaining = input.size() - i;
    const uint32_t b0 = static_cast<unsigned char>(input[i]);
    const uint32_t b1 =
        remaining > 1 ? static_cast<unsigned char>(input[i + 1]) : 0u;
    const uint32_t b2 =
        remaining > 2 ? static_cast<unsigned char>(input[i + 2]) : 0u;
    const uint32_t group = (b0 << 16) | (b1 << 8) | b2;

    out += kAlphabet[(group >> 18) & 0x3f];
    out += kAlphabet[(group >> 12) & 0x3f];
    out += remaining > 1 ? kAlphabet[(group >> 6) & 0x3f] : '=';
    out += remaining > 2 ? kAlphabet[group & 0x3f] : '=';
  }

  return out;
}

} // namespace auth
} // namespace kvdb
