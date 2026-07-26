#pragma once

#include <cstddef>
#include <string>

// WalSyncMode is defined by the component it configures (storage/wal.hpp) so
// that there is exactly one definition of it in the program; this header only
// references it. That is also why config/ depends on storage/ and not the other
// way round - the WAL must stay usable without dragging in application config.
#include "../storage/wal.hpp"

namespace kvdb {

/**
 * @brief Durability knobs for PersistentKVStore (spec R2.8).
 *
 * Configuration, not magic numbers: the WAL fsync policy and the two
 * compaction thresholds live here with named defaults instead of being buried
 * in the store. They are deliberately NOT exposed as positional CLI arguments -
 * the argv contract (http_port, grpc_port, sidecar_port, db_file) is unchanged.
 */
struct DurabilityOptions {
  /**
   * @brief fsync policy applied to every WAL append.
   *
   * kAlways is the only setting that makes an acknowledged write survive a
   * power loss. kNever exists for tests and benchmarks.
   */
  WalSyncMode sync_mode = WalSyncMode::kAlways;

  /** @brief Compact once the WAL has reached this many bytes. */
  size_t wal_max_bytes = 4u * 1024 * 1024;

  /** @brief Compact once the WAL has reached this many records. */
  size_t wal_max_records = 10000;
};

/**
 * @brief Application configuration container.
 *
 * Immutable configuration object that holds all runtime settings.
 * Follows the Value Object pattern - created once and passed by const
 * reference.
 */
struct Config {
  std::string db_file = "kv.db";
  std::string grpc_port = "50051";
  std::string sidecar_port = "50052";
  int http_port = 8080;

  /** @brief Durability policy handed to the store (spec R2.8). */
  DurabilityOptions durability{};

  /**
   * @brief Create config with default values.
   *
   * The values themselves are default member initialisers above rather than a
   * designated-initialiser list here: designated initialisers are C++20, and
   * this is a C++17 build with -Wpedantic. That was one warning in one
   * translation unit while only main.cpp included this header; Phase 2 pulls it
   * into storage/kv_store.hpp (for DurabilityOptions) and therefore into every
   * test binary, so the warning is removed rather than multiplied. Same values,
   * same behaviour.
   */
  static Config defaults() { return Config{}; }

  /**
   * @brief Parse configuration from command line arguments.
   *
   * @param argc Argument count from main
   * @param argv Argument values from main
   * @return Config Parsed configuration with CLI overrides
   */
  static Config from_args(int argc, char *argv[]) {
    Config cfg = defaults();

    if (argc > 1)
      cfg.http_port = std::stoi(argv[1]);
    if (argc > 2)
      cfg.grpc_port = argv[2];
    if (argc > 3)
      cfg.sidecar_port = argv[3];
    if (argc > 4)
      cfg.db_file = argv[4];

    return cfg;
  }

  /**
   * @brief Get the full gRPC server address.
   */
  [[nodiscard]] std::string grpc_address() const {
    return "0.0.0.0:" + grpc_port;
  }

  /**
   * @brief Get the sidecar channel address.
   */
  [[nodiscard]] std::string sidecar_address() const {
    return "localhost:" + sidecar_port;
  }
};

} // namespace kvdb
