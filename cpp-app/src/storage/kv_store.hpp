#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>

#include <msgpack.hpp>

#include "../commands/kv_command.hpp"
#include "../common/log.hpp"
#include "../config/config.hpp"
#include "atomic_file.hpp"
#include "format.hpp"
#include "wal.hpp"

namespace kvdb {

/**
 * @brief The whole key-value state, as the store holds it in memory.
 *
 * Worth a name because it now sits in two contracts rather than one: the base
 * file on disk (R2.1) and the snapshot streamed to another node over gRPC
 * (R3.1) are the *same bytes*, produced from and parsed back into this type.
 */
using StateMap = std::unordered_map<std::string, std::string>;

/** @brief State-image magic (R2.1). Four bytes, not NUL-terminated on disk. */
inline constexpr const char *kStateMagic = "KVB1";
inline constexpr size_t kStateMagicSize = 4;

/**
 * @brief Smallest number of bytes one entry can occupy.
 *
 * Two empty blobs, i.e. two uint32 length prefixes and no payload. Used to
 * bound a declared entry count against the bytes actually available.
 */
inline constexpr size_t kStateMinEntrySize = 8;

/** @brief Does @p bytes start with the state-image magic? */
[[nodiscard]] inline bool has_state_magic(const std::string &bytes) {
  return bytes.size() >= kStateMagicSize &&
         bytes.compare(0, kStateMagicSize, kStateMagic, kStateMagicSize) == 0;
}

/**
 * @brief Encode a whole map as a state image (R2.1 / R3.1).
 *
 * @code
 *   "KVB1" | uint32 entry_count |
 *   entry_count x ( uint32 key_len | key | uint32 value_len | value )
 * @endcode
 *
 * All integers are little-endian and nothing is escaped, so keys and values
 * may contain @c '=', newlines and NUL bytes.
 *
 * This is the single encoder in the program: the base file PersistentKVStore
 * writes and the snapshot the state machine streams to the sidecar are
 * byte-identical, which is what makes "ship the file you already have" a legal
 * implementation of snapshot transfer.
 *
 * @throws std::length_error if a key or value is longer than a uint32 can
 *         describe (from format::append_blob). Truncating the prefix instead
 *         would silently corrupt the image.
 */
[[nodiscard]] inline std::string serialize_state(const StateMap &state) {
  std::string out;
  out.append(kStateMagic, kStateMagicSize);
  // The format caps the map at 2^32-1 entries, which is far beyond what fits
  // in memory here.
  format::append_u32(out, static_cast<uint32_t>(state.size()));
  for (const auto &[key, value] : state) {
    format::append_blob(out, key);
    format::append_blob(out, value);
  }
  return out;
}

/**
 * @brief Decode a state image produced by serialize_state().
 *
 * Everything here is attacker-influenced input - it arrives off disk after a
 * crash, or off the network from a peer - so a length read out of the buffer
 * is never used to index, allocate or advance without first being checked
 * against the bytes that actually remain. The map is built to the side and
 * only returned once the whole image has parsed, so a corrupt image cannot
 * leave a caller with a half-loaded state.
 *
 * @throws std::runtime_error naming what was wrong: a missing magic, a
 *         truncated header or entry, an entry count that disagrees with the
 *         bytes that follow it, or a blob length that overruns the buffer.
 *         The message carries no path, because these bytes need not have come
 *         from a file; callers that have one wrap the message with it.
 */
[[nodiscard]] inline StateMap deserialize_state(const std::string &bytes) {
  if (!has_state_magic(bytes)) {
    throw std::runtime_error("missing the \"KVB1\" magic");
  }

  size_t offset = kStateMagicSize;

  uint32_t entry_count = 0;
  if (!format::read_u32(bytes, offset, entry_count)) {
    throw std::runtime_error("truncated entry count");
  }

  // Bound the count before it is used to size an allocation: a corrupt uint32
  // must not be able to ask for a multi-gigabyte reserve.
  const size_t remaining = bytes.size() - offset;
  if (static_cast<size_t>(entry_count) > remaining / kStateMinEntrySize) {
    throw std::runtime_error("entry count " + std::to_string(entry_count) +
                             " exceeds the " + std::to_string(remaining) +
                             " bytes that follow it");
  }

  StateMap state;
  state.reserve(entry_count); // Safe: bounded by the check above.

  for (uint32_t i = 0; i < entry_count; ++i) {
    std::string key;
    std::string value;
    if (!format::read_blob(bytes, offset, key) ||
        !format::read_blob(bytes, offset, value)) {
      throw std::runtime_error("entry " + std::to_string(i) + " is truncated");
    }
    // Last one wins, matching the legacy reader. The writer never emits
    // duplicates, so this only matters for hand-made images.
    state[std::move(key)] = std::move(value);
  }

  if (offset != bytes.size()) {
    throw std::runtime_error(std::to_string(bytes.size() - offset) +
                             " trailing bytes after " +
                             std::to_string(entry_count) + " entries");
  }

  return state;
}

/**
 * @brief Observability seam over a store: counters, not storage semantics.
 *
 * Deliberately SEPARATE from IKVStore. key_count() and wal_size_bytes() were
 * kept off IKVStore on purpose -- widening the storage interface for
 * observability forces every test fake to implement it -- and that decision
 * still holds. What changed is that the HTTP handler is now a consumer of these
 * counters (GET /cluster/status), and it holds interfaces, not the concrete
 * store. So they get their own seam rather than moving.
 *
 * PersistentKVStore implements both interfaces; main.cpp holds the concrete
 * type and injects it twice, once per interface.
 */
class IStoreStats {
public:
  virtual ~IStoreStats() = default;

  /** @brief Keys currently held in the local store. */
  [[nodiscard]] virtual std::size_t key_count() const = 0;

  /** @brief Current size of the write-ahead log, in bytes. */
  [[nodiscard]] virtual std::size_t wal_size_bytes() const = 0;
};

/**
 * @brief Abstract interface for key-value storage.
 *
 * Follows the Interface Segregation Principle - defines only
 * the essential operations needed for KV storage.
 *
 * Note that durability is deliberately absent from this interface: how (or
 * whether) an implementation reaches the disk is its own business, which is
 * what let Phase 2 replace the persistence strategy wholesale without the
 * state machine or the HTTP handler noticing.
 */
class IKVStore {
public:
  virtual ~IKVStore() = default;

  virtual void set(const std::string &key, const std::string &value) = 0;
  virtual std::optional<std::string> get(const std::string &key) const = 0;
  virtual bool remove(const std::string &key) = 0;
  virtual bool contains(const std::string &key) const = 0;

  /**
   * @brief A consistent copy of the whole state (R3.2).
   *
   * "Consistent" is the whole point: the copy must not observe a write
   * half-applied, which is why it is the store that produces it rather than
   * the caller iterating. What the caller then does with it - serialize it,
   * stream it to a peer - happens outside the store and outside its lock.
   */
  [[nodiscard]] virtual StateMap snapshot_state() const = 0;

  /**
   * @brief Replace the whole state with @p state, durably (R3.2 / R3.7).
   *
   * A replace, not a merge: keys the store holds and @p state does not are
   * gone afterwards. This is the apply side of a raft snapshot install, so
   * anything the local node had that the snapshot does not is by definition
   * not part of the agreed state.
   *
   * Taken by value: callers hand over a map they have just built and it is
   * moved into place.
   */
  virtual void restore_state(StateMap state) = 0;

  /**
   * @brief One page of keys, plus whether the prefix range is exhausted.
   *
   * `reached_end` is not derivable from `keys.size() < limit`: a caller that
   * filters the page (for reserved keys, or for an ACL) may need several pages
   * to fill one response, and a short page does not mean the last page.
   */
  struct KeyPage {
    std::vector<std::string> keys;
    bool reached_end = false;
  };

  /**
   * @brief Up to @p limit keys carrying @p prefix, from @p start inclusive.
   *
   * Lexicographic (byte) order. An empty @p start means "the beginning of the
   * prefix range"; a @p start that precedes the range snaps forward to it.
   *
   * @p start is INCLUSIVE on purpose. An exclusive "after key K" cursor cannot
   * express "resume past this entire range of keys", because a string has no
   * immediate predecessor. An inclusive position expresses both: `K + '\0'` is
   * the first position strictly after `K` (any key greater than `K` either
   * extends it, and so is >= `K + '\0'`, or diverges earlier), and `"__sys;"`
   * is the first position past every `"__sys:"`-prefixed key.
   *
   * Keys are copied out; the caller may hold them freely. Deliberately takes no
   * filter callback: that would run caller logic while the store mutex is held.
   * A caller that needs filtering calls this repeatedly instead.
   */
  [[nodiscard]] virtual KeyPage scan_keys(std::string_view prefix,
                                          std::string_view start,
                                          size_t limit) const = 0;
};

/**
 * @brief Thread-safe, crash-safe persistent key-value store (spec R2.1-R2.7).
 *
 * On-disk state is two files.
 *
 * The **base file**, at @c db_path, is a whole-map state image in the
 * length-prefixed binary format @c KVB1 - see serialize_state() for the
 * layout. It is written through atomic_write_file(), so the file a reader sees
 * is always a complete image, never a half-finished one (R2.2). A file that
 * does not start with the magic is read once with the legacy line parser and
 * immediately rewritten in the new format (R2.3).
 *
 * The **write-ahead log** sits next to it (@c kv.db -> @c kv.wal). Every
 * set()/remove() appends one msgpack-encoded KVCommand and fsyncs it before
 * returning, so a write that was acknowledged survives a @c kill @c -9 (R2.7).
 * The base file is only rewritten when the WAL crosses a threshold from
 * DurabilityOptions, which is what turns an O(store) write into an O(entry) one
 * (R2.6).
 *
 * Recovery is therefore "load the base file, then replay the WAL over it"
 * (R2.5).
 *
 * Phase 3 adds the two ends of a raft snapshot on top of exactly the same
 * machinery: snapshot_state() hands out a consistent copy for someone else to
 * stream, and restore_state() installs one that arrived from a peer (R3.2).
 *
 * **Locking discipline.** Every public method holds @c mutex_ for its whole
 * body. Private helpers suffixed @c _unlocked assume the caller already holds
 * it and must never re-acquire it - that is what keeps compaction, which runs
 * inside set()/remove(), from deadlocking against the caller that triggered it.
 * The constructor calls the same helpers without the lock, which is safe
 * because no other thread can reach the object before construction returns.
 */
class PersistentKVStore : public IKVStore, public IStoreStats {
public:
  /**
   * @brief Open the store at @p db_path and recover its contents.
   *
   * @param db_path Base file path; the WAL path is derived from it.
   * @param options Durability policy (fsync mode, compaction thresholds).
   * @throws std::runtime_error if the base file exists but is corrupt, or if
   *         a recovery rewrite (migration / WAL heal) cannot be written.
   */
  explicit PersistentKVStore(std::string db_path,
                             DurabilityOptions options = {})
      : db_path_(std::move(db_path)), options_(options),
        wal_(wal_path_for(db_path_), options.sync_mode) {
    recover();
  }

  PersistentKVStore(const PersistentKVStore &) = delete;
  PersistentKVStore &operator=(const PersistentKVStore &) = delete;

  /**
   * @brief Derive the WAL path from the base-file path.
   *
   * A trailing @c ".db" is replaced: @c kv.db becomes @c kv.wal. A path
   * without that suffix simply gains @c ".wal".
   */
  [[nodiscard]] static std::string wal_path_for(const std::string &db_path) {
    const std::string kDbSuffix = ".db";
    const std::string kWalSuffix = ".wal";
    if (db_path.size() >= kDbSuffix.size() &&
        db_path.compare(db_path.size() - kDbSuffix.size(), kDbSuffix.size(),
                        kDbSuffix) == 0) {
      return db_path.substr(0, db_path.size() - kDbSuffix.size()) + kWalSuffix;
    }
    return db_path + kWalSuffix;
  }

  /**
   * @brief Store a key-value pair durably.
   *
   * The WAL record is written and fsynced *before* the in-memory map changes,
   * which is the whole point of a write-ahead log: there is no instant at
   * which a caller has been told the write succeeded but the record is not on
   * disk. If the append fails the exception propagates and the map is
   * untouched.
   */
  void set(const std::string &key, const std::string &value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    wal_.append(encode_command(kOpSet, key, value));
    put_unlocked(key, value);
    maybe_compact_unlocked();
  }

  /**
   * @brief Retrieve a value by key.
   * @return The value if found, std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::string>
  get(const std::string &key) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it != store_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  /**
   * @brief Remove a key-value pair durably.
   *
   * Removing a key that is not present touches neither the WAL nor the base
   * file.
   *
   * @return true if the key existed and was removed.
   */
  bool remove(const std::string &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (store_.find(key) == store_.end()) {
      return false;
    }
    wal_.append(encode_command(kOpDelete, key, std::string()));
    erase_unlocked(key);
    maybe_compact_unlocked();
    return true;
  }

  /**
   * @brief Number of keys currently held (R5.4 gauge).
   *
   * Deliberately NOT on IKVStore: it exists for observability, and widening the
   * storage interface for it would force every test fake to implement it too.
   * It IS on IStoreStats, which is that same reasoning taken one step further
   * -- a separate seam for the HTTP handler, which holds interfaces rather than
   * the concrete store. main.cpp holds the concrete type and injects both.
   */
  [[nodiscard]] size_t key_count() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.size();
  }

  /** @brief Current WAL size in bytes (R5.4 gauge). */
  [[nodiscard]] size_t wal_size_bytes() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return wal_.size_bytes();
  }

  /**
   * @brief The index's contents, in order, as owned strings (tests only).
   *
   * Exists so the string_view invariant is observable. Deliberately NOT on
   * IKVStore and deliberately O(n): production code pages through scan_keys.
   */
  [[nodiscard]] std::vector<std::string> ordered_keys_for_test() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    keys.reserve(index_.size());
    for (const std::string_view key : index_) {
      keys.emplace_back(key);
    }
    return keys;
  }

  /**
   * @brief Check if a key exists in the store.
   */
  [[nodiscard]] bool contains(const std::string &key) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.count(key) > 0;
  }

  /**
   * @brief Consistent copy of the whole map (R3.2).
   *
   * Copy under the lock, return, and nothing else - no serialization, no I/O.
   * Serializing and streaming the result happens after the lock is released,
   * so writers are blocked for O(copy) rather than O(network): a snapshot
   * transfer that stalls on a slow peer must not stall this node's write path.
   *
   * The price is that the whole store exists twice in memory while a snapshot
   * is in flight. That is affordable at this scale and is a deliberate trade
   * against holding the lock, but it is a real ceiling - a store approaching a
   * meaningful fraction of RAM would need an incremental or copy-on-write
   * snapshot instead.
   */
  [[nodiscard]] StateMap snapshot_state() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_;
  }

  /**
   * @brief Install @p state as the entire contents of the store (R3.2, R3.7).
   *
   * Replaces, never merges: whatever the node held before is gone, including
   * keys the snapshot does not mention. After this returns, @c kv.db holds
   * exactly @p state and @c kv.wal is empty, so a crash one instruction later
   * recovers to exactly the snapshot (R3.7) - not to the snapshot plus a
   * replay of writes that predate it.
   *
   * **Ordering, and why it is the reverse of compact_unlocked().** Compaction
   * writes a base file that already *contains* every record in the WAL, so
   * crashing before the truncate costs one idempotent re-replay - hence base
   * file first there. A restore has no such relationship: the WAL holds
   * pre-restore commands that the incoming state neither contains nor was
   * built from. Persisting the snapshot first and dying before the truncate
   * would recover as "snapshot, then stale writes applied on top" - a state no
   * replica ever had, reached silently and permanently. So the WAL is dropped
   * first, and the drop is fsynced (reset_wal_unlocked()) so that it cannot
   * still be in flight when the new base file lands.
   *
   * That leaves exactly three states a crash can expose, all of them sane: the
   * pre-restore state, the pre-restore state minus its WAL tail, or the
   * snapshot. Raft re-drives the install in the first two, because a restore
   * that did not return success never advanced the applied index - and every
   * command in a dropped WAL is still in the raft log.
   *
   * Encoding comes first because it is the one step that touches nothing: a
   * state that cannot be encoded fails before the WAL has been dropped. The
   * map is swapped in last, so a failed persist leaves the store serving the
   * state it already had rather than one it has just told raft it could not
   * install.
   *
   * @throws std::runtime_error if the WAL cannot be reset or the base file
   *         cannot be written. Callers must surface it: a node that cannot
   *         install a snapshot must not report that it did (R3.5).
   */
  void restore_state(StateMap state) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string image = serialize_state(state);
    reset_wal_unlocked();
    write_base_unlocked(image);
    replace_all_unlocked(std::move(state));
  }

  /**
   * @brief One page of keys under @p prefix, from @p start inclusive.
   *
   * O(log n + page) against the ordered index, so the cost of a listing is the
   * page rather than the store. See IKVStore::scan_keys for the contract, and
   * in particular for why @p start is inclusive.
   */
  [[nodiscard]] KeyPage scan_keys(std::string_view prefix,
                                  std::string_view start,
                                  size_t limit) const override {
    KeyPage page;
    if (limit == 0) {
      // Asked for nothing, so nothing is known about what follows.
      return page;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // max(prefix, start): an empty or too-early start snaps to the beginning of
    // the prefix range, so the walk below cannot stop on a key that simply
    // precedes it.
    const std::string_view from = (start > prefix) ? start : prefix;
    page.keys.reserve(limit);

    for (auto it = index_.lower_bound(from); it != index_.end(); ++it) {
      const std::string_view key = *it;
      if (!has_prefix_unlocked(key, prefix)) {
        break; // Ordered, so the first miss ends the range.
      }
      if (page.keys.size() == limit) {
        return page; // Full page; reached_end stays false.
      }
      page.keys.emplace_back(key);
    }

    page.reached_end = true;
    return page;
  }

private:
  /** @brief Operation strings understood by KVCommand::parse_operation. */
  static constexpr const char *kOpSet = "SET";
  static constexpr const char *kOpDelete = "DELETE";

  // Declaration order matters: wal_'s initialiser reads db_path_.
  std::string db_path_;
  DurabilityOptions options_;
  Wal wal_;
  StateMap store_;

  /**
   * @brief Lexicographically ordered view of every key in @c store_.
   *
   * Holds std::string_view into the KEY STRINGS OWNED BY store_'s own nodes,
   * so no key bytes are copied: an unordered_map node keeps its address across
   * a rehash, which is what makes the views stable. Cost is one RB-tree node
   * per key (48-64 bytes, independent of key length).
   *
   * INVARIANT, and it is a use-after-free if broken: every view in here points
   * at a live node of store_. Exactly three helpers may mutate store_ --
   * put_unlocked, erase_unlocked and replace_all_unlocked -- and every one of
   * them maintains this set. Nothing else in this file may touch store_
   * directly.
   *
   * Declared AFTER store_ so it is destroyed FIRST: a set of views must not
   * outlive the strings it points at, even during teardown.
   *
   * Byte-transparent, like the store: std::less<std::string_view> compares via
   * char_traits::compare, so keys containing '=', newlines or NUL bytes order
   * exactly.
   */
  std::set<std::string_view> index_;

  mutable std::mutex mutex_;

  /**
   * @brief Insert or update one entry, keeping @c index_ in step.
   *
   * The view is taken from the MAP NODE'S key (@c result.first->first), never
   * from @p key. A view of the caller's argument dangles the moment that
   * string dies -- and would appear to work, because the bytes are usually
   * still there.
   *
   * Caller must hold @c mutex_.
   */
  void put_unlocked(const std::string &key, const std::string &value) {
    const auto result = store_.insert_or_assign(key, value);
    if (result.second) {
      // A new node was created. An update reuses the existing node, whose key
      // is already registered, so re-inserting would be wasted work.
      index_.insert(std::string_view(result.first->first));
    }
  }

  /**
   * @brief Erase one entry, keeping @c index_ in step.
   * @return true if the key existed.
   *
   * The view is removed BEFORE the node dies. The reverse order leaves a
   * dangling view in the set for as long as it takes to erase it, and the
   * erase itself compares against it.
   *
   * Caller must hold @c mutex_.
   */
  bool erase_unlocked(const std::string &key) {
    const auto it = store_.find(key);
    if (it == store_.end()) {
      return false;
    }
    index_.erase(std::string_view(it->first));
    store_.erase(it);
    return true;
  }

  /**
   * @brief Replace the whole map, rebuilding @c index_ from scratch.
   *
   * Every pre-existing view is dead and every new node is unregistered, so
   * this is a clear-and-rebuild rather than a diff. O(n log n), and it only
   * runs on a snapshot install or a start-up load.
   *
   * Caller must hold @c mutex_ (the constructor's calls are the documented
   * exception: nothing can observe the object yet).
   */
  /**
   * @brief True when @p key begins with @p prefix.
   *
   * C++17 has no std::string_view::starts_with. Pure, so despite the suffix it
   * needs no lock at all -- named for consistency with the helpers around it.
   */
  [[nodiscard]] static bool has_prefix_unlocked(std::string_view key,
                                                std::string_view prefix) {
    return key.size() >= prefix.size() &&
           key.compare(0, prefix.size(), prefix) == 0;
  }

  void replace_all_unlocked(StateMap state) {
    index_.clear();
    store_ = std::move(state);
    for (const auto &entry : store_) {
      index_.insert(std::string_view(entry.first));
    }
  }

  /**
   * @brief Rebuild in-memory state from disk (R2.3 / R2.5).
   *
   * Base file first, then the WAL replayed over it, then - if either step left
   * the files in a shape we do not want to see again - one compaction to
   * normalise them.
   */
  void recover() {
    const bool migrated = load_base_unlocked();
    const bool replay_stopped_early = replay_wal_unlocked();

    if (migrated || replay_stopped_early) {
      // Fold everything we could recover into a fresh base file and drop the
      // WAL. This is what turns a legacy file into a KVB1 one (R2.3) and what
      // stops a WAL tail we refused to apply from being retried on every
      // subsequent start.
      compact_unlocked();
      return;
    }

    // Nothing wrong, but the WAL may already have been over threshold when the
    // process died before it could compact.
    maybe_compact_unlocked();
  }

  /**
   * @brief Read the base file into @c store_.
   * @return true if the file was in the legacy line format and therefore has
   *         to be rewritten (R2.3).
   */
  bool load_base_unlocked() {
    const std::optional<std::string> contents = read_file(db_path_);
    if (!contents.has_value()) {
      return false; // No file yet: empty store, and nothing to rewrite.
    }

    if (has_state_magic(*contents)) {
      // A file that claims to be KVB1 is parsed as KVB1, full stop. Falling
      // back to the legacy parser when a binary file fails to parse would turn
      // detectable corruption into plausible-looking garbage, so a malformed
      // KVB1 file is a hard error instead. The converse ambiguity - a legacy
      // file whose first line happens to begin with "KVB1" - resolves the same
      // way: it fails loudly rather than loading wrong data.
      try {
        replace_all_unlocked(deserialize_state(*contents));
      } catch (const std::runtime_error &error) {
        // The decoder is shared with the snapshot path and knows nothing about
        // where its bytes came from, so the path is attached here - an
        // operator staring at a start-up failure needs to be told which file.
        throw corrupt_base(error.what());
      }
      return false;
    }

    parse_legacy_unlocked(*contents);
    return true;
  }

  /**
   * @brief Parse the pre-Phase-2 "key=value\n" file (R2.3, migration only).
   *
   * Deliberately bug-compatible with the reader it replaces: split at the
   * FIRST '=', drop any line that has none. Files on disk were written by that
   * reader's matching writer, so "fixing" the rule here would change what
   * existing data decodes to - the job is to read what is there, not to
   * improve it. The caller rewrites the result in KVB1 immediately, so any
   * given file takes this path at most once.
   *
   * MIGRATION ONLY: this function, the non-magic branch of
   * load_base_unlocked() and their tests can be deleted one release after
   * Phase 2 ships.
   */
  void parse_legacy_unlocked(const std::string &contents) {
    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
      const size_t eq_pos = line.find('=');
      if (eq_pos == std::string::npos) {
        continue;
      }
      put_unlocked(line.substr(0, eq_pos), line.substr(eq_pos + 1));
    }
  }

  /**
   * @brief Replay the WAL over the loaded base state (R2.5).
   *
   * Wal::replay() already heals a physically torn tail (short read or CRC
   * mismatch). This handles the other failure mode: a record whose framing and
   * CRC are perfect but whose payload is not a decodable, valid KVCommand.
   *
   * That is treated exactly like a torn record - stop, and apply nothing after
   * it. The alternative, skipping the bad record and carrying on, is worse: a
   * record we cannot decode may have been a DELETE, so applying later records
   * on top of a state that is missing it would reconstruct a state this
   * replica never had, silently and permanently. Stopping keeps recovery
   * prefix-consistent, which is the same guarantee the torn-tail heal gives.
   *
   * @return true if replay stopped early, i.e. the WAL still holds records we
   *         refused and must be rewritten.
   */
  bool replay_wal_unlocked() {
    bool stopped = false;
    size_t index = 0;

    wal_.replay([this, &stopped, &index](const std::string &payload) {
      if (stopped) {
        return; // Everything after the first bad record is discarded.
      }
      if (!apply_payload_unlocked(payload)) {
        stopped = true;
        log::warn(log::kComponentStore,
                  "WAL record is not a valid command; stopping replay there "
                  "and dropping the rest",
                  {log::field("record_index", index),
                   log::field("wal_path", wal_path_for(db_path_))});
        return;
      }
      ++index;
    });

    return stopped;
  }

  /**
   * @brief Apply one WAL payload - a msgpack KVCommand - to @c store_.
   * @return false if the payload could not be decoded or is not a valid
   *         SET/DELETE, in which case @c store_ was not modified.
   */
  bool apply_payload_unlocked(const std::string &payload) {
    KVCommand cmd;
    try {
      cmd = KVCommand::from_msgpack(payload.data(), payload.size());
    } catch (const std::exception &) {
      return false;
    }

    if (!cmd.is_valid()) {
      return false;
    }

    switch (cmd.operation_type()) {
    case Operation::SET:
      put_unlocked(cmd.key, cmd.value);
      return true;
    case Operation::DELETE:
      // The bool is discarded on purpose: replay does not care whether the key
      // was present, and erase_unlocked is deliberately not [[nodiscard]].
      erase_unlocked(cmd.key);
      return true;
    case Operation::USER_SET:
    case Operation::USER_DEL:
      // NOT a record this store ever writes. set()/remove() re-encode every
      // mutation as SET or DELETE — a committed user write reaches the WAL as a
      // plain SET of its `__sys:user:...` key — so a USER_* record in a WAL
      // means the file was not produced by this store. Treated exactly like a
      // record that will not decode: stop replay here rather than guess, which
      // keeps recovery prefix-consistent.
      return false;
    case Operation::UNKNOWN:
      break;
    }
    // Unreachable for UNKNOWN: is_valid() already rejected it. Kept so the
    // function cannot fall off the end if the two ever drift apart.
    return false;
  }

  /**
   * @brief Encode a mutation as the msgpack KVCommand the WAL stores.
   *
   * set()/remove() are handed a key and a value, not the bytes the client
   * sent, so the WAL payload is an equivalent *re-encoding* rather than a
   * byte-for-byte copy of the original request. It decodes through
   * KVCommand::from_msgpack to the same command, which is all replay needs.
   */
  [[nodiscard]] static std::string encode_command(const std::string &op,
                                                  const std::string &key,
                                                  const std::string &value) {
    KVCommand cmd;
    cmd.op = op;
    cmd.key = key;
    cmd.value = value;
    // KVCommand::to_msgpack is the single encoder — see the note there. The
    // HTTP handler builds raft payloads with the same one, which is what keeps
    // a WAL record and a raft entry byte-identical for the same command.
    return cmd.to_msgpack();
  }

  /** @brief Atomically install an encoded state image as the base (R2.2). */
  void write_base_unlocked(const std::string &image) const {
    atomic_write_file(db_path_, image);
  }

  /** @brief Write the current map as a complete base file, atomically. */
  void persist_unlocked() const {
    write_base_unlocked(serialize_state(store_));
  }

  /**
   * @brief Drop every WAL record and force the truncation to stable storage.
   *
   * Wal::truncate() deliberately does not fsync, and for its two original
   * callers that is right: a truncation lost to a crash costs nothing but a
   * repeat of the torn-tail heal, or a re-replay of records the base file
   * already contains. restore_state() is the caller for which it is not right
   * - there, a WAL that survives the crash is replayed *over* an installed
   * snapshot, which is corruption rather than a repeat. Hence the explicit
   * fsync: the truncation must be on disk before the snapshot's base file can
   * land, or the ordering argument in restore_state() buys nothing.
   *
   * @throws std::runtime_error if the WAL cannot be truncated or fsynced.
   */
  void reset_wal_unlocked() {
    wal_.truncate();

    const std::string &wal_path = wal_.path();
    fileio::FdGuard fd(::open(wal_path.c_str(), O_WRONLY | O_CLOEXEC));
    if (!fd.valid()) {
      const int err = errno;
      if (err == ENOENT) {
        return; // Never created, so there is no truncation to make durable.
      }
      fileio::throw_errno("cannot open WAL to fsync", wal_path, err);
    }
    if (!fileio::fsync_retry(fd.get())) {
      const int err = errno;
      fileio::throw_errno("cannot fsync WAL", wal_path, err);
    }
    if (fd.close_checked() != 0) {
      const int err = errno;
      fileio::throw_errno("cannot close WAL", wal_path, err);
    }
  }

  /**
   * @brief Fold the WAL into a fresh base file and drop it (R2.6).
   *
   * Order is the correctness argument: the base file is durable (fsynced and
   * renamed into place) before the records it subsumes are thrown away, so a
   * crash between the two costs at most a replay of records that are already
   * in the base file - and SET/DELETE replay is idempotent. restore_state()
   * needs the opposite order for the opposite reason; see the note there.
   */
  void compact_unlocked() {
    persist_unlocked();
    wal_.truncate();
  }

  /**
   * @brief Compact if the WAL has reached either threshold (R2.6).
   *
   * Synchronous and inside the caller's lock: apply throughput is bounded by
   * raft, not by this rewrite, so the simple thing is the right thing here.
   */
  void maybe_compact_unlocked() {
    if (wal_.size_bytes() < options_.wal_max_bytes &&
        wal_.record_count() < options_.wal_max_records) {
      return;
    }
    compact_unlocked();
  }

  /** @brief Build the exception thrown for an unreadable base file. */
  [[nodiscard]] std::runtime_error
  corrupt_base(const std::string &reason) const {
    return std::runtime_error("corrupt base file \"" + db_path_ +
                              "\": " + reason);
  }
};

} // namespace kvdb
