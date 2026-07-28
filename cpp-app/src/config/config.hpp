#pragma once

#include <cstddef>
#include <cstdlib>
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
/**
 * @brief Bounds on an inbound HTTP request (R4.9).
 *
 * Before Phase 4 there were none: the server read one 4 KiB chunk and hoped the
 * headers fitted, and a body could be as large as the client felt like sending.
 * Both are now explicit, because "how much memory can a stranger make this
 * process allocate" should be a number someone chose.
 */
struct RequestLimits {
  /** @brief Largest accepted request body. Over this is a 413. */
  size_t max_body_bytes = 1u * 1024 * 1024;

  /**
   * @brief Largest accepted header block, terminator included. Over this is a
   * 431.
   *
   * A cap is what makes the read-until-blank-line loop safe: without it a
   * client that never sends the terminator makes the server buffer forever.
   */
  size_t max_header_bytes = 32u * 1024;
};

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
  /** @brief NODE_ID from the environment, or "unknown". */
  [[nodiscard]] static std::string default_node_id() {
    const char *value = std::getenv("NODE_ID");
    return (value != nullptr && *value != '\0') ? std::string(value)
                                                : std::string("unknown");
  }

  /** @brief LOG_LEVEL from the environment, or "info". */
  [[nodiscard]] static std::string default_log_level() {
    const char *value = std::getenv("LOG_LEVEL");
    return (value != nullptr && *value != '\0') ? std::string(value)
                                                : std::string("info");
  }

  std::string db_file = "kv.db";
  std::string grpc_port = "50051";
  std::string sidecar_port = "50052";
  int http_port = 8080;

  /** @brief Durability policy handed to the store (spec R2.8). */
  DurabilityOptions durability{};

  /**
   * @brief Node identity, stamped on every log line (R5.2).
   *
   * Defaults to the NODE_ID environment variable so entrypoint.sh does not need
   * another positional argument; the compose file already sets it for the
   * sidecar.
   */
  std::string node_id = default_node_id();

  /** @brief Log threshold: debug|info|warn|error. */
  std::string log_level = default_log_level();

  /** @brief Inbound HTTP request bounds (R4.9). */
  RequestLimits limits{};

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
