/**
 * @file kv_store_test.cpp
 * @brief Unit tests for PersistentKVStore (spec R0.11).
 *
 * Two jobs here:
 *   1. Pin the basic IKVStore contract (set/get/remove/contains, reload).
 *   2. Pin the CURRENT LOSSY on-disk format. `persist()` writes one
 *      "key=value\n" line per entry and `load()` splits each line at the FIRST
 *      '=', dropping any line without one. That mangles keys containing '=' and
 *      truncates anything containing a newline. Phase 2 (R2.1) replaces this
 *      with a length-prefixed binary format; these tests are the "before"
 *      picture that proves it.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include "storage/kv_store.hpp"

namespace kvdb {
namespace {

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
    remove_db_file();
  }

  void TearDown() override { remove_db_file(); }

  [[nodiscard]] std::string path() const { return db_path_.string(); }

  /** @brief Read the whole db file back, byte for byte. */
  [[nodiscard]] std::string read_file() const {
    std::ifstream file(db_path_, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
  }

  /** @brief Overwrite the db file behind the store's back. */
  void write_file(const std::string &contents) const {
    std::ofstream file(db_path_, std::ios::binary | std::ios::trunc);
    file << contents;
  }

  std::filesystem::path db_path_;

private:
  void remove_db_file() const {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
  }
};

// --- Basic contract -------------------------------------------------------

TEST_F(PersistentKVStoreTest, ConstructionOnMissingFileYieldsEmptyStore) {
  ASSERT_FALSE(std::filesystem::exists(db_path_));

  std::unique_ptr<PersistentKVStore> store;
  EXPECT_NO_THROW(store = std::make_unique<PersistentKVStore>(path()));

  ASSERT_NE(store, nullptr);
  EXPECT_FALSE(store->contains("anything"));
  EXPECT_FALSE(store->get("anything").has_value());
  // load() opens the file read-only, so a pure reader never creates it.
  EXPECT_FALSE(std::filesystem::exists(db_path_));
}

TEST_F(PersistentKVStoreTest, GetOnAbsentKeyReturnsNullopt) {
  PersistentKVStore store(path());

  EXPECT_FALSE(store.get("nope").has_value());
  EXPECT_FALSE(store.contains("nope"));
}

TEST_F(PersistentKVStoreTest, SetThenGetAndContains) {
  PersistentKVStore store(path());

  store.set("alpha", "beta");

  ASSERT_TRUE(store.get("alpha").has_value());
  EXPECT_EQ(*store.get("alpha"), "beta");
  EXPECT_TRUE(store.contains("alpha"));
  EXPECT_FALSE(store.contains("Alpha")); // keys are case sensitive
}

TEST_F(PersistentKVStoreTest, RemoveExistingKeyReturnsTrueAndPersists) {
  PersistentKVStore store(path());
  store.set("alpha", "beta");

  EXPECT_TRUE(store.remove("alpha"));

  EXPECT_FALSE(store.contains("alpha"));
  EXPECT_FALSE(store.get("alpha").has_value());
  EXPECT_EQ(read_file(), ""); // the rewrite emptied the file
}

TEST_F(PersistentKVStoreTest,
       RemoveAbsentKeyReturnsFalseAndDoesNotRewriteFile) {
  PersistentKVStore store(path());
  store.set("alpha", "beta");

  // Clobber the file behind the store's back; a rewrite would restore
  // "alpha=beta\n" from the in-memory map.
  const std::string sentinel = "SENTINEL_NOT_REWRITTEN\n";
  write_file(sentinel);

  EXPECT_FALSE(store.remove("missing"));
  EXPECT_EQ(read_file(), sentinel);

  // ...and removing a key that IS present does rewrite.
  EXPECT_TRUE(store.remove("alpha"));
  EXPECT_EQ(read_file(), "");
}

TEST_F(PersistentKVStoreTest, OverwriteDoesNotAccumulateDuplicateLines) {
  PersistentKVStore store(path());

  store.set("k", "v1");
  store.set("k", "v2");

  // persist() truncates and rewrites the whole map, so there is exactly one
  // line for the key.
  EXPECT_EQ(read_file(), "k=v2\n");

  const PersistentKVStore reloaded(path());
  ASSERT_TRUE(reloaded.get("k").has_value());
  EXPECT_EQ(*reloaded.get("k"), "v2");
}

TEST_F(PersistentKVStoreTest, PersistenceRoundTripAcrossInstances) {
  {
    PersistentKVStore store(path());
    store.set("one", "1");
    store.set("two", "2");
    store.set("three", "3");
    EXPECT_TRUE(store.remove("two"));
  }

  const PersistentKVStore reloaded(path());

  ASSERT_TRUE(reloaded.get("one").has_value());
  EXPECT_EQ(*reloaded.get("one"), "1");
  ASSERT_TRUE(reloaded.get("three").has_value());
  EXPECT_EQ(*reloaded.get("three"), "3");
  EXPECT_FALSE(reloaded.contains("two"));
}

// --- On-disk format -------------------------------------------------------

TEST_F(PersistentKVStoreTest, PersistUsesKeyEqualsValueLineFormat) {
  PersistentKVStore store(path());

  store.set("alpha", "beta");

  // Single entry, so the unordered_map iteration order cannot matter.
  EXPECT_EQ(read_file(), "alpha=beta\n");
}

TEST_F(PersistentKVStoreTest, EmptyValueRoundTrips) {
  {
    PersistentKVStore store(path());
    store.set("k", "");
    EXPECT_EQ(read_file(), "k=\n");
  }

  const PersistentKVStore reloaded(path());

  ASSERT_TRUE(reloaded.get("k").has_value());
  EXPECT_EQ(*reloaded.get("k"), "");
}

TEST_F(PersistentKVStoreTest, LoadSkipsLinesWithoutAnEqualsSign) {
  write_file("garbage-line\nk=v\n\nanother\n");

  const PersistentKVStore store(path());

  ASSERT_TRUE(store.get("k").has_value());
  EXPECT_EQ(*store.get("k"), "v");
  EXPECT_FALSE(store.contains("garbage-line"));
  EXPECT_FALSE(store.contains("another"));
}

// --- CURRENT LOSSY BEHAVIOR (pinned for Phase 2) --------------------------

TEST_F(PersistentKVStoreTest, ValueContainingEqualsSurvivesReload) {
  // load() splits at the FIRST '=', so extra '=' inside the *value* is safe.
  {
    PersistentKVStore store(path());
    store.set("k", "a=b=c");
    EXPECT_EQ(read_file(), "k=a=b=c\n");
  }

  const PersistentKVStore reloaded(path());

  ASSERT_TRUE(reloaded.get("k").has_value());
  EXPECT_EQ(*reloaded.get("k"), "a=b=c");
}

TEST_F(PersistentKVStoreTest, KeyContainingEqualsIsCorruptedByReload) {
  // CURRENT LOSSY BEHAVIOR, pinned so Phase 2 (R2.1, length-prefixed binary
  // format) can prove it is fixed: the key "a=b" is written as "a=b=v" and read
  // back as key "a" with value "b=v" - the key that was written is gone.
  {
    PersistentKVStore store(path());
    store.set("a=b", "v");
    EXPECT_EQ(read_file(), "a=b=v\n");
    ASSERT_TRUE(store.get("a=b").has_value()); // still fine in memory
    EXPECT_EQ(*store.get("a=b"), "v");
  }

  const PersistentKVStore reloaded(path());

  EXPECT_FALSE(reloaded.get("a=b").has_value());
  ASSERT_TRUE(reloaded.get("a").has_value());
  EXPECT_EQ(*reloaded.get("a"), "b=v");
}

TEST_F(PersistentKVStoreTest, ValueContainingNewlineIsTruncatedByReload) {
  // CURRENT LOSSY BEHAVIOR, pinned for Phase 2: the value spans two lines on
  // disk; the second line has no '=' so load() drops it entirely and the value
  // silently loses everything after the first newline.
  {
    PersistentKVStore store(path());
    store.set("k", "line1\nline2");
    EXPECT_EQ(read_file(), "k=line1\nline2\n");
  }

  const PersistentKVStore reloaded(path());

  ASSERT_TRUE(reloaded.get("k").has_value());
  EXPECT_EQ(*reloaded.get("k"), "line1");
  EXPECT_FALSE(reloaded.contains("line2"));
}

TEST_F(PersistentKVStoreTest, KeyContainingNewlineIsReplacedByReload) {
  // CURRENT LOSSY BEHAVIOR, pinned for Phase 2: "a\nb" is written as two lines,
  // "a" (dropped: no '=') and "b=v", so reload invents a key "b" that was never
  // written and loses the one that was.
  {
    PersistentKVStore store(path());
    store.set("a\nb", "v");
    EXPECT_EQ(read_file(), "a\nb=v\n");
  }

  const PersistentKVStore reloaded(path());

  EXPECT_FALSE(reloaded.get("a\nb").has_value());
  ASSERT_TRUE(reloaded.get("b").has_value());
  EXPECT_EQ(*reloaded.get("b"), "v");
}

TEST_F(PersistentKVStoreTest, ValueContainingNulSurvivesReload) {
  // NUL is not special to the line format, so unlike '=' and '\n' it makes it
  // back intact. Pinned so Phase 2 does not regress it.
  const std::string value("a\0b", 3);
  {
    PersistentKVStore store(path());
    store.set("k", value);
  }

  const PersistentKVStore reloaded(path());

  ASSERT_TRUE(reloaded.get("k").has_value());
  EXPECT_EQ(*reloaded.get("k"), value);
  EXPECT_EQ(reloaded.get("k")->size(), 3u);
}

} // namespace
} // namespace kvdb
