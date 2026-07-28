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

  /**
   * @brief Structural limits applied while decoding (R6.7).
   *
   * WITHOUT THESE, A 17-BYTE REQUEST BODY IS A CLUSTER-WIDE DENIAL OF SERVICE.
   * Found by the libFuzzer target in cpp-app/fuzz: msgpack's map32 marker
   * (0xdf) declares an entry count in its next four bytes, so a short chain of
   * them declares billions of entries and msgpack-c allocates for them before
   * discovering the bytes are not there. Reproduced outside the fuzzer — 17
   * bytes asking for more than 512 MiB.
   *
   * It is worse than a single-node crash. This decode happens at APPLY time, on
   * every replica, on an entry that Raft has ALREADY COMMITTED — and committed
   * entries replay on restart. So one such write takes down all three nodes and
   * keeps taking them down every time they come back up.
   *
   * The limits are the actual shape of the payload, not a guess: the wire
   * format is a flat map of exactly three string fields (MSGPACK_DEFINE_MAP
   * below), so anything nested, longer, or wider is not a command this store
   * has ever produced. Depth 4 leaves headroom over the flat map's depth of 1
   * without permitting a recursion bomb.
   */
  static constexpr size_t kMaxMapEntries = 64;
  static constexpr size_t kMaxArrayEntries = 64;
  static constexpr size_t kMaxStringBytes = 8u * 1024 * 1024;
  static constexpr size_t kMaxDepth = 4;

  static KVCommand from_msgpack(const char *data, size_t size) {
    KVCommand cmd;

    // The limit is checked as the parser walks the input, so an over-large
    // declared length is refused BEFORE anything is allocated for it. That
    // ordering is the whole fix; validating after the allocation would be too
    // late by definition.
    const msgpack::unpack_limit limit(kMaxArrayEntries, kMaxMapEntries,
                                      kMaxStringBytes, kMaxStringBytes,
                                      kMaxStringBytes, kMaxDepth);

    size_t offset = 0;
    msgpack::object_handle oh = msgpack::unpack(
        data, size, offset, MSGPACK_NULLPTR, MSGPACK_NULLPTR, limit);
    msgpack::object obj = oh.get();
    obj.convert(cmd);
    return cmd;
  }
};

} // namespace kvdb
