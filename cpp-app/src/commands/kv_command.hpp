#pragma once

#include <msgpack.hpp>
#include <string>

namespace kvdb {

/**
 * @brief Operation types supported by the KV store.
 */
enum class Operation { SET, DELETE, UNKNOWN };

/**
 * @brief Parse operation string to enum.
 */
inline Operation parse_operation(const std::string &op) {
  if (op == "SET")
    return Operation::SET;
  if (op == "DELETE")
    return Operation::DELETE;
  return Operation::UNKNOWN;
}

/**
 * @brief Command structure for KV operations.
 *
 * This structure is serialized/deserialized using MsgPack
 * for efficient binary transmission over Raft consensus.
 *
 * Maps to the format: {'op': '...', 'key': '...', 'value': '...'}
 */
struct KVCommand {
  std::string op;
  std::string key;
  std::string value;

  // MsgPack serialization macro
  MSGPACK_DEFINE_MAP(op, key, value);

  /**
   * @brief Get the operation type as an enum.
   */
  [[nodiscard]] Operation operation_type() const { return parse_operation(op); }

  /**
   * @brief Check if this is a valid command.
   */
  [[nodiscard]] bool is_valid() const {
    return operation_type() != Operation::UNKNOWN && !key.empty();
  }

  /**
   * @brief Deserialize a KVCommand from MsgPack binary data.
   *
   * @param data Raw MsgPack bytes
   * @param size Size of the data buffer
   * @return KVCommand The deserialized command
   * @throws std::runtime_error If deserialization fails
   */
  static KVCommand from_msgpack(const char *data, size_t size) {
    KVCommand cmd;
    msgpack::object_handle oh = msgpack::unpack(data, size);
    msgpack::object obj = oh.get();
    obj.convert(cmd);
    return cmd;
  }
};

} // namespace kvdb
