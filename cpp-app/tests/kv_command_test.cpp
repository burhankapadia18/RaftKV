/**
 * @file kv_command_test.cpp
 * @brief Unit tests for KVCommand (spec R0.10).
 *
 * KVCommand::from_msgpack is the trust boundary for every byte a client sends:
 * the HTTP layer forwards the body unparsed, so this is the first and only
 * place the payload is validated. These tests pin the CURRENT behavior,
 * including the places where it is permissive; Phase 1 (truthful errors) is
 * what changes the reporting, not this phase.
 */

#include <gtest/gtest.h>

#include <exception>
#include <map>
#include <string>
#include <vector>

#include <msgpack.hpp>

#include "commands/kv_command.hpp"

namespace kvdb {
namespace {

/**
 * @brief Pack a MsgPack map of string->string.
 *
 * This mirrors exactly what test_client.py puts on the wire (a MsgPack map,
 * not the array variant), which is what MSGPACK_DEFINE_MAP expects.
 */
std::string pack_string_map(const std::map<std::string, std::string> &fields) {
  msgpack::sbuffer buffer;
  msgpack::pack(buffer, fields);
  return std::string(buffer.data(), buffer.size());
}

KVCommand decode(const std::string &bytes) {
  return KVCommand::from_msgpack(bytes.data(), bytes.size());
}

// --- Valid payloads -------------------------------------------------------

TEST(KVCommandTest, DecodesValidSetCommand) {
  const std::string bytes =
      pack_string_map({{"op", "SET"}, {"key", "user:1"}, {"value", "alice"}});

  const KVCommand cmd = decode(bytes);

  EXPECT_EQ(cmd.op, "SET");
  EXPECT_EQ(cmd.key, "user:1");
  EXPECT_EQ(cmd.value, "alice");
  EXPECT_EQ(cmd.operation_type(), Operation::SET);
  EXPECT_TRUE(cmd.is_valid());
}

TEST(KVCommandTest, DecodesValidDeleteCommand) {
  const std::string bytes =
      pack_string_map({{"op", "DELETE"}, {"key", "user:1"}, {"value", ""}});

  const KVCommand cmd = decode(bytes);

  EXPECT_EQ(cmd.op, "DELETE");
  EXPECT_EQ(cmd.key, "user:1");
  EXPECT_EQ(cmd.value, "");
  EXPECT_EQ(cmd.operation_type(), Operation::DELETE);
  // An empty value is fine for DELETE - is_valid() only looks at op and key.
  EXPECT_TRUE(cmd.is_valid());
}

TEST(KVCommandTest, DecodesBinarySafeValues) {
  // MsgPack strings are length-prefixed, so embedded NULs and newlines survive
  // the decode intact. Storage is where they diverge: the NUL survives
  // PersistentKVStore too, the newline does not - see
  // ValueContainingNulSurvivesReload and
  // ValueContainingNewlineIsTruncatedByReload in kv_store_test.cpp.
  const std::string value("a\0b\nc", 5);
  const std::string bytes =
      pack_string_map({{"op", "SET"}, {"key", "k"}, {"value", value}});

  const KVCommand cmd = decode(bytes);

  EXPECT_EQ(cmd.value, value);
  EXPECT_EQ(cmd.value.size(), 5u);
}

// --- Malformed payloads ---------------------------------------------------

TEST(KVCommandTest, ThrowsOnEmptyBuffer) {
  const std::string empty;
  // msgpack::unpack cannot complete an object -> msgpack::insufficient_bytes,
  // which derives from std::runtime_error. Assert on std::exception so the
  // test does not depend on the concrete msgpack-c exception hierarchy.
  EXPECT_THROW(KVCommand::from_msgpack(empty.data(), 0), std::exception);
}

TEST(KVCommandTest, ThrowsOnTruncatedBytes) {
  std::string bytes =
      pack_string_map({{"op", "SET"}, {"key", "user:1"}, {"value", "alice"}});
  ASSERT_GT(bytes.size(), 4u);
  bytes.resize(bytes.size() - 3); // chop the tail off a complete object

  EXPECT_THROW(decode(bytes), std::exception);
}

TEST(KVCommandTest, ThrowsOnGarbageBytes) {
  // 0xc1 is the one byte the MsgPack spec marks "never used".
  const char never_used[] = {static_cast<char>(0xc1)};
  EXPECT_THROW(KVCommand::from_msgpack(never_used, sizeof(never_used)),
               std::exception);
}

TEST(KVCommandTest, ThrowsWhenPayloadIsNotAMap) {
  msgpack::sbuffer buffer;
  msgpack::pack(buffer, std::string("not-a-map"));

  // MSGPACK_DEFINE_MAP rejects anything that is not a MAP object
  // (msgpack::type_error, which derives from std::bad_cast).
  EXPECT_THROW(KVCommand::from_msgpack(buffer.data(), buffer.size()),
               std::exception);
}

TEST(KVCommandTest, ThrowsOnArrayWireFormat) {
  // The struct uses MSGPACK_DEFINE_MAP, not the array variant. A client that
  // sends ["SET","k","v"] is rejected - pinned because switching the macro
  // would silently change the accepted wire format for every stored raft entry.
  msgpack::sbuffer buffer;
  const std::vector<std::string> as_array{"SET", "k", "v"};
  msgpack::pack(buffer, as_array);

  EXPECT_THROW(KVCommand::from_msgpack(buffer.data(), buffer.size()),
               std::exception);
}

TEST(KVCommandTest, ThrowsWhenAFieldHasTheWrongType) {
  msgpack::sbuffer buffer;
  msgpack::packer<msgpack::sbuffer> pk(&buffer);
  pk.pack_map(3);
  pk.pack(std::string("op"));
  pk.pack(42); // integer where a string is expected
  pk.pack(std::string("key"));
  pk.pack(std::string("k"));
  pk.pack(std::string("value"));
  pk.pack(std::string("v"));

  // Type mismatch on a present key throws (unlike a *missing* key - see below).
  EXPECT_THROW(KVCommand::from_msgpack(buffer.data(), buffer.size()),
               std::exception);
}

// --- Permissive decoding (pinned current behavior) ------------------------

TEST(KVCommandTest, UnknownOpDecodesButIsInvalid) {
  const std::string bytes =
      pack_string_map({{"op", "PATCH"}, {"key", "k"}, {"value", "v"}});

  const KVCommand cmd = decode(bytes);

  // Decoding succeeds; only is_valid() flags the problem. CURRENT BEHAVIOR:
  // StateMachine::Apply never calls is_valid() (it is dead code - the only
  // references are its definition and this test file). An unknown op is still
  // caught, but by the switch's UNKNOWN arm in state_machine.hpp: stderr log
  // plus success=false, with gRPC status OK and no reason on the wire.
  //
  // The case the missing is_valid() call actually loses is the EMPTY KEY:
  // {op:"SET", key:""} is invalid, yet Apply runs store_.set("", value) and
  // reports success=true. Nothing pins that today - it needs a KVHttpHandler
  // or StateMachine test, which Phase 1 (R1.2/R1.3) adds along with routing
  // both cases through validation. This test pins only the decode contract.
  EXPECT_EQ(cmd.op, "PATCH");
  EXPECT_EQ(cmd.operation_type(), Operation::UNKNOWN);
  EXPECT_FALSE(cmd.is_valid());
}

TEST(KVCommandTest, MissingValueFieldIsSilentlyDefaulted) {
  // CURRENT BEHAVIOR: msgpack-c's MSGPACK_DEFINE_MAP unpack looks each member
  // name up in the map and skips it when absent, leaving the member at its
  // default. A SET with no "value" therefore decodes as a SET of the empty
  // string rather than being rejected.
  const std::string bytes = pack_string_map({{"op", "SET"}, {"key", "k"}});

  const KVCommand cmd = decode(bytes);

  EXPECT_EQ(cmd.op, "SET");
  EXPECT_EQ(cmd.key, "k");
  EXPECT_EQ(cmd.value, "");
  EXPECT_TRUE(cmd.is_valid());
}

TEST(KVCommandTest, MissingKeyFieldIsSilentlyDefaultedAndInvalid) {
  const std::string bytes = pack_string_map({{"op", "SET"}, {"value", "v"}});

  const KVCommand cmd = decode(bytes);

  EXPECT_EQ(cmd.key, "");
  EXPECT_FALSE(cmd.is_valid()); // empty key -> invalid
}

TEST(KVCommandTest, EmptyMapDecodesToAnAllDefaultCommand) {
  const std::string bytes = pack_string_map({});

  const KVCommand cmd = decode(bytes);

  EXPECT_EQ(cmd.op, "");
  EXPECT_EQ(cmd.key, "");
  EXPECT_EQ(cmd.value, "");
  EXPECT_EQ(cmd.operation_type(), Operation::UNKNOWN);
  EXPECT_FALSE(cmd.is_valid());
}

TEST(KVCommandTest, UnknownExtraFieldsAreIgnored) {
  const std::string bytes = pack_string_map(
      {{"op", "SET"}, {"key", "k"}, {"value", "v"}, {"ttl", "60"}});

  const KVCommand cmd = decode(bytes);

  EXPECT_EQ(cmd.op, "SET");
  EXPECT_EQ(cmd.key, "k");
  EXPECT_EQ(cmd.value, "v");
  EXPECT_TRUE(cmd.is_valid());
}

// --- Truth tables ---------------------------------------------------------

TEST(KVCommandTest, ParseOperationTruthTable) {
  struct Case {
    const char *op;
    Operation expected;
  };

  const Case cases[] = {
      {"SET", Operation::SET},
      {"DELETE", Operation::DELETE},
      // Matching is exact and case sensitive.
      {"set", Operation::UNKNOWN},
      {"Set", Operation::UNKNOWN},
      {"delete", Operation::UNKNOWN},
      {" SET", Operation::UNKNOWN},
      {"SET ", Operation::UNKNOWN},
      {"GET", Operation::UNKNOWN},
      {"", Operation::UNKNOWN},
  };

  for (const Case &c : cases) {
    EXPECT_EQ(parse_operation(c.op), c.expected) << "op=[" << c.op << "]";
  }
}

TEST(KVCommandTest, IsValidTruthTable) {
  struct Case {
    const char *op;
    const char *key;
    const char *value;
    bool expected;
  };

  const Case cases[] = {
      {"SET", "k", "v", true},
      {"SET", "k", "", true},     // empty value is allowed for SET
      {"DELETE", "k", "", true},  // DELETE ignores value
      {"DELETE", "k", "v", true}, // ...even when one is supplied
      {"SET", "", "v", false},    // empty key
      {"DELETE", "", "", false},  // empty key
      {"PATCH", "k", "v", false}, // unknown op
      {"", "k", "v", false},      // unknown op
      {"", "", "", false},
  };

  for (const Case &c : cases) {
    KVCommand cmd;
    cmd.op = c.op;
    cmd.key = c.key;
    cmd.value = c.value;
    EXPECT_EQ(cmd.is_valid(), c.expected)
        << "op=[" << c.op << "] key=[" << c.key << "] value=[" << c.value
        << "]";
  }
}

} // namespace
} // namespace kvdb
