#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "atomic_file.hpp"
#include "format.hpp"

namespace kvdb {

/**
 * @brief When the write-ahead log forces appended bytes to stable storage.
 *
 * kAlways is the Phase 2 durability contract: append() has not returned until
 * its bytes survive `kill -9`. kNever exists for tests and benchmarks, where
 * one fsync per record dominates the runtime. The bytes written are identical
 * in both modes - only the fsync differs.
 */
enum class WalSyncMode {
  kAlways, ///< fsync() the file after every append (default).
  kNever,  ///< Never fsync; leave it to the page cache.
};

/** @brief Per-record framing overhead: uint32 length + uint32 CRC. */
inline constexpr std::size_t kWalRecordOverheadBytes =
    2 * sizeof(std::uint32_t);

/** @brief Largest payload one record can frame (the prefix is a uint32). */
inline constexpr std::size_t kWalMaxPayloadBytes =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

/** @brief Permission bits used when the WAL file is first created. */
inline constexpr mode_t kWalFileMode = 0644;

/**
 * @brief Append-only write-ahead log of opaque command payloads (R2.4).
 *
 * Record layout on disk, little-endian and unpadded:
 * @code
 *   uint32 payload_len | payload bytes | uint32 crc32(payload)
 * @endcode
 * The payload is the msgpack-encoded KVCommand exactly as it arrived over the
 * wire; the WAL never parses it, so NUL bytes, newlines and '=' are just
 * bytes.
 *
 * Recovery (R2.5): replay() hands every intact record to a callback in write
 * order. The first damaged record - a short length prefix, a length that
 * overruns the file, a missing or mismatching CRC - ends the replay, and the
 * file is truncated back to where that damaged record started. A crash
 * part-way through an append therefore costs exactly the unacknowledged tail
 * and nothing before it. Records *after* the damaged one are discarded too:
 * once framing is lost there is no way to know they are the records that were
 * meant to follow. The heal is idempotent - replaying the healed file again
 * yields the same records and truncates nothing further.
 *
 * @note Not internally synchronized. PersistentKVStore owns the only instance
 *       and calls into it under its single mutex (CLAUDE.md).
 * @note The counters start at zero and describe what this instance has
 *       written or replayed. Call replay() once at startup before using
 *       size_bytes()/record_count() to drive compaction (R2.6).
 */
class Wal {
public:
  /**
   * @brief Bind a WAL to a path without touching the filesystem.
   *
   * The file is created lazily by the first append(), so a node that only
   * reads never leaves an empty kv.wal behind.
   *
   * @param path      Path of the log file (e.g. "/data/kv.wal").
   * @param sync_mode Whether to fsync() after every append.
   */
  Wal(std::string path, WalSyncMode sync_mode)
      : path_(std::move(path)), sync_mode_(sync_mode) {}

  // Owns a file descriptor (through FdGuard, which closes it): neither
  // copyable nor movable.
  Wal(const Wal &) = delete;
  Wal &operator=(const Wal &) = delete;
  Wal(Wal &&) = delete;
  Wal &operator=(Wal &&) = delete;

  /**
   * @brief Frame @p payload into one record and append it to the log.
   *
   * The append fd is opened once and kept for the object's lifetime - this
   * sits on the hot path of every write, so reopening per call is not
   * affordable. fileio::write_all loops over short writes and EINTR, because
   * a single write() is not required to consume the whole buffer.
   *
   * @param payload Opaque bytes to log (the msgpack command).
   * @throws std::runtime_error on any open/write/fsync failure, or if the
   *         payload is too large to frame in a uint32 length prefix.
   */
  void append(const std::string &payload) {
    if (payload.size() > kWalMaxPayloadBytes) {
      throw std::runtime_error("wal: payload too large for '" + path_ + "'");
    }

    std::string record;
    record.reserve(payload.size() + kWalRecordOverheadBytes);
    format::append_blob(record, payload);
    format::append_u32(record, format::crc32(payload));

    ensure_open();
    if (!fileio::write_all(fd_.get(), record.data(), record.size())) {
      const int err = errno;
      fileio::throw_errno("cannot append to WAL", path_, err);
    }
    // Updated before the fsync so the counters keep describing what is
    // actually in the file even if the fsync then fails.
    size_bytes_ += record.size();
    ++record_count_;

    if (sync_mode_ == WalSyncMode::kAlways && !fileio::fsync_retry(fd_.get())) {
      const int err = errno;
      fileio::throw_errno("cannot fsync WAL", path_, err);
    }
  }

  /**
   * @brief Replay every intact record in write order, healing a torn tail.
   *
   * Resets the counters and repopulates them from the file, so after this
   * call size_bytes()/record_count() describe the healed file on disk. A
   * missing file replays as an empty one and is not created.
   *
   * @param on_record Invoked once per intact record, in write order.
   * @throws std::runtime_error if the file cannot be read or truncated.
   */
  void replay(const std::function<void(const std::string &)> &on_record) {
    record_count_ = 0;
    size_bytes_ = 0;

    const std::optional<std::string> contents = read_file(path_);
    if (!contents.has_value()) {
      return;
    }
    const std::string &data = *contents;

    std::size_t offset = 0;
    while (offset < data.size()) {
      // Where this record starts; the heal truncates back to exactly here.
      const std::size_t record_start = offset;
      std::string payload;
      std::uint32_t stored_crc = 0;

      // These bytes came off disk after a crash and are not to be trusted:
      // read_blob refuses a length prefix that overruns what is actually
      // there, so a garbage length can never drive an allocation or a read
      // past the buffer. read_u32 likewise refuses a short CRC field.
      if (!format::read_blob(data, offset, payload) ||
          !format::read_u32(data, offset, stored_crc) ||
          format::crc32(payload) != stored_crc) {
        truncate_to(record_start);
        return;
      }

      size_bytes_ = offset;
      ++record_count_;
      on_record(payload);
    }
  }

  /**
   * @brief Drop every record: the WAL is now covered by the base file (R2.6).
   * @throws std::runtime_error if the file exists but cannot be truncated.
   */
  void truncate() {
    truncate_to(0);
    record_count_ = 0;
  }

  /** @brief Bytes currently in the log file. */
  [[nodiscard]] std::size_t size_bytes() const { return size_bytes_; }

  /** @brief Records currently in the log file. */
  [[nodiscard]] std::size_t record_count() const { return record_count_; }

  /** @brief Path this log was bound to. */
  [[nodiscard]] const std::string &path() const { return path_; }

private:
  /** @brief Open the append fd on first use; a no-op afterwards. */
  void ensure_open() {
    if (fd_.valid()) {
      return;
    }
    const int fd = ::open(
        path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, kWalFileMode);
    if (fd < 0) {
      const int err = errno;
      fileio::throw_errno("cannot open WAL", path_, err);
    }
    fd_ = fileio::FdGuard(fd);
  }

  /**
   * @brief Shrink the log to @p length bytes and record the new size.
   *
   * Not fsynced: a crash before the truncation reaches disk simply leaves the
   * same torn tail for the next replay to heal again, and the heal is
   * idempotent.
   */
  void truncate_to(std::size_t length) {
    if (::truncate(path_.c_str(), static_cast<off_t>(length)) != 0) {
      const int err = errno;
      // Truncating a log that was never created is a no-op, not a failure.
      const bool nothing_to_truncate = err == ENOENT && length == 0;
      if (!nothing_to_truncate) {
        fileio::throw_errno("cannot truncate WAL", path_, err);
      }
    }
    size_bytes_ = length;
  }

  std::string path_;
  WalSyncMode sync_mode_;
  fileio::FdGuard fd_;
  std::size_t size_bytes_ = 0;
  std::size_t record_count_ = 0;
};

} // namespace kvdb
