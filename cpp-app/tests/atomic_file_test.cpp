/**
 * @file atomic_file_test.cpp
 * @brief Unit tests for atomic_write_file() / read_file() (spec R2.2).
 *
 * A unit test cannot pull the power cord, so what is testable here is the
 * observable contract that makes the crash-safety argument hold:
 *
 *   - the temp file is "<path>.tmp" and never survives a call, successful or
 *     not - leftover litter in the data directory is how a "safe" persist path
 *     quietly fills a disk;
 *   - the target is replaced wholesale (no overlay of the old bytes) and
 *     round-trips arbitrary binary content, including NUL, '=' and '\n';
 *   - a missing file reads back as std::nullopt while an unreadable one throws.
 *     Conflating the two would turn an I/O fault on a full node into a silently
 *     empty store, which is a data-loss bug, not an error-reporting nit;
 *   - failures throw std::runtime_error naming the path an operator has to go
 *     and look at.
 *
 * The fsync calls themselves are not directly observable from user space; they
 * are covered by review and by the e2e docker-kill test (R2 acceptance).
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include "storage/atomic_file.hpp"

namespace kvdb {
namespace {

/** @brief std::string from a string literal, embedded NUL bytes included. */
template <size_t N> std::string bytes(const char (&literal)[N]) {
  return std::string(literal, N - 1);
}

class AtomicFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    static int counter = 0;
    const ::testing::TestInfo *info =
        ::testing::UnitTest::GetInstance()->current_test_info();

    std::string name = "kvdb_atomic_file_test_";
    name += info->name();
    name += "_";
    name += std::to_string(counter++);

    dir_ = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir_));
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  [[nodiscard]] std::string path_in(const std::string &name) const {
    return (dir_ / name).string();
  }

  /** @brief The file under test, named after the real one. */
  [[nodiscard]] std::string db_path() const { return path_in("kv.db"); }

  /** @brief The scratch file atomic_write_file() is required to use (R2.2). */
  [[nodiscard]] std::string temp_path() const { return path_in("kv.db.tmp"); }

  /** @brief Number of entries currently in the test directory. */
  [[nodiscard]] size_t directory_entry_count() const {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir_)) {
      static_cast<void>(entry);
      ++count;
    }
    return count;
  }

  std::filesystem::path dir_;
};

// --- Round trip -------------------------------------------------------------

TEST_F(AtomicFileTest, WriteThenReadRoundTripsTextContent) {
  atomic_write_file(db_path(), "hello world");

  const std::optional<std::string> contents = read_file(db_path());

  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(*contents, "hello world");
}

TEST_F(AtomicFileTest, WriteThenReadRoundTripsBinaryContent) {
  // Shaped like a real R2.1 base file: magic, a count, and a payload carrying
  // every byte the old line format used to mangle.
  const std::string binary =
      bytes("KVB1") + bytes("\x01\x00\x00\x00") + bytes("a\0=\nb");

  atomic_write_file(db_path(), binary);
  const std::optional<std::string> contents = read_file(db_path());

  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(contents->size(), binary.size());
  EXPECT_EQ(*contents, binary);
}

TEST_F(AtomicFileTest, WritingEmptyContentCreatesAnEmptyFile) {
  atomic_write_file(db_path(), "");

  ASSERT_TRUE(std::filesystem::exists(db_path()));
  EXPECT_EQ(std::filesystem::file_size(db_path()), 0u);

  const std::optional<std::string> contents = read_file(db_path());
  ASSERT_TRUE(contents.has_value());
  EXPECT_TRUE(contents->empty());
}

TEST_F(AtomicFileTest, RoundTripsContentLargerThanOneIoChunk) {
  // Exercises both loops: the write() loop over a buffer a single call may not
  // consume, and the read() loop over more than kFileReadChunkSize bytes.
  const size_t size = 5u * kFileReadChunkSize / 2;
  std::string large;
  large.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    large.push_back(static_cast<char>(i % 256));
  }

  atomic_write_file(db_path(), large);
  const std::optional<std::string> contents = read_file(db_path());

  ASSERT_TRUE(contents.has_value());
  ASSERT_EQ(contents->size(), large.size());
  EXPECT_EQ(*contents, large);
}

// --- Replacement and temp-file hygiene -------------------------------------

TEST_F(AtomicFileTest, SuccessfulWriteLeavesNoTempFileBehind) {
  atomic_write_file(db_path(), "payload");

  EXPECT_TRUE(std::filesystem::exists(db_path()));
  EXPECT_FALSE(std::filesystem::exists(temp_path()));
  EXPECT_EQ(directory_entry_count(), 1u);
}

TEST_F(AtomicFileTest, OverwriteReplacesContentWholesaleAndLeavesNoTempFile) {
  atomic_write_file(db_path(), "the first version, which is deliberately long");
  atomic_write_file(db_path(), "short");

  const std::optional<std::string> contents = read_file(db_path());

  ASSERT_TRUE(contents.has_value());
  // Not "shortt version, which is..." - the old bytes are gone, not overlaid.
  EXPECT_EQ(*contents, "short");
  EXPECT_FALSE(std::filesystem::exists(temp_path()));
  EXPECT_EQ(directory_entry_count(), 1u);
}

TEST_F(AtomicFileTest, StaleTempFileFromAPreviousCrashIsReplaced) {
  // A crash between open() and rename() leaves a partial "<path>.tmp". The next
  // persist must truncate it rather than append to or trip over it.
  {
    std::ofstream stale(temp_path(), std::ios::binary);
    stale << "garbage left over by a crashed process";
  }
  ASSERT_TRUE(std::filesystem::exists(temp_path()));

  atomic_write_file(db_path(), "fresh");

  EXPECT_FALSE(std::filesystem::exists(temp_path()));
  const std::optional<std::string> contents = read_file(db_path());
  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(*contents, "fresh");
}

TEST_F(AtomicFileTest, RepeatedWritesNeverAccumulateFiles) {
  for (int i = 0; i < 10; ++i) {
    atomic_write_file(db_path(), "iteration " + std::to_string(i));
  }

  EXPECT_EQ(directory_entry_count(), 1u);
  const std::optional<std::string> contents = read_file(db_path());
  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(*contents, "iteration 9");
}

TEST_F(AtomicFileTest, RelativePathWithNoSlashWritesIntoTheCurrentDirectory) {
  // Covers the "no '/' in the path" branch of the parent-directory derivation,
  // which has to resolve to "." - an empty string there would make the
  // directory fsync fail with ENOENT and turn a good write into a throw.
  struct CwdRestore {
    std::filesystem::path previous;
    ~CwdRestore() {
      std::error_code ec;
      std::filesystem::current_path(previous, ec);
    }
  } restore{std::filesystem::current_path()};
  std::filesystem::current_path(dir_);

  atomic_write_file("relative.db", "relative-payload");

  const std::optional<std::string> contents = read_file("relative.db");
  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(*contents, "relative-payload");
  EXPECT_TRUE(std::filesystem::exists(dir_ / "relative.db"));
  EXPECT_FALSE(std::filesystem::exists(dir_ / "relative.db.tmp"));
}

// --- read_file: absent vs. unreadable --------------------------------------

TEST_F(AtomicFileTest, ReadFileOfAMissingPathReturnsNullopt) {
  ASSERT_FALSE(std::filesystem::exists(db_path()));

  std::optional<std::string> contents;
  ASSERT_NO_THROW(contents = read_file(db_path()));

  // Not an empty string, not a throw: a fresh node has no kv.db yet.
  EXPECT_FALSE(contents.has_value());
}

TEST_F(AtomicFileTest, ReadFileInAMissingDirectoryReturnsNullopt) {
  EXPECT_FALSE(read_file(path_in("no-such-dir/kv.db")).has_value());
}

TEST_F(AtomicFileTest, MissingFileAndEmptyFileAreDistinguishable) {
  EXPECT_FALSE(read_file(db_path()).has_value());

  atomic_write_file(db_path(), "");

  const std::optional<std::string> contents = read_file(db_path());
  ASSERT_TRUE(contents.has_value());
  EXPECT_TRUE(contents->empty());
}

TEST_F(AtomicFileTest, ReadFileThroughANonDirectoryComponentThrows) {
  // ENOENT is "absent"; ENOTDIR is a broken path an operator must be told
  // about. Returning nullopt here would fabricate an empty store.
  atomic_write_file(db_path(), "i am a regular file");

  EXPECT_THROW(static_cast<void>(read_file(path_in("kv.db/nested"))),
               std::runtime_error);
}

// --- Failure reporting ------------------------------------------------------

TEST_F(AtomicFileTest, WriteIntoAMissingDirectoryThrowsNamingThePath) {
  const std::string target = path_in("no-such-dir/kv.db");
  ASSERT_FALSE(std::filesystem::exists(dir_ / "no-such-dir"));

  try {
    atomic_write_file(target, "payload");
    FAIL() << "expected atomic_write_file to throw";
  } catch (const std::runtime_error &error) {
    const std::string message = error.what();
    // Naming the path is the whole value of the message: a node that dies on
    // startup must say which file it could not write.
    EXPECT_NE(message.find(target), std::string::npos) << message;
  }

  EXPECT_FALSE(std::filesystem::exists(target));
  EXPECT_EQ(directory_entry_count(), 0u);
}

TEST_F(AtomicFileTest, FailedWriteLeavesNoTempFileInTheDataDirectory) {
  // Fail late on purpose: the target is an existing directory, so the temp file
  // is created, written and fsynced successfully and only rename() fails. That
  // is the path where cleanup actually matters.
  const std::string target = path_in("target");
  ASSERT_TRUE(std::filesystem::create_directory(target));

  EXPECT_THROW(atomic_write_file(target, "payload"), std::runtime_error);

  EXPECT_FALSE(std::filesystem::exists(target + ".tmp"));
  EXPECT_TRUE(std::filesystem::is_directory(target));
  EXPECT_EQ(directory_entry_count(), 1u);
}

TEST_F(AtomicFileTest, FailedWriteDoesNotDisturbOtherFiles) {
  atomic_write_file(db_path(), "the good contents");

  // Fails at rename() (the target is a directory) after the temp file has been
  // created next to kv.db. Neither kv.db nor the directory may be affected.
  const std::string target = path_in("a-directory");
  ASSERT_TRUE(std::filesystem::create_directory(target));
  EXPECT_THROW(atomic_write_file(target, "payload"), std::runtime_error);

  const std::optional<std::string> contents = read_file(db_path());
  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(*contents, "the good contents");
  EXPECT_FALSE(std::filesystem::exists(target + ".tmp"));
  EXPECT_EQ(directory_entry_count(), 2u); // kv.db and a-directory, nothing else
}

} // namespace
} // namespace kvdb
