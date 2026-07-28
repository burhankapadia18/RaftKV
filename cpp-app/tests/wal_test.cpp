/**
 * @file wal_test.cpp
 * @brief Unit tests for the write-ahead log (spec R2.4, R2.5).
 *
 * Two jobs here:
 *   1. Pin the on-disk framing - `uint32 len | payload | uint32 crc32` in
 *      little-endian - byte for byte, and prove the log is binary-clean
 *      (NUL, '\n' and '=' are ordinary bytes, unlike the Phase 0 line format).
 *   2. Hammer the torn tail. Every corruption shape below is what a `kill -9`
 *      part-way through an append can actually leave behind, and each one
 *      asserts the same three things: the intact prefix still replays, the
 *      file is HEALED on disk (a second replay sees the same prefix and
 *      nothing else), and a later append lands cleanly after the healed tail.
 *      The length prefix in particular is attacker-influenced garbage after a
 *      crash - a record claiming 4 GiB must be rejected, not allocated.
 *
 * Corrupt files are written by hand with raw bytes so the exact on-disk shape
 * is under the test's control rather than the implementation's.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "storage/format.hpp"
#include "storage/wal.hpp"

namespace kvdb {
namespace {

/** @brief Encode a uint32 the way the record framing does: little-endian. */
[[nodiscard]] std::string le_u32(std::uint32_t value) {
  std::string out(4, '\0');
  out[0] = static_cast<char>(value & 0xFFu);
  out[1] = static_cast<char>((value >> 8) & 0xFFu);
  out[2] = static_cast<char>((value >> 16) & 0xFFu);
  out[3] = static_cast<char>((value >> 24) & 0xFFu);
  return out;
}

/** @brief The exact bytes one intact record of @p payload occupies. */
[[nodiscard]] std::string make_record(const std::string &payload) {
  return le_u32(static_cast<std::uint32_t>(payload.size())) + payload +
         le_u32(format::crc32(payload));
}

/** @brief The exact bytes a log holding @p payloads in order occupies. */
[[nodiscard]] std::string
concat_records(const std::vector<std::string> &payloads) {
  std::string out;
  for (const std::string &payload : payloads) {
    out += make_record(payload);
  }
  return out;
}

class WalTest : public ::testing::Test {
protected:
  void SetUp() override {
    static int counter = 0;
    const ::testing::TestInfo *info =
        ::testing::UnitTest::GetInstance()->current_test_info();

    std::string name = "kvdb_wal_test_";
    name += info->name();
    name += "_";
    name += std::to_string(counter++);
    name += ".wal";

    wal_path_ = std::filesystem::temp_directory_path() / name;
    remove_wal_file();
  }

  void TearDown() override { remove_wal_file(); }

  [[nodiscard]] std::string path() const { return wal_path_.string(); }

  [[nodiscard]] bool exists() const {
    return std::filesystem::exists(wal_path_);
  }

  void remove_wal_file() const {
    std::error_code ec;
    std::filesystem::remove(wal_path_, ec);
  }

  /** @brief Read the log back byte for byte, NULs included. */
  [[nodiscard]] std::string read_raw() const {
    std::ifstream file(wal_path_, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
  }

  /** @brief Lay down exact bytes behind the WAL's back. */
  void write_raw(const std::string &bytes) const {
    std::ofstream file(wal_path_, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  /** @brief Collect everything a replay hands back, in order. */
  [[nodiscard]] static std::vector<std::string> replay_all(Wal &wal) {
    std::vector<std::string> seen;
    wal.replay(
        [&seen](const std::string &payload) { seen.push_back(payload); });
    return seen;
  }

  /** @brief Replay @p path in a fresh Wal and return the payloads. */
  [[nodiscard]] std::vector<std::string> replay_fresh() const {
    Wal wal(path(), WalSyncMode::kAlways);
    return replay_all(wal);
  }

  /**
   * @brief The full torn-tail contract for one corruption shape (R2.5).
   *
   * @param on_disk         Exact bytes to plant in the log file.
   * @param expected_prefix Payloads that survive ahead of the damage.
   */
  void expect_torn_tail_heals(const std::string &on_disk,
                              const std::vector<std::string> &expected_prefix) {
    write_raw(on_disk);
    const std::string healed = concat_records(expected_prefix);
    ASSERT_LT(healed.size(), on_disk.size()) << "test bug: nothing is torn";

    // (a) the intact prefix is replayed, and the counters describe the file
    //     that replay leaves behind - not the torn one it found.
    {
      Wal wal(path(), WalSyncMode::kAlways);
      EXPECT_EQ(replay_all(wal), expected_prefix);
      EXPECT_EQ(wal.record_count(), expected_prefix.size());
      EXPECT_EQ(wal.size_bytes(), healed.size());
    }

    // (b) the file is healed on disk, and healing is idempotent: a second
    //     replay yields the same prefix and nothing else.
    EXPECT_EQ(read_raw(), healed);
    EXPECT_EQ(replay_fresh(), expected_prefix);
    EXPECT_EQ(read_raw(), healed);

    // (c) a subsequent append lands after the healed tail.
    const std::string next = "after-heal";
    {
      Wal wal(path(), WalSyncMode::kAlways);
      ASSERT_EQ(replay_all(wal), expected_prefix);
      wal.append(next);
      EXPECT_EQ(wal.record_count(), expected_prefix.size() + 1);
      EXPECT_EQ(wal.size_bytes(), healed.size() + make_record(next).size());
    }
    EXPECT_EQ(read_raw(), healed + make_record(next));

    std::vector<std::string> expected_final = expected_prefix;
    expected_final.push_back(next);
    EXPECT_EQ(replay_fresh(), expected_final);
  }

  std::filesystem::path wal_path_;
};

// --- Framing --------------------------------------------------------------

TEST_F(WalTest, AppendFramesLengthPayloadAndCrcLittleEndian) {
  {
    Wal wal(path(), WalSyncMode::kAlways);
    wal.append("123456789");

    EXPECT_EQ(wal.record_count(), 1u);
    EXPECT_EQ(wal.size_bytes(), 17u); // 4 + 9 + 4
  }

  // crc32("123456789") == 0xCBF43926 is the standard IEEE check value, so
  // these bytes pin the framing without going through format.hpp at all.
  const std::string expected = std::string("\x09\x00\x00\x00", 4) +
                               "123456789" + std::string("\x26\x39\xf4\xcb", 4);
  EXPECT_EQ(read_raw(), expected);
  EXPECT_EQ(expected, make_record("123456789"));
}

TEST_F(WalTest, EmptyPayloadIsAValidRecordNotATornTail) {
  {
    Wal wal(path(), WalSyncMode::kAlways);
    wal.append("");
    EXPECT_EQ(wal.record_count(), 1u);
    EXPECT_EQ(wal.size_bytes(), 8u);
  }

  // Zero length, and crc32 of nothing is 0 - eight zero bytes on disk.
  EXPECT_EQ(read_raw(), std::string(8, '\0'));
  EXPECT_EQ(replay_fresh(), std::vector<std::string>{""});
}

TEST_F(WalTest, AppendDoesNotBufferInUserSpace) {
  Wal wal(path(), WalSyncMode::kAlways);
  wal.append("one");
  wal.append("two");

  // Same instance, no close in between: the bytes must already be in the
  // file, otherwise a kill -9 would lose an acknowledged write.
  EXPECT_EQ(read_raw(), concat_records({"one", "two"}));
  EXPECT_EQ(replay_all(wal), (std::vector<std::string>{"one", "two"}));
}

// --- Round-trip -----------------------------------------------------------

TEST_F(WalTest, AppendedRecordsReplayInExactOrder) {
  {
    Wal wal(path(), WalSyncMode::kAlways);
    wal.append("one");
    wal.append("two");
    wal.append("three");
    EXPECT_EQ(wal.record_count(), 3u);
  }

  Wal wal(path(), WalSyncMode::kAlways);
  const std::vector<std::string> expected{"one", "two", "three"};

  EXPECT_EQ(replay_all(wal), expected);
  EXPECT_EQ(wal.record_count(), 3u);
  EXPECT_EQ(wal.size_bytes(), concat_records(expected).size());
  EXPECT_EQ(wal.size_bytes(), read_raw().size());
}

TEST_F(WalTest, DuplicatePayloadsAreReplayedOncePerAppend) {
  // The WAL is a log, not a set: two identical commands are two records.
  {
    Wal wal(path(), WalSyncMode::kAlways);
    wal.append("same");
    wal.append("same");
  }

  EXPECT_EQ(replay_fresh(), (std::vector<std::string>{"same", "same"}));
}

TEST_F(WalTest, BinaryHostilePayloadsRoundTripByteForByte) {
  // The whole point of length-prefixed binary framing: none of these bytes
  // mean anything to the log.
  const std::vector<std::string> payloads{
      std::string("a\0b", 3),
      "line1\nline2\n",
      "key=value=more",
      std::string("\0\0\0\0", 4),
      std::string("\xff\xfe\x00\x01\x7f", 5),
      std::string("trailing-nul\0", 13),
  };

  {
    Wal wal(path(), WalSyncMode::kAlways);
    for (const std::string &payload : payloads) {
      wal.append(payload);
    }
  }

  const std::vector<std::string> replayed = replay_fresh();
  ASSERT_EQ(replayed.size(), payloads.size());
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    SCOPED_TRACE("payload index " + std::to_string(i));
    EXPECT_EQ(replayed[i], payloads[i]);
    EXPECT_EQ(replayed[i].size(), payloads[i].size());
  }
}

TEST_F(WalTest, LargePayloadRoundTrips) {
  // Big enough that a single write() is not guaranteed to consume it all.
  constexpr std::size_t kPayloadBytes = 256u * 1024u;
  std::string payload;
  payload.reserve(kPayloadBytes);
  for (std::size_t i = 0; i < kPayloadBytes; ++i) {
    payload.push_back(static_cast<char>(i % 251));
  }

  {
    Wal wal(path(), WalSyncMode::kAlways);
    wal.append(payload);
    EXPECT_EQ(wal.size_bytes(), payload.size() + 8);
  }

  EXPECT_EQ(replay_fresh(), std::vector<std::string>{payload});
}

TEST_F(WalTest, ManyRecordsKeepTheirOrder) {
  constexpr int kRecords = 1000;
  std::vector<std::string> expected;
  expected.reserve(kRecords);
  {
    Wal wal(path(), WalSyncMode::kNever); // 1000 fsyncs is not the point here
    for (int i = 0; i < kRecords; ++i) {
      expected.push_back("value-" + std::to_string(i));
      wal.append(expected.back());
    }
    EXPECT_EQ(wal.record_count(), static_cast<std::size_t>(kRecords));
  }

  EXPECT_EQ(replay_fresh(), expected);
}

// --- Nothing to replay ----------------------------------------------------

TEST_F(WalTest, ReplayOfMissingFileIsAQuietNoOp) {
  ASSERT_FALSE(exists());

  Wal wal(path(), WalSyncMode::kAlways);
  std::vector<std::string> seen;
  EXPECT_NO_THROW(
      wal.replay([&seen](const std::string &p) { seen.push_back(p); }));

  EXPECT_TRUE(seen.empty());
  EXPECT_EQ(wal.record_count(), 0u);
  EXPECT_EQ(wal.size_bytes(), 0u);
  // A pure reader must not litter the data directory.
  EXPECT_FALSE(exists());
}

TEST_F(WalTest, ReplayOfEmptyFileIsAQuietNoOp) {
  write_raw("");
  ASSERT_TRUE(exists());

  Wal wal(path(), WalSyncMode::kAlways);
  EXPECT_TRUE(replay_all(wal).empty());
  EXPECT_EQ(wal.record_count(), 0u);
  EXPECT_EQ(wal.size_bytes(), 0u);
  EXPECT_EQ(read_raw(), "");
}

TEST_F(WalTest, ConstructionAloneDoesNotCreateTheFile) {
  {
    const Wal wal(path(), WalSyncMode::kAlways);
    EXPECT_EQ(wal.size_bytes(), 0u);
    EXPECT_EQ(wal.record_count(), 0u);
    EXPECT_EQ(wal.path(), path());
  }
  // The file appears on the first append, not on construction.
  EXPECT_FALSE(exists());
}

// --- Torn tail (R2.5) -----------------------------------------------------

TEST_F(WalTest, TruncatedMidPayloadIsHealed) {
  // Third record declares 10 bytes of payload; only 3 made it to disk.
  const std::string torn = le_u32(10) + "abc";
  expect_torn_tail_heals(concat_records({"alpha", "beta"}) + torn,
                         {"alpha", "beta"});
}

TEST_F(WalTest, TruncatedMidLengthPrefixIsHealed) {
  // A crash between records can leave 1-3 bytes of the next length prefix.
  for (std::size_t stray = 1; stray <= 3; ++stray) {
    SCOPED_TRACE("stray length-prefix bytes: " + std::to_string(stray));
    expect_torn_tail_heals(make_record("alpha") + std::string(stray, '\x7f'),
                           {"alpha"});
  }
}

TEST_F(WalTest, TruncatedChecksumIsHealed) {
  // Payload complete, CRC field short or absent: still a torn record.
  const std::string payload = "beta";
  const std::string crc = le_u32(format::crc32(payload));
  for (std::size_t kept = 0; kept < 4; ++kept) {
    SCOPED_TRACE("checksum bytes present: " + std::to_string(kept));
    const std::string torn =
        le_u32(static_cast<std::uint32_t>(payload.size())) + payload +
        crc.substr(0, kept);
    expect_torn_tail_heals(make_record("alpha") + torn, {"alpha"});
  }
}

TEST_F(WalTest, CorruptedPayloadFailsTheChecksumAndIsHealed) {
  // A structurally complete record whose payload bit-rotted. Nothing about
  // the framing gives it away - only the CRC does.
  std::string on_disk = concat_records({"alpha", "beta", "gamma"});
  const std::size_t beta_payload_start = make_record("alpha").size() + 4;
  on_disk[beta_payload_start] = static_cast<char>(
      static_cast<unsigned char>(on_disk[beta_payload_start]) ^ 0x01u);

  // "gamma" is intact and sits after the damage, and is still discarded:
  // once framing is in doubt, everything past the break goes.
  expect_torn_tail_heals(on_disk, {"alpha"});
}

TEST_F(WalTest, CorruptedChecksumFieldIsHealed) {
  std::string on_disk = concat_records({"alpha", "beta"});
  on_disk.back() =
      static_cast<char>(static_cast<unsigned char>(on_disk.back()) ^ 0xFFu);

  expect_torn_tail_heals(on_disk, {"alpha"});
}

TEST_F(WalTest, GarbageLengthPrefixClaimingAHugeRecordIsHealed) {
  // THE hostile case: 0xFFFFFFFF bytes are claimed, 4 are present. The
  // declared length must be bounds-checked against what is actually there,
  // never trusted into an allocation or a read past the buffer.
  const std::string torn = le_u32(0xFFFFFFFFu) + "junk";
  expect_torn_tail_heals(make_record("alpha") + torn, {"alpha"});
}

TEST_F(WalTest, LengthPrefixOverrunningByOneByteIsHealed) {
  // Off-by-one rather than obvious garbage: 5 declared, 4 present.
  const std::string torn = le_u32(5) + "beta";
  expect_torn_tail_heals(make_record("alpha") + torn, {"alpha"});
}

TEST_F(WalTest, LoneZeroLengthPrefixWithoutAChecksumIsHealed) {
  // Four zero bytes look like a valid empty payload until the CRC field
  // turns out to be missing.
  expect_torn_tail_heals(make_record("alpha") + le_u32(0), {"alpha"});
}

TEST_F(WalTest, DamageInTheFirstRecordEmptiesTheLog) {
  write_raw(le_u32(99) + "short");

  {
    Wal wal(path(), WalSyncMode::kAlways);
    EXPECT_TRUE(replay_all(wal).empty());
    EXPECT_EQ(wal.record_count(), 0u);
    EXPECT_EQ(wal.size_bytes(), 0u);
  }
  EXPECT_EQ(read_raw(), "");

  // The emptied log is still usable.
  {
    Wal wal(path(), WalSyncMode::kAlways);
    ASSERT_TRUE(replay_all(wal).empty());
    wal.append("fresh");
  }
  EXPECT_EQ(read_raw(), make_record("fresh"));
  EXPECT_EQ(replay_fresh(), std::vector<std::string>{"fresh"});
}

TEST_F(WalTest, ReplayOfAnIntactLogLeavesTheFileUntouched) {
  const std::string on_disk = concat_records({"one", "two"});
  write_raw(on_disk);

  EXPECT_EQ(replay_fresh(), (std::vector<std::string>{"one", "two"}));
  EXPECT_EQ(read_raw(), on_disk);
  EXPECT_EQ(replay_fresh(), (std::vector<std::string>{"one", "two"}));
  EXPECT_EQ(read_raw(), on_disk);
}

// --- Append after replay --------------------------------------------------

TEST_F(WalTest, AppendAfterReplayContinuesTheFileInsteadOfClobberingIt) {
  {
    Wal wal(path(), WalSyncMode::kAlways);
    wal.append("a");
    wal.append("b");
  }

  {
    Wal wal(path(), WalSyncMode::kAlways);
    ASSERT_EQ(replay_all(wal), (std::vector<std::string>{"a", "b"}));
    wal.append("c");
    EXPECT_EQ(wal.record_count(), 3u);
    EXPECT_EQ(wal.size_bytes(), concat_records({"a", "b", "c"}).size());
  }

  EXPECT_EQ(read_raw(), concat_records({"a", "b", "c"}));
  EXPECT_EQ(replay_fresh(), (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(WalTest, AppendWithoutReplayStillAppendsToAnExistingFile) {
  // Opening for append must not truncate, even if nobody replayed first.
  write_raw(make_record("existing"));

  {
    Wal wal(path(), WalSyncMode::kAlways);
    wal.append("added");
  }

  EXPECT_EQ(read_raw(), concat_records({"existing", "added"}));
  EXPECT_EQ(replay_fresh(), (std::vector<std::string>{"existing", "added"}));
}

// --- Compaction (R2.6 support) --------------------------------------------

TEST_F(WalTest, TruncateEmptiesTheFileAndResetsCounters) {
  Wal wal(path(), WalSyncMode::kAlways);
  wal.append("one");
  wal.append("two");
  ASSERT_GT(wal.size_bytes(), 0u);

  wal.truncate();

  EXPECT_EQ(wal.size_bytes(), 0u);
  EXPECT_EQ(wal.record_count(), 0u);
  EXPECT_EQ(read_raw(), "");

  // ...and the same instance keeps working afterwards, writing from offset 0.
  wal.append("post-compaction");
  EXPECT_EQ(wal.record_count(), 1u);
  EXPECT_EQ(wal.size_bytes(), make_record("post-compaction").size());
  EXPECT_EQ(read_raw(), make_record("post-compaction"));
  EXPECT_EQ(replay_fresh(), std::vector<std::string>{"post-compaction"});
}

TEST_F(WalTest, TruncateOnAMissingFileIsANoOp) {
  ASSERT_FALSE(exists());

  Wal wal(path(), WalSyncMode::kAlways);
  EXPECT_NO_THROW(wal.truncate());

  EXPECT_EQ(wal.size_bytes(), 0u);
  EXPECT_EQ(wal.record_count(), 0u);
  EXPECT_FALSE(exists());
}

TEST_F(WalTest, CountersTrackTheFileSizeAcrossAppends) {
  Wal wal(path(), WalSyncMode::kAlways);
  std::vector<std::string> written;

  for (int i = 0; i < 5; ++i) {
    written.push_back("payload-" + std::to_string(i));
    wal.append(written.back());

    SCOPED_TRACE("after append " + std::to_string(i));
    EXPECT_EQ(wal.record_count(), written.size());
    EXPECT_EQ(wal.size_bytes(), concat_records(written).size());
    EXPECT_EQ(wal.size_bytes(), read_raw().size());
  }
}

// --- Sync modes -----------------------------------------------------------

TEST_F(WalTest, SyncModesProduceIdenticalBytes) {
  const std::vector<std::string> payloads{"one", std::string("t\0wo", 4),
                                          "three"};

  auto write_with = [&](WalSyncMode mode) {
    remove_wal_file();
    Wal wal(path(), mode);
    for (const std::string &payload : payloads) {
      wal.append(payload);
    }
    return read_raw();
  };

  const std::string always_bytes = write_with(WalSyncMode::kAlways);
  const std::string never_bytes = write_with(WalSyncMode::kNever);

  EXPECT_EQ(always_bytes, never_bytes);
  EXPECT_EQ(always_bytes, concat_records(payloads));
  EXPECT_EQ(replay_fresh(), payloads);
}

TEST_F(WalTest, SyncModeNeverStillHealsATornTail) {
  write_raw(make_record("alpha") + le_u32(0xFFFFFFFFu));

  Wal wal(path(), WalSyncMode::kNever);
  EXPECT_EQ(replay_all(wal), std::vector<std::string>{"alpha"});
  EXPECT_EQ(read_raw(), make_record("alpha"));

  wal.append("beta");
  EXPECT_EQ(read_raw(), concat_records({"alpha", "beta"}));
}

} // namespace
} // namespace kvdb
