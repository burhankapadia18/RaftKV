#pragma once

#include <string>

namespace kvdb {

/**
 * @brief Application configuration container.
 *
 * Immutable configuration object that holds all runtime settings.
 * Follows the Value Object pattern - created once and passed by const
 * reference.
 */
struct Config {
  std::string db_file;
  std::string grpc_port;
  std::string sidecar_port;
  int http_port;

  /**
   * @brief Create config with default values.
   */
  static Config defaults() {
    return Config{.db_file = "kv.db",
                  .grpc_port = "50051",
                  .sidecar_port = "50052",
                  .http_port = 8080};
  }

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
