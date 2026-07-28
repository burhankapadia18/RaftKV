#pragma once

#include <msgpack.hpp>
#include <optional>
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
   * @brief Explain why this command is invalid.
   *
   * The validation rules and the human-readable reason for each one live
   * together here so that StateMachineService::Apply can put the reason on
   * the wire (ApplyResponse.error) without restating them.
   *
   * @return std::nullopt when the command is valid, otherwise a short
   *         description naming the offending field.
   */
  [[nodiscard]] std::optional<std::string> validation_error() const {
    if (operation_type() == Operation::UNKNOWN) {
      return "unknown operation: \"" + op + "\"";
    }
    if (key.empty()) {
      return "empty key for operation \"" + op + "\"";
    }
    return std::nullopt;
  }

  /**
   * @brief Check if this is a valid command.
   */
  [[nodiscard]] bool is_valid() const {
    return !validation_error().has_value();
  }

  /**
   * @brief Deserialize a KVCommand from MsgPack binary data.
   *
   * @param data Raw MsgPack bytes
   * @param size Size of the data buffer
   * @return KVCommand The deserialized command
   * @throws std::runtime_error If deserialization fails
   */
  /**
   * @brief Serialize this command to the MsgPack map form.
   *
   * The inverse of from_msgpack, and the ONE encoder in the codebase: the HTTP
   * handler uses it to build a raft payload (R4.7) and PersistentKVStore uses
   * it to build a WAL record. Two encoders would be free to drift, and this
   * format is both a wire format and an on-disk format — a divergence would
   * make old WAL records or old raft entries undecodable.
   */
  [[nodiscard]] std::string to_msgpack() const {
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, *this);
    return std::string(buffer.data(), buffer.size());
  }

  /** @brief Build an encoded SET payload. */
  [[nodiscard]] static std::string encode_set(const std::string &key,
                                              const std::string &value) {
    KVCommand cmd;
    cmd.op = "SET";
    cmd.key = key;
    cmd.value = value;
    return cmd.to_msgpack();
  }

  /** @brief Build an encoded DELETE payload. */
  [[nodiscard]] static std::string encode_delete(const std::string &key) {
    KVCommand cmd;
    cmd.op = "DELETE";
    cmd.key = key;
    return cmd.to_msgpack();
  }

  static KVCommand from_msgpack(const char *data, size_t size) {
    KVCommand cmd;
    msgpack::object_handle oh = msgpack::unpack(data, size);
    msgpack::object obj = oh.get();
    obj.convert(cmd);
    return cmd;
  }
};

} // namespace kvdb
