/**
 * @file kv_store_test.cpp
 * @brief Unit tests for PersistentKVStore (spec R0.11, rewritten for Phase 2).
 *
 * Three jobs here:
 *   1. Pin the IKVStore contract (set/get/remove/contains, reload). Unchanged
 *      from Phase 0 - Phase 2 replaced the persistence strategy underneath the
 *      interface without changing the interface.
 *   2. Pin the NEW on-disk format (R2.1): a base file of magic "KVB1", a
 *      uint32 entry count and length-prefixed key/value blobs, written
 *      atomically, plus a write-ahead log next to it carrying every mutation.
 *   3. Prove the durability properties the phase exists for: writes survive
 *      without any base-file rewrite (R2.7), legacy files migrate (R2.3), the
 *      WAL compacts (R2.6), and a corrupt or torn file fails loudly instead of
 *      loading garbage (R2.5).
 *   4. Pin the Phase 3 snapshot seam (R3.2 / R3.7): snapshot_state() /
 *      restore_state(), and the serialize_state() / deserialize_state() codec
 *      they share with the base file - one encoding for disk and for the wire.
 *
 * The Phase 0 version of this file pinned the *lossiness* of the old
 * "key=value\n" format: keys containing '=' were rewritten, anything
 * containing a newline was truncated. Those tests documented a format that no
 * longer exists. Their replacements below assert exact round-trips on the same
 * inputs, and that flip is the proof R2.1 did what it set out to do.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <msgpack.hpp>

#include "commands/kv_command.hpp"
#include "storage/format.hpp"
#include "storage/kv_store.hpp"

namespace kvdb {
namespace {

// --- Expected-bytes builders ----------------------------------------------
//
// Written by hand rather than through format::append_u32/append_blob on
// purpose: a test that encodes with the same helper it is checking would pass
// for any self-consistent format, including a wrong one.

/** @brief Little-endian uint32, four bytes. */
std::string u32le(uint32_t value) {
  std::string out(4, '\0');
  out[0] = static_cast<char>(value & 0xFFu);
  out[1] = static_cast<char>((value >> 8) & 0xFFu);
  out[2] = static_cast<char>((value >> 16) & 0xFFu);
  out[3] = static_cast<char>((value >> 24) & 0xFFu);
  return out;
}

/** @brief uint32 length prefix followed by the raw bytes. */
std::string blob(const std::string &bytes) {
  return u32le(static_cast<uint32_t>(bytes.size())) + bytes;
}

/** @brief The exact base file expected for @p entries, in the given order. */
std::string
kvb1(const std::vector<std::pair<std::string, std::string>> &entries) {
  std::string out = "KVB1";
  out += u32le(static_cast<uint32_t>(entries.size()));
  for (const auto &entry : entries) {
    out += blob(entry.first);
    out += blob(entry.second);
  }
  return out;
}

/** @brief Msgpack-encode a KVCommand the way the WAL stores one. */
std::string pack_command(const std::string &op, const std::string &key,
                         const std::string &value) {
  KVCommand cmd;
  cmd.op = op;
  cmd.key = key;
  cmd.value = value;

  msgpack::sbuffer buffer;
  msgpack::pack(buffer, cmd);
  return std::string(buffer.data(), buffer.size());
}

/**
 * @brief Valid msgpack that is not a KVCommand.
 *
 * A bare string decodes fine as msgpack but cannot convert to the map
 * KVCommand expects, so from_msgpack() throws - which is the case the store
 * has to treat as "stop replaying here".
 */
std::string pack_non_command() {
  msgpack::sbuffer buffer;
  msgpack::pack(buffer, std::string("this is not a command"));
  return std::string(buffer.data(), buffer.size());
}

void expect_value(const IKVStore &store, const std::string &key,
                  const std::string &expected) {
  const std::optional<std::string> actual = store.get(key);
  ASSERT_TRUE(actual.has_value()) << "expected key to be present: " << key;
  EXPECT_EQ(*actual, expected);
}

// --- Fixture ---------------------------------------------------------------

class PersistentKVStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    static int counter = 0;
    const ::testing::TestInfo *info =
        ::testing::UnitTest::GetInstance()->current_test_info();

    std::string name = "kvdb_store_test_";
    name += info->name();
    name += "_";
    name += std::to_string(counter++);
    name += ".db";

    db_path_ = std::filesystem::temp_directory_path() / name;
    wal_path_ = db_path_;
    wal_path_.replace_extension(".wal");
    remove_files();
  }

  void TearDown() override { remove_files(); }

  [[nodiscard]] std::string path() const { return db_path_.string(); }

  /** @brief Read the base file back, byte for byte ("" if absent). */
  [[nodiscard]] std::string read_db_file() const {
    return read_whole(db_path_);
  }

  /** @brief Read the WAL back, byte for byte ("" if absent or truncated). */
  [[nodiscard]] std::string read_wal_file() const {
    return read_whole(wal_path_);
  }

  /** @brief Overwrite the base file behind the store's back. */
  void write_db_file(const std::string &contents) const {
    std::ofstream file(db_path_, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  /** @brief Append one well-framed WAL record: len | payload | crc32. */
  void append_wal_record(const std::string &payload) const {
    std::string record = u32le(static_cast<uint32_t>(payload.size()));
    record += payload;
    record += u32le(format::crc32(payload));

    std::ofstream file(wal_path_, std::ios::binary | std::ios::app);
    file.write(record.data(), static_cast<std::streamsize>(record.size()));
  }

  /** @brief Chop @p bytes off the end of the WAL, simulating a torn append. */
  void chop_wal_tail(size_t bytes) const {
    const size_t size =
        static_cast<size_t>(std::filesystem::file_size(wal_path_));
    ASSERT_GT(size, bytes);
    std::filesystem::resize_file(wal_path_, size - bytes);
  }

  /**
   * @brief Write @p contents as the base file and assert the store refuses it.
   *
   * "Refuses" means a std::runtime_error naming the file and the problem - not
   * a crash, not an out-of-bounds read, and not a silent partial load.
   */
  void expect_corrupt_base(const std::string &contents) const {
    write_db_file(contents);
    try {
      const PersistentKVStore store(path());
      ADD_FAILURE() << "expected a corrupt base file error; the store loaded";
    } catch (const std::runtime_error &error) {
      const std::string message = error.what();
      EXPECT_NE(message.find("corrupt base file"), std::string::npos)
          << message;
      EXPECT_NE(message.find(path()), std::string::npos) << message;
    }
  }

  std::filesystem::path db_path_;
  std::filesystem::path wal_path_;

private:
  [[nodiscard]] static std::string
  read_whole(const std::filesystem::path &file_path) {
    std::ifstream file(file_path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
  }

  void remove_files() const {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(wal_path_, ec);
    // atomic_write_file() stages through "<path>.tmp"; a failed write can
    // leave one behind.
    std::filesystem::remove(db_path_.string() + ".tmp", ec);
  }
};

/** @brief Options that keep tests off fsync; durability is asserted by shape,
 *         not by timing. */
DurabilityOptions fast_options() {
  DurabilityOptions options;
  options.sync_mode = WalSyncMode::kNever;
  return options;
}

// --- Basic contract -------------------------------------------------------

TEST_F(PersistentKVStoreTest, ConstructionOnMissingFileYieldsEmptyStore) {
  ASSERT_FALSE(std::filesystem::exists(db_path_));

  std::unique_ptr<PersistentKVStore> store;
  EXPECT_NO_THROW(store = std::make_unique<PersistentKVStore>(path()));

  ASSERT_NE(store, nullptr);
  EXPECT_FALSE(store->contains("anything"));
  EXPECT_FALSE(store->get("anything").has_value());
  // Recovery only reads, so a store that is never written to creates no base
  // file.
  EXPECT_FALSE(std::filesystem::exists(db_path_));
}

TEST_F(PersistentKVStoreTest, GetOnAbsentKeyReturnsNullopt) {
  PersistentKVStore store(path(), fast_options());

  EXPECT_FALSE(store.get("nope").has_value());
  EXPECT_FALSE(store.contains("nope"));
}

TEST_F(PersistentKVStoreTest, SetThenGetAndContains) {
  PersistentKVStore store(path(), fast_options());

  store.set("alpha", "beta");

  expect_value(store, "alpha", "beta");
  EXPECT_TRUE(store.contains("alpha"));
  EXPECT_FALSE(store.contains("Alpha")); // keys are case sensitive
}

TEST_F(PersistentKVStoreTest, RemoveExistingKeyReturnsTrueAndPersists) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("alpha", "beta");

    EXPECT_TRUE(store.remove("alpha"));
    EXPECT_FALSE(store.contains("alpha"));
    EXPECT_FALSE(store.get("alpha").has_value());
  }

  const PersistentKVStore reloaded(path(), fast_options());
  EXPECT_FALSE(reloaded.contains("alpha"));
}

TEST_F(PersistentKVStoreTest, RemoveAbsentKeyReturnsFalseAndDoesNotTouchDisk) {
  PersistentKVStore store(path(), fast_options());
  store.set("alpha", "beta");

  const std::string wal_before = read_wal_file();
  ASSERT_FALSE(wal_before.empty());

  EXPECT_FALSE(store.remove("missing"));
  EXPECT_EQ(read_wal_file(), wal_before); // no record was appended

  // ...while removing a key that IS present does log something.
  EXPECT_TRUE(store.remove("alpha"));
  EXPECT_GT(read_wal_file().size(), wal_before.size());
}

TEST_F(PersistentKVStoreTest, OverwriteKeepsOnlyTheLatestValue) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("k", "v1");
    store.set("k", "v2");
    expect_value(store, "k", "v2");
  }

  const PersistentKVStore reloaded(path(), fast_options());
  expect_value(reloaded, "k", "v2");
}

TEST_F(PersistentKVStoreTest, PersistenceRoundTripAcrossInstances) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("one", "1");
    store.set("two", "2");
    store.set("three", "3");
    EXPECT_TRUE(store.remove("two"));
  }

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "one", "1");
  expect_value(reloaded, "three", "3");
  EXPECT_FALSE(reloaded.contains("two"));
}

TEST_F(PersistentKVStoreTest, EmptyValueRoundTrips) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("k", "");
  }

  const PersistentKVStore reloaded(path(), fast_options());
  expect_value(reloaded, "k", "");
}

// --- On-disk base format (R2.1/R2.2) ---------------------------------------

TEST_F(PersistentKVStoreTest, BaseFileUsesTheKvb1BinaryLayout) {
  // Replaces the Phase 0 PersistUsesKeyEqualsValueLineFormat: the on-disk shape
  // this pins is the length-prefixed binary one from R2.1.
  DurabilityOptions options = fast_options();
  options.wal_max_records = 1; // compact after every write

  PersistentKVStore store(path(), options);
  store.set("alpha", "beta");

  // Single entry, so unordered_map iteration order cannot matter.
  const std::string expected =
      std::string("KVB1") + u32le(1) + u32le(5) + "alpha" + u32le(4) + "beta";
  EXPECT_EQ(read_db_file(), expected);
  EXPECT_EQ(read_db_file(), kvb1({{"alpha", "beta"}}));
}

TEST_F(PersistentKVStoreTest, EmptyStoreSerialisesToMagicAndAZeroCount) {
  DurabilityOptions options = fast_options();
  options.wal_max_records = 1;

  PersistentKVStore store(path(), options);
  store.set("k", "v");
  EXPECT_TRUE(store.remove("k"));

  EXPECT_EQ(read_db_file(), kvb1({}));
  EXPECT_EQ(read_db_file().size(), 8u);
}

TEST_F(PersistentKVStoreTest, WalPathIsDerivedFromTheBaseFilePath) {
  EXPECT_EQ(PersistentKVStore::wal_path_for("kv.db"), "kv.wal");
  EXPECT_EQ(PersistentKVStore::wal_path_for("/var/lib/kvdb/kv.db"),
            "/var/lib/kvdb/kv.wal");
  // No ".db" suffix: just append.
  EXPECT_EQ(PersistentKVStore::wal_path_for("data"), "data.wal");
  EXPECT_EQ(PersistentKVStore::wal_path_for("data.dbx"), "data.dbx.wal");
  EXPECT_EQ(PersistentKVStore::wal_path_for("db"), "db.wal");
}

// --- Binary-hostile keys and values ---------------------------------------
//
// Every test in this section replaces a Phase 0 test that pinned the OPPOSITE
// outcome. The line-based format had no escaping, so '=' and '\n' were
// ambiguous with its own delimiters; R2.1's length prefixes remove the
// ambiguity entirely, and "the value comes back exactly as it went in" is now
// simply true for arbitrary bytes.

TEST_F(PersistentKVStoreTest, ValueContainingEqualsRoundTripsExactly) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("k", "a=b=c");
  }

  const PersistentKVStore reloaded(path(), fast_options());
  expect_value(reloaded, "k", "a=b=c");
}

TEST_F(PersistentKVStoreTest, KeyContainingEqualsRoundTripsExactly) {
  // Was KeyContainingEqualsIsCorruptedByReload: "a=b" used to be written as
  // "a=b=v" and read back as key "a" with value "b=v", losing the key that was
  // actually written. R2.1 length-prefixes the key, so there is nothing left to
  // misparse.
  {
    PersistentKVStore store(path(), fast_options());
    store.set("a=b", "v");
  }

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "a=b", "v");
  EXPECT_FALSE(reloaded.contains("a")); // no phantom key invented
}

TEST_F(PersistentKVStoreTest, ValueContainingNewlineRoundTripsExactly) {
  // Was ValueContainingNewlineIsTruncatedByReload: the value used to span two
  // lines on disk and everything after the first newline was silently dropped.
  {
    PersistentKVStore store(path(), fast_options());
    store.set("k", "line1\nline2");
  }

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "k", "line1\nline2");
  EXPECT_FALSE(reloaded.contains("line2"));
}

TEST_F(PersistentKVStoreTest, KeyContainingNewlineRoundTripsExactly) {
  // Was KeyContainingNewlineIsReplacedByReload: "a\nb" used to be written as
  // two lines, so reload invented a key "b" and lost the one that was written.
  {
    PersistentKVStore store(path(), fast_options());
    store.set("a\nb", "v");
  }

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "a\nb", "v");
  EXPECT_FALSE(reloaded.contains("b"));
}

TEST_F(PersistentKVStoreTest, KeyAndValueOfArbitraryBytesRoundTripExactly) {
  // The acceptance case for R2.1, all three hostile bytes at once, in both
  // positions, including a trailing NUL that a C-string-based format would eat.
  const std::string key("k\0ey=with\nall", 13);
  const std::string value("v\0al=ue\nbytes\0", 14);
  ASSERT_EQ(key.size(), 13u);
  ASSERT_EQ(value.size(), 14u);

  {
    PersistentKVStore store(path(), fast_options());
    store.set(key, value);
  }

  const PersistentKVStore reloaded(path(), fast_options());

  const std::optional<std::string> actual = reloaded.get(key);
  ASSERT_TRUE(actual.has_value());
  EXPECT_EQ(*actual, value);
  EXPECT_EQ(actual->size(), 14u);
}

TEST_F(PersistentKVStoreTest, ValueContainingNulSurvivesReload) {
  // True before Phase 2 and still true: pinned so the binary format does not
  // regress the one hostile byte the line format happened to handle.
  const std::string value("a\0b", 3);
  {
    PersistentKVStore store(path(), fast_options());
    store.set("k", value);
  }

  const PersistentKVStore reloaded(path(), fast_options());

  const std::optional<std::string> actual = reloaded.get("k");
  ASSERT_TRUE(actual.has_value());
  EXPECT_EQ(*actual, value);
  EXPECT_EQ(actual->size(), 3u);
}

// --- WAL-backed durability (R2.5/R2.7) -------------------------------------

TEST_F(PersistentKVStoreTest, WritesSurviveWithoutAnyBaseFileRewrite) {
  // The crash-safety property, and the only test that runs the *default*
  // DurabilityOptions end to end: WalSyncMode::kAlways, so every append below
  // really did fsync. The store is then dropped without any clean shutdown,
  // and with the default thresholds nothing ever rewrote the base file - so
  // everything below came back out of the WAL alone. A kill -9 at any point
  // after set() returned leaves exactly this on disk.
  {
    PersistentKVStore store(path());
    store.set("a", "1");
    store.set("b", "2");
    store.set("a", "1-updated");
    EXPECT_TRUE(store.remove("b"));
    store.set("c", "3");
  }

  EXPECT_FALSE(std::filesystem::exists(db_path_)) << "no base file was written";
  EXPECT_GT(read_wal_file().size(), 0u);

  const PersistentKVStore reloaded(path());

  expect_value(reloaded, "a", "1-updated");
  expect_value(reloaded, "c", "3");
  EXPECT_FALSE(reloaded.contains("b")); // the DELETE replayed too
}

TEST_F(PersistentKVStoreTest, WalIsReplayedOnTopOfTheBaseFile) {
  DurabilityOptions options = fast_options();
  options.wal_max_records = 1; // force a base file after the first write

  {
    PersistentKVStore store(path(), options);
    store.set("base", "from-base-file");
    ASSERT_EQ(read_wal_file().size(), 0u);
  }

  // Now append records the base file knows nothing about.
  append_wal_record(pack_command("SET", "later", "from-wal"));
  append_wal_record(pack_command("DELETE", "base", ""));

  const PersistentKVStore reloaded(path(), options);

  expect_value(reloaded, "later", "from-wal");
  EXPECT_FALSE(reloaded.contains("base")); // WAL wins, it is newer
}

TEST_F(PersistentKVStoreTest, TornWalTailIsDroppedAndEarlierWritesSurvive) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("a", "1");
    store.set("b", "2");
  }

  // Chop three bytes: the last record's trailing CRC is now a short read, the
  // exact shape of a crash part-way through an append.
  chop_wal_tail(3);

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "a", "1");
  EXPECT_FALSE(reloaded.contains("b"));
}

TEST_F(PersistentKVStoreTest, UndecodableWalRecordStopsReplayAndHealsTheWal) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("a", "1");
  }

  // A perfectly framed, correctly checksummed record whose payload is not a
  // KVCommand, followed by a good one. Replay must stop at the bad record
  // rather than skip it: the record it cannot read might have been a DELETE,
  // and applying what came after would rebuild a state that never existed.
  append_wal_record(pack_non_command());
  append_wal_record(pack_command("SET", "b", "2"));

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "a", "1");
  EXPECT_FALSE(reloaded.contains("b"));

  // Healed: the recovered prefix is now the base file and the refused tail is
  // gone, so the next start does not hit it again.
  EXPECT_EQ(read_db_file(), kvb1({{"a", "1"}}));
  EXPECT_EQ(read_wal_file().size(), 0u);
}

TEST_F(PersistentKVStoreTest, InvalidCommandInWalStopsReplayToo) {
  append_wal_record(pack_command("SET", "a", "1"));
  append_wal_record(pack_command("BOGUS", "b", "2")); // unknown operation
  append_wal_record(pack_command("SET", "c", "3"));

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "a", "1");
  EXPECT_FALSE(reloaded.contains("b"));
  EXPECT_FALSE(reloaded.contains("c"));
}

TEST_F(PersistentKVStoreTest, UserOpInWalStopsReplay) {
  // A USER_SET/USER_DEL record is a *valid command* — is_valid() accepts both —
  // but it is not one this store ever writes: set()/remove() re-encode every
  // mutation as SET or DELETE, so a committed user write reaches the WAL as a
  // plain SET of its `__sys:user:...` key. A USER_* record therefore means the
  // file was not produced by this store, and replay stops exactly as it does
  // for a record that will not decode. Skipping it would be worse: a USER_DEL
  // that was silently ignored leaves a user who should have been removed.
  append_wal_record(pack_command("SET", "a", "1"));
  append_wal_record(pack_command("USER_SET", "alice", "record-bytes"));
  append_wal_record(pack_command("SET", "c", "3"));

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "a", "1");
  EXPECT_FALSE(reloaded.contains("c"));
  // Nothing was invented under either the bare name or the derived key.
  EXPECT_FALSE(reloaded.contains("alice"));
  EXPECT_FALSE(reloaded.contains("__sys:user:alice"));
}

TEST_F(PersistentKVStoreTest, AUserRecordSurvivesAsAnOrdinaryEntry) {
  // The other half of the argument above: Apply stores a user record with
  // store_.set(), so it must round-trip through the WAL and the base file like
  // any other key. If it did not, every restart would drop the user table.
  {
    PersistentKVStore store(path(), fast_options());
    store.set("__sys:user:alice", std::string("record\0bytes", 12));
    store.set("app:k", "v");
  }

  const PersistentKVStore reloaded(path(), fast_options());

  ASSERT_TRUE(reloaded.get("__sys:user:alice").has_value());
  EXPECT_EQ(*reloaded.get("__sys:user:alice"),
            std::string("record\0bytes", 12));
  expect_value(reloaded, "app:k", "v");
}

// --- Compaction (R2.6) ------------------------------------------------------

TEST_F(PersistentKVStoreTest, RecordThresholdTriggersCompaction) {
  DurabilityOptions options = fast_options();
  options.wal_max_records = 3;

  {
    PersistentKVStore store(path(), options);
    store.set("a", "1");
    store.set("b", "2");

    const size_t before = read_wal_file().size();
    EXPECT_GT(before, 0u);
    EXPECT_FALSE(std::filesystem::exists(db_path_));

    store.set("c", "3"); // third record reaches the threshold

    EXPECT_EQ(read_wal_file().size(), 0u) << "WAL should have been truncated";
    EXPECT_TRUE(std::filesystem::exists(db_path_));
    EXPECT_EQ(read_db_file().substr(0, 4), "KVB1");
  }

  const PersistentKVStore reloaded(path(), options);
  expect_value(reloaded, "a", "1");
  expect_value(reloaded, "b", "2");
  expect_value(reloaded, "c", "3");
}

TEST_F(PersistentKVStoreTest, ByteThresholdTriggersCompaction) {
  DurabilityOptions options = fast_options();
  options.wal_max_bytes = 1; // any append at all reaches it

  PersistentKVStore store(path(), options);
  store.set("k", "v");

  EXPECT_EQ(read_wal_file().size(), 0u);
  EXPECT_EQ(read_db_file(), kvb1({{"k", "v"}}));
}

TEST_F(PersistentKVStoreTest, CompactionKeepsTheWalBoundedOverManyWrites) {
  DurabilityOptions options = fast_options();
  options.wal_max_records = 4;

  PersistentKVStore store(path(), options);
  for (int i = 0; i < 50; ++i) {
    store.set("key" + std::to_string(i), std::to_string(i));
    // Never more than the threshold's worth of records outstanding.
    EXPECT_LT(read_wal_file().size(), 4u * 64u);
  }

  expect_value(store, "key0", "0");
  expect_value(store, "key49", "49");
}

// --- Legacy migration (R2.3) -----------------------------------------------

TEST_F(PersistentKVStoreTest, LegacyFileIsMigratedToTheBinaryFormatOnLoad) {
  write_db_file("alpha=beta\n");

  {
    const PersistentKVStore store(path(), fast_options());
    expect_value(store, "alpha", "beta");
  }

  // Rewritten during construction, so the next start takes the fast path.
  EXPECT_EQ(read_db_file(), kvb1({{"alpha", "beta"}}));

  const PersistentKVStore reloaded(path(), fast_options());
  expect_value(reloaded, "alpha", "beta");
  EXPECT_EQ(read_db_file(), kvb1({{"alpha", "beta"}}));
}

TEST_F(PersistentKVStoreTest, MigrationReadsEveryLegacyLine) {
  write_db_file("one=1\ntwo=2\nthree=3\n");

  const PersistentKVStore store(path(), fast_options());

  expect_value(store, "one", "1");
  expect_value(store, "two", "2");
  expect_value(store, "three", "3");
  // Three entries, so the exact byte order depends on unordered_map iteration;
  // the magic is the part worth pinning here.
  EXPECT_EQ(read_db_file().substr(0, 4), "KVB1");
}

TEST_F(PersistentKVStoreTest, MigrationSkipsLegacyLinesWithoutAnEqualsSign) {
  // Was LoadSkipsLinesWithoutAnEqualsSign. The rule itself is unchanged - it
  // just belongs to the legacy reader now, which is kept bug-compatible on
  // purpose so files written by the old code decode to what they always did.
  write_db_file("garbage-line\nk=v\n\nanother\n");

  const PersistentKVStore store(path(), fast_options());

  expect_value(store, "k", "v");
  EXPECT_FALSE(store.contains("garbage-line"));
  EXPECT_FALSE(store.contains("another"));
  EXPECT_EQ(read_db_file(), kvb1({{"k", "v"}}));
}

TEST_F(PersistentKVStoreTest, MigrationSplitsLegacyLinesAtTheFirstEquals) {
  // The old writer produced this for key "a" / value "b=v" AND for key "a=b" /
  // value "v"; the format cannot tell them apart. Migration resolves it the
  // same way the old reader did rather than guessing differently.
  write_db_file("a=b=v\n");

  const PersistentKVStore store(path(), fast_options());

  expect_value(store, "a", "b=v");
  EXPECT_FALSE(store.contains("a=b"));
}

TEST_F(PersistentKVStoreTest, EmptyLegacyFileMigratesToAnEmptyBaseFile) {
  write_db_file("");

  const PersistentKVStore store(path(), fast_options());

  EXPECT_FALSE(store.contains("anything"));
  EXPECT_EQ(read_db_file(), kvb1({}));
}

// --- Corrupt base file (R2.1 bounds checking) -------------------------------

TEST_F(PersistentKVStoreTest, TruncatedEntryCountIsRejected) {
  expect_corrupt_base(std::string("KVB1") + "ab"); // 2 of the 4 count bytes
}

TEST_F(PersistentKVStoreTest, EntryCountLargerThanTheFileIsRejected) {
  // The classic length-lie: a count no amount of remaining bytes could hold.
  // It must be refused before it is used to size anything.
  expect_corrupt_base(std::string("KVB1") + u32le(0xFFFFFFFFu));
}

TEST_F(PersistentKVStoreTest, TruncatedEntryIsRejected) {
  std::string contents = "KVB1";
  contents += u32le(1);
  contents += u32le(5);
  contents += "alph"; // key_len says 5, only 4 bytes follow

  expect_corrupt_base(contents);
}

TEST_F(PersistentKVStoreTest, BlobLengthOverrunningTheFileIsRejected) {
  std::string contents = "KVB1";
  contents += u32le(1);
  contents += u32le(1);
  contents += "k";
  contents += u32le(0x7FFFFFFFu); // value_len far past the end
  contents += "v";

  expect_corrupt_base(contents);
}

TEST_F(PersistentKVStoreTest, TrailingBytesAfterTheLastEntryAreRejected) {
  expect_corrupt_base(kvb1({{"a", "b"}}) + "junk");
}

TEST_F(PersistentKVStoreTest,
       CorruptBinaryFileDoesNotFallBackToTheLegacyParser) {
  // A file carrying the magic is parsed as binary, period. Falling back to the
  // legacy line parser here would turn a detectably corrupt file into
  // plausible-looking data, which is strictly worse than refusing to start.
  expect_corrupt_base("KVB1k=v\n");
}

TEST_F(PersistentKVStoreTest, ACorruptBaseFileIsNotPartiallyLoaded) {
  std::string contents = "KVB1";
  contents += u32le(2);
  contents += blob("good");
  contents += blob("entry");
  contents += u32le(9); // second entry's key_len lies
  contents += "short";

  write_db_file(contents);

  try {
    const PersistentKVStore store(path(), fast_options());
    ADD_FAILURE() << "expected a corrupt base file error; the store loaded";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("corrupt base file"),
              std::string::npos)
        << error.what();
  }

  // The file was left exactly as it was: nothing half-loaded, nothing
  // helpfully rewritten over the operator's evidence.
  EXPECT_EQ(read_db_file(), contents);
}

// --- The state codec (R3.1/R3.2) -------------------------------------------
//
// serialize_state()/deserialize_state() are the codec the base file and the
// snapshot stream share. They were lifted out of PersistentKVStore rather than
// copied, so the tests above already exercise the same encoder and decoder
// through the store; what follows covers them directly, including the
// malformed inputs a peer can now put on the wire and not just the ones a disk
// can produce.

/**
 * @brief Assert deserialize_state() refuses @p image, mentioning @p reason.
 *
 * "Refuses" means a std::runtime_error - not a crash, not an out-of-bounds
 * read (these run under ASan in CI), and not a partially decoded map.
 */
void expect_bad_image(const std::string &image, const std::string &reason) {
  try {
    const StateMap decoded = deserialize_state(image);
    ADD_FAILURE() << "expected a decode failure; got " << decoded.size()
                  << " entries";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
        << error.what();
  }
}

TEST(StateCodecTest, SerializeStateEmitsTheKvb1Layout) {
  // Single entry, so unordered_map iteration order cannot matter.
  const std::string image = serialize_state(StateMap{{"alpha", "beta"}});

  EXPECT_EQ(image, std::string("KVB1") + u32le(1) + u32le(5) + "alpha" +
                       u32le(4) + "beta");
  EXPECT_EQ(image, kvb1({{"alpha", "beta"}}));
}

TEST(StateCodecTest, AnEmptyStateIsAMagicAndAZeroCount) {
  const std::string image = serialize_state(StateMap{});

  EXPECT_EQ(image, kvb1({}));
  EXPECT_EQ(image.size(), 8u);
  EXPECT_TRUE(deserialize_state(image).empty());
}

TEST(StateCodecTest, ManyEntriesWithHostileBytesRoundTrip) {
  StateMap state;
  for (int i = 0; i < 200; ++i) {
    const std::string suffix = std::to_string(i);
    state[std::string("k\0=", 3) + suffix] = std::string("v\n\0", 3) + suffix;
  }

  const StateMap decoded = deserialize_state(serialize_state(state));

  EXPECT_EQ(decoded.size(), 200u);
  EXPECT_EQ(decoded, state);
}

TEST(StateCodecTest, RejectsAMissingMagic) {
  expect_bad_image("", "magic");
  expect_bad_image("KVB", "magic"); // shorter than the magic itself
  expect_bad_image(std::string("KVB2") + u32le(0), "magic");
  // A legacy "key=value" file is not a state image either. Migration is the
  // store's business (it checks the magic first); the codec just says no.
  expect_bad_image("alpha=beta\n", "magic");
}

TEST(StateCodecTest, RejectsATruncatedHeader) {
  expect_bad_image("KVB1", "truncated entry count");
  expect_bad_image(std::string("KVB1") + "ab", "truncated entry count");
}

TEST(StateCodecTest, RejectsAnEntryCountThatDisagreesWithTheData) {
  // The classic length-lie: a count no amount of remaining bytes could hold.
  // It must be refused before it is used to size anything.
  expect_bad_image(std::string("KVB1") + u32le(0xFFFFFFFFu), "exceeds");
  expect_bad_image(std::string("KVB1") + u32le(1), "exceeds");

  // Two claimed, one and a half supplied. Small enough to pass the bound
  // check above, so this is the per-entry check doing the work.
  expect_bad_image(std::string("KVB1") + u32le(2) + blob("k") + blob("v") +
                       blob("k2"),
                   "entry 1 is truncated");

  // Fewer claimed than supplied: the leftovers are not silently ignored.
  expect_bad_image(kvb1({{"a", "b"}}) + blob("extra") + blob("entry"),
                   "trailing bytes");
  expect_bad_image(std::string("KVB1") + u32le(0) + blob("k") + blob("v"),
                   "trailing bytes");
}

TEST(StateCodecTest, RejectsABlobLengthThatOverrunsTheBuffer) {
  // key_len says 5, four bytes follow.
  expect_bad_image(std::string("KVB1") + u32le(1) + u32le(5) + "alph",
                   "entry 0 is truncated");

  // value_len is a lie pointing far past the end. There are enough bytes for
  // one entry, so the entry-count bound cannot catch this - it is read_blob
  // refusing a length it cannot cover that keeps the decode in bounds.
  expect_bad_image(std::string("KVB1") + u32le(1) + blob("k") +
                       u32le(0x7FFFFFFFu) + "v",
                   "entry 0 is truncated");
}

// --- Snapshots (R3.2 / R3.7) ------------------------------------------------

TEST_F(PersistentKVStoreTest, SnapshotStateReturnsTheWholeStore) {
  PersistentKVStore store(path(), fast_options());
  store.set("a", "1");
  store.set("b", "2");
  ASSERT_TRUE(store.remove("a"));
  store.set("c", "3");

  const StateMap expected{{"b", "2"}, {"c", "3"}};
  EXPECT_EQ(store.snapshot_state(), expected);
}

TEST_F(PersistentKVStoreTest, SnapshotOfAnEmptyStoreIsLegal) {
  const PersistentKVStore store(path(), fast_options());

  const StateMap snapshot = store.snapshot_state();

  EXPECT_TRUE(snapshot.empty());
  EXPECT_EQ(serialize_state(snapshot), kvb1({}));
  EXPECT_TRUE(deserialize_state(serialize_state(snapshot)).empty());
}

TEST_F(PersistentKVStoreTest, SnapshotRoundTripsThroughTheStateCodec) {
  // Every byte the pre-Phase-2 line format mangled, in both positions.
  const std::string hostile_key("k\0ey=with\nall", 13);
  const std::string hostile_value("v\0al=ue\nbytes\0", 14);

  PersistentKVStore store(path(), fast_options());
  store.set(hostile_key, hostile_value);
  store.set("a=b", "c=d");
  store.set("line1\nline2", "x\ny");
  store.set("empty-value", "");

  const StateMap snapshot = store.snapshot_state();
  const StateMap decoded = deserialize_state(serialize_state(snapshot));

  EXPECT_EQ(decoded, snapshot);
  EXPECT_EQ(decoded.size(), 4u);
  ASSERT_EQ(decoded.count(hostile_key), 1u);
  EXPECT_EQ(decoded.at(hostile_key), hostile_value);
  EXPECT_EQ(decoded.at(hostile_key).size(), 14u);
}

TEST_F(PersistentKVStoreTest, TheSnapshotImageIsExactlyTheBaseFile) {
  // The Phase 3 seam: one encoding for disk and wire, so what a node streams
  // to a peer is byte-for-byte what its kv.db holds. Single entry, so
  // unordered_map iteration order cannot make this flaky.
  DurabilityOptions options = fast_options();
  options.wal_max_records = 1; // compact after every write

  PersistentKVStore store(path(), options);
  store.set("alpha", "beta");

  ASSERT_EQ(read_wal_file().size(), 0u); // folded into the base file
  EXPECT_EQ(serialize_state(store.snapshot_state()), read_db_file());
  EXPECT_EQ(read_db_file(), kvb1({{"alpha", "beta"}}));
}

TEST_F(PersistentKVStoreTest, RestoreStateReplacesRatherThanMerges) {
  // The obvious bug in a restore is to apply the snapshot on top of what is
  // already there. A key the local node has and the snapshot does not is not
  // part of the agreed state and must disappear.
  PersistentKVStore store(path(), fast_options());
  store.set("stale", "gone");
  store.set("shared", "old");

  const StateMap snapshot{{"shared", "new"}, {"fresh", "1"}};
  store.restore_state(snapshot);

  EXPECT_FALSE(store.contains("stale"));
  EXPECT_FALSE(store.get("stale").has_value());
  expect_value(store, "shared", "new");
  expect_value(store, "fresh", "1");
  EXPECT_EQ(store.snapshot_state(), snapshot);
}

TEST_F(PersistentKVStoreTest, RestoringAnEmptyStateClearsEverything) {
  PersistentKVStore store(path(), fast_options());
  store.set("a", "1");

  store.restore_state(StateMap{});

  EXPECT_FALSE(store.contains("a"));
  EXPECT_TRUE(store.snapshot_state().empty());
  EXPECT_EQ(read_db_file(), kvb1({}));
  EXPECT_EQ(read_wal_file().size(), 0u);
}

TEST_F(PersistentKVStoreTest, RestoreStatePersistsTheSnapshotAndResetsTheWal) {
  PersistentKVStore store(path(), fast_options());
  store.set("stale", "gone");
  ASSERT_GT(read_wal_file().size(), 0u);
  ASSERT_FALSE(std::filesystem::exists(db_path_));

  store.restore_state(StateMap{{"only", "entry"}});

  // Both halves of R3.7 in one place: the base file is the snapshot, and the
  // WAL that used to describe the old state is empty.
  EXPECT_EQ(read_db_file(), kvb1({{"only", "entry"}}));
  EXPECT_EQ(read_wal_file().size(), 0u);
}

TEST_F(PersistentKVStoreTest, AFreshStoreOnTheSamePathSeesTheRestoredState) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("a", "1");
    store.set("b", "2");
    store.restore_state(StateMap{{"x", "10"}, {"y", "20"}});
  }

  // Nothing else ran between the restore and this reopen, which is the shape
  // of "the process was killed one instruction after the restore" (R3.7).
  const PersistentKVStore reloaded(path(), fast_options());

  const StateMap expected{{"x", "10"}, {"y", "20"}};
  EXPECT_EQ(reloaded.snapshot_state(), expected);
  EXPECT_FALSE(reloaded.contains("a"));
  EXPECT_FALSE(reloaded.contains("b"));
}

TEST_F(PersistentKVStoreTest, WalRecordsWrittenBeforeARestoreAreNotReplayed) {
  // The failure this exists to catch: leave the WAL in place across a restore
  // and the next start replays pre-restore commands *over* the snapshot,
  // reconstructing a state no replica ever had - silently, and with no error
  // anywhere. Both writes below live only in the WAL, so if the reset were
  // skipped they would come straight back.
  {
    PersistentKVStore store(path(), fast_options());
    store.set("pre", "restore");
    store.set("also-pre", "restore");
    ASSERT_FALSE(std::filesystem::exists(db_path_)) << "no base file yet";
    ASSERT_GT(read_wal_file().size(), 0u);

    store.restore_state(StateMap{{"snap", "1"}});
    ASSERT_EQ(read_wal_file().size(), 0u);
  }

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "snap", "1");
  EXPECT_FALSE(reloaded.contains("pre"));
  EXPECT_FALSE(reloaded.contains("also-pre"));
  EXPECT_EQ(reloaded.snapshot_state(), (StateMap{{"snap", "1"}}));
}

TEST_F(PersistentKVStoreTest, WritesAfterARestoreAreLoggedAndSurvive) {
  // The WAL has to be usable again after being reset - the append fd survives
  // the truncation, and a write that lands after a restore is as durable as
  // any other.
  {
    PersistentKVStore store(path(), fast_options());
    store.set("before", "gone");
    store.restore_state(StateMap{{"snap", "1"}});

    store.set("after", "2");
    ASSERT_TRUE(store.remove("snap"));
    store.set("snap", "3");
  }

  const PersistentKVStore reloaded(path(), fast_options());

  expect_value(reloaded, "after", "2");
  expect_value(reloaded, "snap", "3");
  EXPECT_FALSE(reloaded.contains("before"));
}

TEST_F(PersistentKVStoreTest, SnapshotStateIsConsistentUnderConcurrentWrites) {
  // snapshot_state() copies under the same mutex every writer takes, so a
  // snapshot may be missing a key but can never contain a torn one or a
  // dangling node. This is also the deadlock check: the copy must not re-enter
  // a locking method, and std::mutex is not recursive.
  constexpr int kWriters = 4;
  constexpr int kWritesPerWriter = 50;

  PersistentKVStore store(path(), fast_options());

  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([&store, writer] {
      for (int i = 0; i < kWritesPerWriter; ++i) {
        const std::string suffix =
            std::to_string(writer) + ":" + std::to_string(i);
        store.set("key" + suffix, "value" + suffix);
      }
    });
  }

  // Every writer's value is derived from its key, so a snapshot that observed
  // a half-applied write shows up as a mismatch. Recorded rather than
  // asserted on the spot: the writers are still running, and an ASSERT_ here
  // would return from the test with joinable threads and abort the binary.
  std::string torn;
  for (int round = 0; round < 100 && torn.empty(); ++round) {
    for (const auto &[key, value] : store.snapshot_state()) {
      if (value != "value" + key.substr(3)) {
        torn = key + " -> " + value;
        break;
      }
    }
  }

  for (std::thread &writer : writers) {
    writer.join();
  }

  EXPECT_TRUE(torn.empty()) << "torn snapshot entry: " << torn;
  EXPECT_EQ(store.snapshot_state().size(),
            static_cast<size_t>(kWriters) * kWritesPerWriter);
}

// --- Ordered key index (console phase) -------------------------------------
//
// The index holds string_views into store_'s own key strings. These tests exist
// because a dangling view does not crash reliably -- it reads as a corrupted
// key -- so every path that can create or destroy a map node is exercised here.
//
// Written against the file's existing fixture rather than a TempDir helper:
// PersistentKVStoreTest already owns a per-test base/WAL path pair and cleans
// both up, which is exactly what these need.

TEST_F(PersistentKVStoreTest, IndexTracksInsertUpdateAndErase) {
  PersistentKVStore store(path(), fast_options());

  store.set("b", "1");
  store.set("a", "1");
  store.set("c", "1");
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "b", "c"}));

  // An update must NOT double-register: insert_or_assign reuses the node.
  store.set("b", "2");
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "b", "c"}));

  EXPECT_TRUE(store.remove("b"));
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "c"}));

  // Removing a key that was never there must not touch the index.
  EXPECT_FALSE(store.remove("zz"));
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "c"}));
}

TEST_F(PersistentKVStoreTest, IndexIsByteTransparentlyOrdered) {
  PersistentKVStore store(path(), fast_options());

  // Keys with '=', newlines and NUL bytes round-trip and order exactly. The
  // NUL case is the one a naive exclusive cursor would get wrong.
  const std::string with_nul("a\0b", 3);
  store.set("a=b", "1");
  store.set("a\nb", "1");
  store.set(with_nul, "1");
  store.set("a", "1");

  // Byte order: "a" < "a\0b" < "a\nb" < "a=b"  (0x00 < 0x0a < 0x3d)
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", with_nul, "a\nb", "a=b"}));
}

TEST_F(PersistentKVStoreTest, IndexSurvivesRestoreState) {
  PersistentKVStore store(path(), fast_options());
  store.set("gone", "1");

  StateMap replacement;
  replacement["x"] = "1";
  replacement["y"] = "1";
  store.restore_state(std::move(replacement));

  // A whole-map replace kills every old node, so every old view must be gone.
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"x", "y"}));
}

TEST_F(PersistentKVStoreTest, IndexRebuiltFromDiskOnReopen) {
  {
    PersistentKVStore store(path(), fast_options());
    store.set("k2", "1");
    store.set("k1", "1");
    store.set("k3", "1");
    EXPECT_TRUE(store.remove("k2"));
  }
  // Reopen: base-file load plus WAL replay must both feed the index.
  PersistentKVStore reopened(path(), fast_options());
  EXPECT_EQ(reopened.ordered_keys_for_test(),
            (std::vector<std::string>{"k1", "k3"}));
}

TEST_F(PersistentKVStoreTest, IndexSurvivesCompaction) {
  DurabilityOptions options = fast_options();
  options.wal_max_records = 4; // force several compactions
  PersistentKVStore store(path(), options);

  for (int i = 0; i < 40; ++i) {
    store.set("k" + std::to_string(i % 7), std::to_string(i));
  }
  // Compaction rewrites the base file from store_ but never touches the map,
  // so no view may move. A no-op today; pinned so it stays one.
  EXPECT_EQ(store.ordered_keys_for_test().size(), 7u);
}

TEST_F(PersistentKVStoreTest, IndexRebuiltFromLegacyBaseFile) {
  write_db_file("b=2\na=1\nc=3\n");

  PersistentKVStore store(path(), fast_options());
  EXPECT_EQ(store.ordered_keys_for_test(),
            (std::vector<std::string>{"a", "b", "c"}));
}

} // namespace
} // namespace kvdb
