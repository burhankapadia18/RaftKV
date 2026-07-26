/**
 * @file format_test.cpp
 * @brief Unit tests for the on-disk encoding primitives (spec R2.1/R2.4).
 *
 * Two things are being pinned here, and they are pinned differently on purpose:
 *
 *   1. The EXACT BYTES. Little-endian ordering is part of the file format
 *      contract - a base file written by one build has to be readable by the
 *      next one - so the expected buffers below are spelled out by hand rather
 *      than produced by calling the encoder. A round-trip test alone would pass
 *      just as happily if both sides flipped to big-endian.
 *
 *   2. The HOSTILE DECODE CASES. read_u32/read_blob parse bytes off disk whose
 *      length fields ultimately came from a client payload. A declared length
 *      is never trustworthy, so every truncation and overrun case must return
 *      false rather than read past the end of the buffer. These tests are run
 *      under ASan in CI, which is what actually enforces the "rather than".
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "storage/format.hpp"

namespace kvdb {
namespace format {
namespace {

/**
 * @brief std::string from a string literal, embedded NUL bytes included.
 *
 * Every expected buffer below is spelled out as a literal rather than produced
 * by the encoder under test - that is what makes these byte-order assertions
 * worth anything. This keeps them readable instead of drowning in
 * std::string(pointer, length) noise.
 */
template <size_t N> std::string bytes(const char (&literal)[N]) {
  return std::string(literal, N - 1);
}

// --- append_u32: exact on-disk byte order ---------------------------------

TEST(FormatU32Test, AppendWritesFourLittleEndianBytes) {
  std::string out;

  append_u32(out, 0x01020304u);

  ASSERT_EQ(out.size(), 4u);
  // Least significant byte first. This literal IS the format spec.
  EXPECT_EQ(out, bytes("\x04\x03\x02\x01"));
}

TEST(FormatU32Test, AppendEncodesBoundaryValuesLittleEndian) {
  struct Case {
    uint32_t value;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {0u, bytes("\x00\x00\x00\x00")},
      {1u, bytes("\x01\x00\x00\x00")},
      {0xFFu, bytes("\xFF\x00\x00\x00")},
      {0x100u, bytes("\x00\x01\x00\x00")},
      {0x7FFFFFFFu, bytes("\xFF\xFF\xFF\x7F")},
      {0x80000000u, bytes("\x00\x00\x00\x80")},
      {0xFFFFFFFFu, bytes("\xFF\xFF\xFF\xFF")},
  };

  for (const Case &test_case : cases) {
    std::string out;
    append_u32(out, test_case.value);
    EXPECT_EQ(out, test_case.expected) << "value=" << test_case.value;
  }
}

TEST(FormatU32Test, AppendsToAnExistingBufferWithoutClobberingIt) {
  std::string out = "KVB1";

  append_u32(out, 1u);

  EXPECT_EQ(out, "KVB1" + bytes("\x01\x00\x00\x00"));
}

// --- read_u32: round trip and cursor behaviour ----------------------------

TEST(FormatU32Test, RoundTripsBoundaryValues) {
  const std::vector<uint32_t> values = {0u,          1u,          0xFFu,
                                        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};

  for (uint32_t value : values) {
    std::string buffer;
    append_u32(buffer, value);
    ASSERT_EQ(buffer.size(), 4u) << "value=" << value;

    size_t offset = 0;
    uint32_t decoded = 0;
    ASSERT_TRUE(read_u32(buffer, offset, decoded)) << "value=" << value;
    EXPECT_EQ(decoded, value);
    EXPECT_EQ(offset, 4u);
  }
}

TEST(FormatU32Test, DecodesAHandWrittenLittleEndianBuffer) {
  // The mirror image of AppendWritesFourLittleEndianBytes: if the decoder ever
  // disagreed with the encoder about byte order, only one of the two tests
  // could stay green.
  const std::string buffer = bytes("\x04\x03\x02\x01");

  size_t offset = 0;
  uint32_t decoded = 0;

  ASSERT_TRUE(read_u32(buffer, offset, decoded));
  EXPECT_EQ(decoded, 0x01020304u);
}

TEST(FormatU32Test, SequentialReadsAdvanceTheOffset) {
  std::string buffer;
  append_u32(buffer, 7u);
  append_u32(buffer, 0xFFFFFFFFu);

  size_t offset = 0;
  uint32_t first = 0;
  uint32_t second = 0;

  ASSERT_TRUE(read_u32(buffer, offset, first));
  EXPECT_EQ(offset, 4u);
  ASSERT_TRUE(read_u32(buffer, offset, second));
  EXPECT_EQ(offset, 8u);
  EXPECT_EQ(first, 7u);
  EXPECT_EQ(second, 0xFFFFFFFFu);

  // Cursor now sits exactly at the end; one more read must fail cleanly.
  uint32_t third = 0;
  EXPECT_FALSE(read_u32(buffer, offset, third));
  EXPECT_EQ(offset, 8u);
}

// --- read_u32: truncation ---------------------------------------------------

TEST(FormatU32Test, ReadFromAnEmptyBufferFails) {
  const std::string empty;
  size_t offset = 0;
  uint32_t decoded = 0xDEADBEEFu;

  EXPECT_FALSE(read_u32(empty, offset, decoded));

  EXPECT_EQ(offset, 0u);           // cursor untouched on failure
  EXPECT_EQ(decoded, 0xDEADBEEFu); // output untouched on failure
}

TEST(FormatU32Test, ReadWithThreeOfFourBytesAvailableFails) {
  const std::string three_bytes = bytes("\x01\x02\x03");
  size_t offset = 0;
  uint32_t decoded = 0xDEADBEEFu;

  EXPECT_FALSE(read_u32(three_bytes, offset, decoded));

  EXPECT_EQ(offset, 0u);
  EXPECT_EQ(decoded, 0xDEADBEEFu);
}

TEST(FormatU32Test, ReadWithFewerThanFourBytesLeftAfterTheOffsetFails) {
  const std::string buffer = bytes("\x01\x02\x03\x04");

  for (size_t offset_start = 1; offset_start <= 4; ++offset_start) {
    size_t offset = offset_start;
    uint32_t decoded = 0xDEADBEEFu;
    EXPECT_FALSE(read_u32(buffer, offset, decoded))
        << "offset=" << offset_start;
    EXPECT_EQ(offset, offset_start);
    EXPECT_EQ(decoded, 0xDEADBEEFu);
  }
}

TEST(FormatU32Test, ReadWithAnOffsetPastTheEndFails) {
  // A corrupt or stale cursor must be rejected, not turned into an
  // out-of-bounds read (the size() - offset subtraction would wrap).
  const std::string buffer = bytes("\x01\x02\x03\x04");
  size_t offset = 99;
  uint32_t decoded = 0xDEADBEEFu;

  EXPECT_FALSE(read_u32(buffer, offset, decoded));

  EXPECT_EQ(offset, 99u);
  EXPECT_EQ(decoded, 0xDEADBEEFu);
}

// --- append_blob / read_blob ----------------------------------------------

TEST(FormatBlobTest, AppendWritesLittleEndianLengthThenTheRawBytes) {
  std::string out;

  append_blob(out, "hi");

  EXPECT_EQ(out, bytes("\x02\x00\x00\x00") + "hi");
}

TEST(FormatBlobTest, EmptyBlobIsALengthPrefixAndNothingElse) {
  std::string out;

  append_blob(out, "");

  EXPECT_EQ(out, bytes("\x00\x00\x00\x00"));

  size_t offset = 0;
  std::string decoded = "sentinel";
  ASSERT_TRUE(read_blob(out, offset, decoded));
  EXPECT_EQ(decoded, "");
  EXPECT_EQ(offset, 4u);
}

TEST(FormatBlobTest, RoundTripsBinaryHostileContent) {
  // '=' and '\n' are exactly what corrupted the Phase 0 line format, and NUL is
  // what a naive C-string implementation would truncate on. Length prefixing
  // means none of them are special any more.
  const std::vector<std::string> blobs = {
      "",
      "plain",
      bytes("\0"),
      bytes("a\0b"),
      bytes("\0\0\0\0"),
      "a=b=c",
      "=",
      "line1\nline2",
      "trailing\n",
      "\r\n",
      bytes("=\n\0="),
      std::string(70000, 'x'),
  };

  for (const std::string &blob : blobs) {
    std::string buffer;
    append_blob(buffer, blob);
    ASSERT_EQ(buffer.size(), blob.size() + 4u) << "blob size " << blob.size();

    size_t offset = 0;
    std::string decoded;
    ASSERT_TRUE(read_blob(buffer, offset, decoded))
        << "blob size " << blob.size();
    EXPECT_EQ(decoded.size(), blob.size());
    EXPECT_EQ(decoded, blob);
    EXPECT_EQ(offset, buffer.size());
  }
}

TEST(FormatBlobTest, SequentialBlobsDecodeInOrderAndLandExactlyAtTheEnd) {
  std::string buffer;
  append_blob(buffer, "alpha");
  append_blob(buffer, "");
  append_blob(buffer, bytes("g\0mma"));

  size_t offset = 0;
  std::string first;
  std::string second;
  std::string third;

  ASSERT_TRUE(read_blob(buffer, offset, first));
  ASSERT_TRUE(read_blob(buffer, offset, second));
  ASSERT_TRUE(read_blob(buffer, offset, third));

  EXPECT_EQ(first, "alpha");
  EXPECT_EQ(second, "");
  EXPECT_EQ(third, bytes("g\0mma"));
  EXPECT_EQ(offset, buffer.size());

  // Nothing left: the next read fails and leaves the cursor put.
  std::string extra = "sentinel";
  EXPECT_FALSE(read_blob(buffer, offset, extra));
  EXPECT_EQ(offset, buffer.size());
  EXPECT_EQ(extra, "sentinel");
}

TEST(FormatBlobTest, MixedU32AndBlobsComposeIntoTheBaseFileEntryLayout) {
  // R2.1: magic | entry count | (key blob | value blob)*. Composing the
  // primitives must produce that byte-for-byte.
  std::string buffer = "KVB1";
  append_u32(buffer, 1u);
  append_blob(buffer, "k");
  append_blob(buffer, "v");

  const std::string expected = "KVB1" + bytes("\x01\x00\x00\x00") +
                               bytes("\x01\x00\x00\x00") + "k" +
                               bytes("\x01\x00\x00\x00") + "v";
  EXPECT_EQ(buffer, expected);

  size_t offset = 4; // past the magic
  uint32_t count = 0;
  std::string key;
  std::string value;
  ASSERT_TRUE(read_u32(buffer, offset, count));
  ASSERT_EQ(count, 1u);
  ASSERT_TRUE(read_blob(buffer, offset, key));
  ASSERT_TRUE(read_blob(buffer, offset, value));
  EXPECT_EQ(key, "k");
  EXPECT_EQ(value, "v");
  EXPECT_EQ(offset, buffer.size());
}

// --- read_blob: hostile lengths -------------------------------------------

TEST(FormatBlobTest, MissingLengthPrefixFails) {
  const std::string truncated = bytes("\x05\x00\x00");
  size_t offset = 0;
  std::string decoded = "sentinel";

  EXPECT_FALSE(read_blob(truncated, offset, decoded));

  EXPECT_EQ(offset, 0u);
  EXPECT_EQ(decoded, "sentinel");
}

TEST(FormatBlobTest, LengthPrefixWithNoPayloadFails) {
  const std::string only_prefix = bytes("\x01\x00\x00\x00");
  size_t offset = 0;
  std::string decoded = "sentinel";

  EXPECT_FALSE(read_blob(only_prefix, offset, decoded));

  EXPECT_EQ(offset, 0u);
  EXPECT_EQ(decoded, "sentinel");
}

TEST(FormatBlobTest, DeclaredLengthOverrunningTheBufferFails) {
  // The hostile case: the prefix promises 16 bytes, only 2 follow. Trusting it
  // would read 14 bytes past the end of the string.
  const std::string hostile = bytes("\x10\x00\x00\x00") + "ab";
  size_t offset = 0;
  std::string decoded = "sentinel";

  EXPECT_FALSE(read_blob(hostile, offset, decoded));

  EXPECT_EQ(offset, 0u);
  EXPECT_EQ(decoded, "sentinel");
}

TEST(FormatBlobTest, DeclaredLengthOffByOneOverrunFails) {
  // The boundary: exactly one byte more than is available.
  const std::string hostile = bytes("\x03\x00\x00\x00") + "ab";
  size_t offset = 0;
  std::string decoded = "sentinel";

  EXPECT_FALSE(read_blob(hostile, offset, decoded));
  EXPECT_EQ(offset, 0u);

  // ...and exactly what is available succeeds, so the check is not off by one
  // in the other direction.
  const std::string exact = bytes("\x02\x00\x00\x00") + "ab";
  offset = 0;
  ASSERT_TRUE(read_blob(exact, offset, decoded));
  EXPECT_EQ(decoded, "ab");
  EXPECT_EQ(offset, exact.size());
}

TEST(FormatBlobTest, DeclaredLengthOfUint32MaxFails) {
  // 4 GiB claimed, 2 bytes present. Must be rejected without attempting an
  // allocation or a copy.
  const std::string hostile = bytes("\xFF\xFF\xFF\xFF") + "ab";
  size_t offset = 0;
  std::string decoded = "sentinel";

  EXPECT_FALSE(read_blob(hostile, offset, decoded));

  EXPECT_EQ(offset, 0u);
  EXPECT_EQ(decoded, "sentinel");
}

TEST(FormatBlobTest, TruncationInASequenceStopsAtTheLastGoodRecord) {
  // What a torn tail looks like to the decoder (R2.5): two intact blobs
  // followed by a partial one. The cursor must be left pointing at the start of
  // the broken record so the caller knows where to truncate.
  std::string buffer;
  append_blob(buffer, "one");
  append_blob(buffer, "two");
  const size_t good_prefix_size = buffer.size();
  append_blob(buffer, "three");
  buffer.resize(buffer.size() - 2); // tear the last blob mid-payload

  size_t offset = 0;
  std::string decoded;
  ASSERT_TRUE(read_blob(buffer, offset, decoded));
  EXPECT_EQ(decoded, "one");
  ASSERT_TRUE(read_blob(buffer, offset, decoded));
  EXPECT_EQ(decoded, "two");
  ASSERT_EQ(offset, good_prefix_size);

  EXPECT_FALSE(read_blob(buffer, offset, decoded));
  EXPECT_EQ(offset, good_prefix_size);
  EXPECT_EQ(decoded, "two"); // untouched by the failed read
}

// --- crc32 -----------------------------------------------------------------

TEST(FormatCrc32Test, MatchesKnownIeeeVectors) {
  // Standard CRC-32/ISO-HDLC vectors, i.e. what zlib's crc32() returns. Pinning
  // these means the WAL checksum can be verified with any off-the-shelf tool.
  EXPECT_EQ(crc32(""), 0x00000000u);
  EXPECT_EQ(crc32("a"), 0xE8B7BE43u);
  EXPECT_EQ(crc32("abc"), 0x352441C2u);
  EXPECT_EQ(crc32("123456789"), 0xCBF43926u);
  EXPECT_EQ(crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(FormatCrc32Test, PointerAndStringOverloadsAgree) {
  const std::string payload = "raftkv-durability-payload";

  EXPECT_EQ(crc32(payload), crc32(payload.data(), payload.size()));
  EXPECT_EQ(crc32(payload), 0x6A84C2C2u);
}

TEST(FormatCrc32Test, CoversEmbeddedNulBytes) {
  const std::string with_nul("a\x00"
                             "b",
                             3);

  EXPECT_EQ(crc32(with_nul), 0x15E87871u);
  // A C-string implementation would stop at the NUL and agree with crc32("a").
  EXPECT_NE(crc32(with_nul), crc32("a"));
}

TEST(FormatCrc32Test, ZeroLengthIsTheEmptyChecksumWhateverThePointer) {
  const std::string empty;

  EXPECT_EQ(crc32(empty.data(), empty.size()), 0x00000000u);
  EXPECT_EQ(crc32(empty), 0x00000000u);
}

TEST(FormatCrc32Test, AnySingleBitFlipChangesTheChecksum) {
  // The whole point of the WAL checksum: a torn or bit-rotted record must not
  // hash to the value stored alongside it. CRC-32 detects every 1-bit error.
  const std::string payload = "raftkv-durability-payload";
  const uint32_t baseline = crc32(payload);

  for (size_t index = 0; index < payload.size(); ++index) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string flipped = payload;
      flipped[index] = static_cast<char>(
          static_cast<unsigned char>(flipped[index]) ^ (1u << bit));
      EXPECT_NE(crc32(flipped), baseline) << "byte " << index << " bit " << bit;
    }
  }
}

TEST(FormatCrc32Test, LengthIsPartOfTheChecksum) {
  // Trailing NULs must not be invisible: a record truncated to a shorter length
  // has to produce a different checksum.
  EXPECT_NE(crc32(bytes("ab\0")), crc32("ab"));
  EXPECT_NE(crc32(std::string(4, '\x00')), crc32(std::string(5, '\x00')));
}

} // namespace
} // namespace format
} // namespace kvdb
