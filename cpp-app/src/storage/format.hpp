#pragma once

/**
 * @file format.hpp
 * @brief Little-endian encode/decode primitives and CRC32 for the on-disk
 *        formats introduced in Phase 2 (R2.1, R2.4).
 *
 * Two on-disk layouts are built out of these helpers:
 *
 *   Base file (R2.1)   "KVB1" | u32 entry_count | (blob key | blob value)*
 *   WAL record (R2.4)  u32 payload_len | payload | u32 crc32(payload)
 *
 * Everything here is pure: no I/O, no globals, no allocation beyond the output
 * string. Byte order is little-endian and is part of the file format contract,
 * not an implementation detail - format_test.cpp asserts the exact bytes.
 *
 * The decoders parse bytes that have been sitting on disk and originate from a
 * client payload, so they are a trust boundary. Every length read out of the
 * buffer is bounds-checked against what is actually available before a single
 * byte of it is touched, and a decode that fails leaves both the cursor and
 * the output untouched so a caller can stop cleanly (the WAL torn-tail heal in
 * R2.5 depends on that).
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace kvdb {
namespace format {

/** @brief Width of every length/count field in the Phase 2 formats. */
inline constexpr size_t kU32Size = 4;

namespace detail {

/**
 * @brief Reflected form of the IEEE 802.3 CRC-32 polynomial.
 *
 * This is the same polynomial zlib, gzip and PNG use, so the checksums written
 * into the WAL can be verified with any off-the-shelf tool.
 */
inline constexpr uint32_t kCrc32Polynomial = 0xEDB88320u;

/** @brief Initial and final-xor value of the CRC-32 register. */
inline constexpr uint32_t kCrc32Seed = 0xFFFFFFFFu;

using Crc32Table = std::array<uint32_t, 256>;

/**
 * @brief Build the byte-wise CRC-32 lookup table.
 *
 * Evaluated at compile time, so there is no mutable global state and no
 * first-use race between the HTTP thread and the gRPC apply thread.
 */
[[nodiscard]] constexpr Crc32Table make_crc32_table() {
  Crc32Table table{};
  for (uint32_t index = 0; index < 256u; ++index) {
    uint32_t remainder = index;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 1u) ? (kCrc32Polynomial ^ (remainder >> 1))
                                   : (remainder >> 1);
    }
    table[index] = remainder;
  }
  return table;
}

inline constexpr Crc32Table kCrc32Table = make_crc32_table();

} // namespace detail

/**
 * @brief Append @p value as 4 little-endian bytes.
 */
inline void append_u32(std::string &out, uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

/**
 * @brief Read 4 little-endian bytes at @p offset and advance it past them.
 *
 * @param in     Buffer to decode from.
 * @param offset Cursor into @p in; advanced by 4 only on success.
 * @param out    Decoded value; untouched on failure.
 * @return false when fewer than 4 bytes remain at @p offset (including an
 *         @p offset that is already past the end of @p in).
 */
[[nodiscard]] inline bool read_u32(const std::string &in, size_t &offset,
                                   uint32_t &out) {
  // Checked in this order so the subtraction below cannot wrap: a caller with
  // a corrupt cursor must not be turned into an out-of-bounds read.
  if (offset > in.size() || in.size() - offset < kU32Size) {
    return false;
  }
  const auto byte_at = [&in](size_t index) {
    return static_cast<uint32_t>(static_cast<unsigned char>(in[index]));
  };
  out = byte_at(offset) | (byte_at(offset + 1) << 8) |
        (byte_at(offset + 2) << 16) | (byte_at(offset + 3) << 24);
  offset += kU32Size;
  return true;
}

/**
 * @brief Append a uint32 length prefix followed by the raw bytes of @p blob.
 *
 * @throws std::length_error if @p blob is longer than a uint32 can describe.
 *         Truncating the prefix instead would silently corrupt the file, which
 *         is exactly the failure mode this format exists to prevent.
 */
inline void append_blob(std::string &out, const std::string &blob) {
  if (blob.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("blob exceeds the uint32 length prefix");
  }
  append_u32(out, static_cast<uint32_t>(blob.size()));
  out.append(blob);
}

/**
 * @brief Read a length-prefixed blob at @p offset and advance past it.
 *
 * @param in     Buffer to decode from.
 * @param offset Cursor into @p in; advanced past prefix and payload only on
 *               success.
 * @param out    Decoded bytes; untouched on failure.
 * @return false when the length prefix is missing or truncated, or when the
 *         declared length overruns the buffer. The declared length is
 *         attacker-influenced and is never trusted before that check.
 */
[[nodiscard]] inline bool read_blob(const std::string &in, size_t &offset,
                                    std::string &out) {
  size_t cursor = offset;
  uint32_t length = 0;
  if (!read_u32(in, cursor, length)) {
    return false;
  }
  // read_u32 succeeded, so cursor <= in.size() and this cannot wrap. Compare in
  // 64 bits so a 32-bit size_t build cannot overflow the widened length either.
  const size_t remaining = in.size() - cursor;
  if (static_cast<uint64_t>(length) > static_cast<uint64_t>(remaining)) {
    return false;
  }
  out.assign(in, cursor, static_cast<size_t>(length));
  offset = cursor + static_cast<size_t>(length);
  return true;
}

/**
 * @brief CRC-32 (IEEE 802.3, reflected) over @p size bytes at @p data.
 *
 * Matches zlib's crc32(): crc32("") == 0, crc32("123456789") == 0xCBF43926.
 * @p data may be null when @p size is 0.
 */
[[nodiscard]] inline uint32_t crc32(const char *data, size_t size) {
  uint32_t remainder = detail::kCrc32Seed;
  for (size_t i = 0; i < size; ++i) {
    const auto byte = static_cast<unsigned char>(data[i]);
    remainder =
        detail::kCrc32Table[(remainder ^ byte) & 0xFFu] ^ (remainder >> 8);
  }
  return remainder ^ detail::kCrc32Seed;
}

/**
 * @brief CRC-32 over the whole string, embedded NUL bytes included.
 */
[[nodiscard]] inline uint32_t crc32(const std::string &s) {
  return crc32(s.data(), s.size());
}

} // namespace format
} // namespace kvdb
