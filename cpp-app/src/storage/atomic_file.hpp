#pragma once

/**
 * @file atomic_file.hpp
 * @brief Crash-safe whole-file write and whole-file read (R2.2).
 *
 * atomic_write_file() is the "write to <path>.tmp, fsync, rename over <path>,
 * fsync the directory" dance. All four steps matter:
 *
 *   - writing to a temp file means a crash can never leave a half-written
 *     <path> behind, only a half-written <path>.tmp that nothing reads;
 *   - fsync of the temp file forces the data out before the rename, otherwise
 *     the rename can land while the contents are still in page cache;
 *   - rename() is atomic within a filesystem, so a reader sees either the whole
 *     old file or the whole new one;
 *   - fsync of the containing directory is what makes the rename itself
 *     durable. Skipping it is the classic bug: the file survives, the directory
 *     entry pointing at it does not.
 *
 * This is deliberately POSIX fds rather than std::ofstream - iostreams have no
 * way to fsync, which makes every guarantee above unenforceable.
 *
 * Note for macOS developers: fsync() there flushes to the drive but does not
 * force a platter/cache flush (that needs fcntl(F_FULLFSYNC)). The deployment
 * target and CI are Linux, where fsync() is sufficient.
 */

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace kvdb {

/** @brief Suffix of the scratch file atomic_write_file() renames from. */
inline constexpr const char *kAtomicWriteTempSuffix = ".tmp";

/** @brief Permissions of files created by atomic_write_file(): rw-r--r--. */
inline constexpr mode_t kAtomicWriteFileMode = 0644;

/** @brief Buffer size of one read() in read_file(). */
inline constexpr size_t kFileReadChunkSize = 64u * 1024;

/**
 * @brief Low-level POSIX helpers shared by the durability layer.
 *
 * Kept public (rather than in an anonymous namespace) so the WAL can reuse the
 * same fd ownership and retry semantics instead of reimplementing them.
 */
namespace fileio {

/**
 * @brief RAII owner of a POSIX file descriptor.
 *
 * Every exit path from atomic_write_file()/read_file() runs through this, so a
 * throw part-way through cannot leak an fd - which under a long-lived process
 * like kvdb_node would be a slow-motion EMFILE.
 */
class FdGuard {
public:
  explicit FdGuard(int fd = -1) noexcept : fd_(fd) {}

  ~FdGuard() { reset(); }

  FdGuard(const FdGuard &) = delete;
  FdGuard &operator=(const FdGuard &) = delete;

  FdGuard(FdGuard &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  FdGuard &operator=(FdGuard &&other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }

  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  /**
   * @brief Close the descriptor now and report whether close() succeeded.
   *
   * Worth checking explicitly on a write path: some filesystems only surface a
   * write error at close() time. close() is never retried on EINTR - POSIX
   * leaves the fd state unspecified and on Linux it is already closed, so a
   * retry would race another thread's fd.
   */
  int close_checked() noexcept {
    if (fd_ < 0) {
      return 0;
    }
    const int result = ::close(fd_);
    fd_ = -1;
    return result;
  }

  /** @brief Close and forget the descriptor, ignoring any error. */
  void reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_;
};

/**
 * @brief Unlinks a path on destruction unless release() was called.
 *
 * Used to make sure a failed persist does not leave a stale "<path>.tmp"
 * lying around in the data directory.
 */
class TempFileGuard {
public:
  explicit TempFileGuard(std::string path) : path_(std::move(path)) {}

  ~TempFileGuard() {
    if (armed_) {
      // Best effort: this runs while an exception is already propagating, so
      // there is nothing useful to do with a failure here.
      ::unlink(path_.c_str());
    }
  }

  TempFileGuard(const TempFileGuard &) = delete;
  TempFileGuard &operator=(const TempFileGuard &) = delete;

  /** @brief Stop the destructor from unlinking the path. */
  void release() noexcept { armed_ = false; }

private:
  std::string path_;
  bool armed_ = true;
};

/**
 * @brief Throw a std::runtime_error naming the operation, path and errno.
 *
 * @p err is taken by value rather than read from errno inside, because
 * building the message would clobber errno before it could be read.
 */
[[noreturn]] inline void throw_errno(const char *what, const std::string &path,
                                     int err) {
  // std::generic_category().message() is the thread-safe strerror().
  throw std::runtime_error(std::string(what) + " \"" + path +
                           "\": " + std::generic_category().message(err) +
                           " (errno " + std::to_string(err) + ")");
}

/**
 * @brief write() the whole buffer, tolerating short writes and EINTR.
 *
 * A single write() is not guaranteed to consume the whole buffer, so treating
 * one call as "done" silently truncates large files.
 *
 * @return true on success; on failure errno describes the reason.
 */
[[nodiscard]] inline bool write_all(int fd, const char *data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const ssize_t result = ::write(fd, data + written, size - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      // No progress and no error: spinning here would hang the node, so treat
      // it as a hard I/O failure.
      errno = EIO;
      return false;
    }
    written += static_cast<size_t>(result);
  }
  return true;
}

/**
 * @brief fsync() retrying on EINTR.
 * @return true on success; on failure errno describes the reason.
 */
[[nodiscard]] inline bool fsync_retry(int fd) {
  while (::fsync(fd) != 0) {
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

/**
 * @brief Directory component of @p path.
 *
 * "dir/kv.db" -> "dir", "/kv.db" -> "/", "kv.db" -> "." (the current
 * directory). Returning "" for the last case would make open() fail with
 * ENOENT and turn a perfectly good relative path into a persist failure.
 */
[[nodiscard]] inline std::string parent_directory(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

/**
 * @brief fsync a directory so a rename inside it becomes durable.
 * @return true on success; on failure errno describes the reason.
 */
[[nodiscard]] inline bool fsync_directory(const std::string &directory) {
  FdGuard dir_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY));
  if (!dir_fd.valid()) {
    return false;
  }
  if (!fsync_retry(dir_fd.get())) {
    return false;
  }
  return dir_fd.close_checked() == 0;
}

} // namespace fileio

/**
 * @brief Write @p contents to @p path atomically and durably (R2.2).
 *
 * Writes "<path>.tmp", fsyncs it, renames it over @p path, then fsyncs the
 * containing directory. After this returns, a `kill -9` cannot lose or corrupt
 * @p path: a reader sees either the complete previous contents or the complete
 * new contents.
 *
 * The temp file is unlinked on every path, so a failed persist never litters
 * the data directory.
 *
 * @throws std::runtime_error naming the path and errno on any failure. Up to
 *         and including the rename, a throw means @p path is untouched. A
 *         throw from the final directory fsync means the new contents ARE
 *         installed but their durability is unconfirmed - a caller must not
 *         treat that as "the write did not happen".
 */
inline void atomic_write_file(const std::string &path,
                              const std::string &contents) {
  const std::string temp_path = path + kAtomicWriteTempSuffix;
  fileio::TempFileGuard cleanup(temp_path);

  // Declared after `cleanup` so it is destroyed first: close the fd, then
  // unlink, never the other way round.
  fileio::FdGuard fd(::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                            kAtomicWriteFileMode));
  if (!fd.valid()) {
    const int err = errno;
    // Nothing was created, so there is nothing of ours to remove.
    cleanup.release();
    fileio::throw_errno("cannot create temp file", temp_path, err);
  }

  if (!fileio::write_all(fd.get(), contents.data(), contents.size())) {
    const int err = errno;
    fileio::throw_errno("cannot write temp file", temp_path, err);
  }

  if (!fileio::fsync_retry(fd.get())) {
    const int err = errno;
    fileio::throw_errno("cannot fsync temp file", temp_path, err);
  }

  if (fd.close_checked() != 0) {
    const int err = errno;
    fileio::throw_errno("cannot close temp file", temp_path, err);
  }

  // std::rename is POSIX rename() here: replacing an existing target is atomic
  // within a filesystem, which is the entire point of the temp-file dance.
  if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
    const int err = errno;
    fileio::throw_errno("cannot rename temp file onto", path, err);
  }

  // The rename consumed the temp file, so from here on there is nothing left
  // to clean up. Disarm before the directory fsync, whose failure must not
  // trigger an unlink of a path that no longer exists.
  cleanup.release();

  if (!fileio::fsync_directory(fileio::parent_directory(path))) {
    const int err = errno;
    fileio::throw_errno("cannot fsync parent directory of", path, err);
  }
}

/**
 * @brief Read a whole file into memory.
 *
 * @return The file contents, or std::nullopt when the file does not exist.
 *         "Absent" and "unreadable" are deliberately not conflated: a missing
 *         kv.db means a fresh node, while an unreadable one means the operator
 *         has a problem that must not be silently turned into an empty store.
 * @throws std::runtime_error naming the path and errno on a real I/O error.
 */
[[nodiscard]] inline std::optional<std::string>
read_file(const std::string &path) {
  fileio::FdGuard fd(::open(path.c_str(), O_RDONLY));
  if (!fd.valid()) {
    const int err = errno;
    if (err == ENOENT) {
      return std::nullopt;
    }
    fileio::throw_errno("cannot open file", path, err);
  }

  std::string contents;
  std::array<char, kFileReadChunkSize> buffer{};
  while (true) {
    const ssize_t result = ::read(fd.get(), buffer.data(), buffer.size());
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int err = errno;
      fileio::throw_errno("cannot read file", path, err);
    }
    if (result == 0) {
      break;
    }
    contents.append(buffer.data(), static_cast<size_t>(result));
  }

  if (fd.close_checked() != 0) {
    const int err = errno;
    fileio::throw_errno("cannot close file", path, err);
  }
  return contents;
}

} // namespace kvdb
