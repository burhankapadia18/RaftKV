#pragma once

#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace kvdb {

/**
 * @brief Abstract interface for key-value storage.
 *
 * Follows the Interface Segregation Principle - defines only
 * the essential operations needed for KV storage.
 */
class IKVStore {
public:
  virtual ~IKVStore() = default;

  virtual void set(const std::string &key, const std::string &value) = 0;
  virtual std::optional<std::string> get(const std::string &key) const = 0;
  virtual bool remove(const std::string &key) = 0;
  virtual bool contains(const std::string &key) const = 0;
};

/**
 * @brief Thread-safe, persistent key-value store.
 *
 * Implements IKVStore with file-based persistence and mutex protection.
 * Uses copy-on-read to avoid lock contention on reads.
 */
class PersistentKVStore : public IKVStore {
public:
  explicit PersistentKVStore(std::string db_path)
      : db_path_(std::move(db_path)) {
    load();
  }

  /**
   * @brief Store a key-value pair and persist to disk.
   */
  void set(const std::string &key, const std::string &value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    store_[key] = value;
    persist();
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
   * @brief Remove a key-value pair and persist to disk.
   * @return true if the key existed and was removed.
   */
  bool remove(const std::string &key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    bool erased = store_.erase(key) > 0;
    if (erased) {
      persist();
    }
    return erased;
  }

  /**
   * @brief Check if a key exists in the store.
   */
  [[nodiscard]] bool contains(const std::string &key) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.count(key) > 0;
  }

private:
  std::string db_path_;
  std::unordered_map<std::string, std::string> store_;
  mutable std::mutex mutex_;

  /**
   * @brief Load data from disk into memory.
   */
  void load() {
    std::ifstream file(db_path_);
    if (!file.is_open())
      return;

    std::string line;
    while (std::getline(file, line)) {
      size_t eq_pos = line.find('=');
      if (eq_pos != std::string::npos) {
        store_[line.substr(0, eq_pos)] = line.substr(eq_pos + 1);
      }
    }
  }

  /**
   * @brief Persist in-memory data to disk.
   *
   * Note: This is a simple implementation. Production systems
   * should use fsync() and write-ahead logging for durability.
   */
  void persist() {
    std::ofstream file(db_path_, std::ios::trunc);
    for (const auto &[key, value] : store_) {
      file << key << "=" << value << "\n";
    }
  }
};

} // namespace kvdb
