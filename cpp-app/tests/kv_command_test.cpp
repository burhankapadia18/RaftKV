/**
 * @file kv_command_test.cpp
 * @brief Unit tests for KVCommand (spec R0.10).
 *
 * KVCommand::from_msgpack is the trust boundary for every byte a client sends:
 * the HTTP layer forwards the body unparsed, so this is the first and only
 * place the payload is validated. These tests pin the decode contract,
 * including the places where it is permissive.
 *
 * Phase 1 (R1.2/R1.3) added validation_error(), which is where the reason a
 * command is rejected now lives; is_valid() is a thin wrapper over it. What
 * Apply() does with that reason is pinned in state_machine_test.cpp.
 */

#include <gtest/gtest.h>

#include <exception>
#include <map>
#include <optional>
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
  // the decode intact - and since Phase 2 they survive storage as well: the
  // base file and the WAL are length-prefixed too. See
  // ValueContainingNulSurvivesReload and
  // ValueContainingNewlineRoundTripsExactly in kv_store_test.cpp. (Before
  // Phase 2 the newline was truncated by the line-based kv.db.)
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

  // Decoding succeeds; validation is what flags the problem. Since Phase 1
  // (R1.2/R1.3) StateMachineService::Apply runs validation_error() before it
  // touches the store, so an unknown op and an empty key both come back as
  // success=false with the reason in ApplyResponse.error - see
  // ApplyRejectsUnknownOpWithAReason / ApplyRejectsEmptyKeyWithoutTouchingStore
  // in state_machine_test.cpp. (Before that, an unknown op was caught by the
  // switch's UNKNOWN arm with no reason attached, and the empty key was not
  // caught at all: store_.set("", value) ran and reported success.)
  //
  // This test pins only the decode contract.
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
    // is_valid() is implemented in terms of validation_error(); pin that the
    // two can never disagree about whether a command is acceptable.
    EXPECT_EQ(cmd.validation_error().has_value(), !c.expected)
        << "op=[" << c.op << "] key=[" << c.key << "] value=[" << c.value
        << "]";
  }
}

TEST(KVCommandTest, ValidationErrorNamesTheOffendingField) {
  // R1.2/R1.3: the reason strings are the payload of ApplyResponse.error, so
  // they are part of the observable contract, not just log text. They must
  // distinguish the two rejection causes and name the op.
  struct Case {
    const char *op;
    const char *key;
    const char *expected; // nullptr == valid, expect std::nullopt
  };

  const Case cases[] = {
      {"SET", "k", nullptr},
      {"DELETE", "k", nullptr},
      // The op is quoted so an op that is empty or pure whitespace is still
      // legible in a log line or an HTTP error body.
      {"PATCH", "k", "unknown operation: \"PATCH\""},
      {"set", "k", "unknown operation: \"set\""},
      {"", "k", "unknown operation: \"\""},
      // An unrecognized op is reported even when the key is also empty - the
      // op is checked first because "SET with no key" only makes sense to say
      // about an op the store actually knows.
      {"PATCH", "", "unknown operation: \"PATCH\""},
      {"SET", "", "empty key for operation \"SET\""},
      {"DELETE", "", "empty key for operation \"DELETE\""},
  };

  for (const Case &c : cases) {
    KVCommand cmd;
    cmd.op = c.op;
    cmd.key = c.key;
    cmd.value = "v";

    const std::optional<std::string> reason = cmd.validation_error();

    if (c.expected == nullptr) {
      EXPECT_FALSE(reason.has_value())
          << "op=[" << c.op << "] key=[" << c.key << "] was rejected";
      continue;
    }

    ASSERT_TRUE(reason.has_value())
        << "op=[" << c.op << "] key=[" << c.key << "] was accepted";
    EXPECT_EQ(*reason, c.expected);
  }
}

} // namespace
} // namespace kvdb
