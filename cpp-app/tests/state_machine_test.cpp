/**
 * @file state_machine_test.cpp
 * @brief Unit tests for StateMachineService::Apply (spec R1.2 / R1.3).
 *
 * Apply() is the apply boundary: every committed raft entry, on every node,
 * arrives here and nowhere else. It is also the first and only place the
 * client payload is parsed, because the HTTP layer forwards the body opaquely.
 * That makes it the last line of defence, and until Phase 1 it was not
 * defending anything - KVCommand::is_valid() was never called, so
 * {op:"SET", key:""} was written to the store and reported as a success.
 *
 * These tests exercise the service in-process with a fake IKVStore, so
 * "the store was not touched" can be asserted precisely. No gRPC server is
 * involved: Apply() ignores its ServerContext, so a null one is fine.
 */

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

#include <msgpack.hpp>

#include "consensus.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include "raft/state_machine.hpp"
#include "storage/kv_store.hpp"

namespace kvdb {
namespace {

/**
 * @brief In-memory IKVStore that also counts the writes it was asked to do.
 *
 * The counter is what makes "the store was not modified" a real assertion:
 * comparing the contents alone would pass for a rejected DELETE of a key that
 * was never there.
 */
class FakeKVStore : public IKVStore {
public:
  void set(const std::string &key, const std::string &value) override {
    ++writes;
    data[key] = value;
  }

  [[nodiscard]] std::optional<std::string>
  get(const std::string &key) const override {
    auto it = data.find(key);
    if (it == data.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool remove(const std::string &key) override {
    ++writes;
    return data.erase(key) > 0;
  }

  [[nodiscard]] bool contains(const std::string &key) const override {
    return data.count(key) > 0;
  }

  /** @brief Every set()/remove() call, whether or not it changed anything. */
  int writes = 0;
  std::map<std::string, std::string> data;
};

/**
 * @brief Pack a MsgPack map of string->string.
 *
 * Mirrors what test_client.py and tests/e2e put on the wire (a MsgPack map,
 * not the array variant), which is what MSGPACK_DEFINE_MAP expects.
 */
std::string pack_string_map(const std::map<std::string, std::string> &fields) {
  msgpack::sbuffer buffer;
  msgpack::pack(buffer, fields);
  return std::string(buffer.data(), buffer.size());
}

/**
 * @brief Build the Command the sidecar sends.
 *
 * The payload goes in `data` and nothing else is set - that is the wire
 * convention documented in .claude/rules/protobuf.md.
 */
consensus::Command command_with_payload(const std::string &payload) {
  consensus::Command request;
  request.set_data(payload);
  return request;
}

// --- Accepted commands ----------------------------------------------------

TEST(StateMachineServiceTest, ApplySetWritesToTheStoreAndReportsSuccess) {
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(
      pack_string_map({{"op", "SET"}, {"key", "user:1"}, {"value", "alice"}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.error(), ""); // error is empty iff success
  EXPECT_EQ(store.writes, 1);
  ASSERT_TRUE(store.get("user:1").has_value());
  EXPECT_EQ(*store.get("user:1"), "alice");
}

TEST(StateMachineServiceTest, ApplySetWithAnEmptyValueSucceeds) {
  // Only the key is required; a SET of the empty string is a legitimate write.
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(
      pack_string_map({{"op", "SET"}, {"key", "k"}, {"value", ""}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.error(), "");
  ASSERT_TRUE(store.get("k").has_value());
  EXPECT_EQ(*store.get("k"), "");
}

TEST(StateMachineServiceTest, ApplyDeleteRemovesFromTheStore) {
  FakeKVStore store;
  store.data["user:1"] = "alice";
  store.data["user:2"] = "bob";
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(
      pack_string_map({{"op", "DELETE"}, {"key", "user:1"}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.error(), "");
  EXPECT_FALSE(store.contains("user:1"));
  EXPECT_TRUE(store.contains("user:2"));
}

TEST(StateMachineServiceTest, ApplyDeleteOfAnAbsentKeyStillReportsSuccess) {
  // Apply() discards IKVStore::remove()'s bool. That is correct for a
  // replicated log - deleting a key that is already gone is idempotent, not a
  // failure - but it is worth pinning, because it is the one case where
  // success=true does not imply the store changed.
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(
      pack_string_map({{"op", "DELETE"}, {"key", "never-existed"}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.error(), "");
  EXPECT_TRUE(store.data.empty());
}

TEST(StateMachineServiceTest, ApplyReadsOnlyTheDataFieldOfTheCommand) {
  // Per .claude/rules/protobuf.md the op/key/value proto fields are unused in
  // transit; the whole payload rides in Command.data as opaque MsgPack.
  // Pinned so that populating them later cannot silently change apply
  // behavior for entries already in the raft log.
  FakeKVStore store;
  StateMachineService service(store);
  consensus::Command request = command_with_payload(
      pack_string_map({{"op", "SET"}, {"key", "real"}, {"value", "payload"}}));
  request.set_op("DELETE");
  request.set_key("decoy");
  request.set_value("ignored");
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(reply.success());
  ASSERT_TRUE(store.get("real").has_value());
  EXPECT_EQ(*store.get("real"), "payload");
  EXPECT_FALSE(store.contains("decoy"));
}

// --- Rejected commands (R1.2 / R1.3) --------------------------------------

TEST(StateMachineServiceTest, ApplyRejectsUnknownOpWithAReason) {
  // R1.3. Before Phase 1 this fell through the switch's UNKNOWN arm:
  // success=false with nothing on the wire explaining why.
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(
      pack_string_map({{"op", "PATCH"}, {"key", "k"}, {"value", "v"}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  // A rejection is a normal response, not a transport error: the caller has to
  // be able to tell "the state machine refused this" from "the call failed".
  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "unknown operation: \"PATCH\"");
  EXPECT_EQ(store.writes, 0);
  EXPECT_TRUE(store.data.empty());
}

TEST(StateMachineServiceTest, ApplyRejectsEmptyKeyWithoutTouchingStore) {
  // R1.2 - THE BUG Phase 1 fixes. is_valid() existed but was never called, so
  // this exact command ran store_.set("", "orphan") on every node and answered
  // success=true. The store assertions, not the reply ones, are the point.
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(
      pack_string_map({{"op", "SET"}, {"key", ""}, {"value", "orphan"}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "empty key for operation \"SET\"");
  EXPECT_NE(reply.error().find("key"), std::string::npos);
  EXPECT_EQ(store.writes, 0);
  EXPECT_TRUE(store.data.empty());
  EXPECT_FALSE(store.contains(""));
}

TEST(StateMachineServiceTest, ApplyRejectsDeleteWithAnEmptyKey) {
  FakeKVStore store;
  store.data[""] = "planted"; // a leftover from before the fix
  StateMachineService service(store);
  const consensus::Command request =
      command_with_payload(pack_string_map({{"op", "DELETE"}, {"key", ""}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "empty key for operation \"DELETE\"");
  EXPECT_EQ(store.writes, 0);
  ASSERT_TRUE(store.get("").has_value());
  EXPECT_EQ(*store.get(""), "planted"); // untouched
}

TEST(StateMachineServiceTest, ApplyRejectsAMissingKeyField) {
  // MSGPACK_DEFINE_MAP leaves absent members at their default, so a payload
  // with no "key" at all decodes to an empty key and takes the same path.
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(
      pack_string_map({{"op", "SET"}, {"value", "orphan"}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "empty key for operation \"SET\"");
  EXPECT_EQ(store.writes, 0);
}

TEST(StateMachineServiceTest, ApplyRejectsAnEmptyMapPayload) {
  // Empty op AND empty key: the unknown op is reported, since "SET with no
  // key" is only a sensible thing to say about an op the store recognizes.
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(pack_string_map({}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "unknown operation: \"\"");
  EXPECT_EQ(store.writes, 0);
}

// --- Malformed payloads ---------------------------------------------------
//
// These take the other failure shape: the decode throws, and the catch-all
// answers with a non-OK gRPC status. Note that gRPC does not deliver a
// response message alongside an error status, so the `error` set on the reply
// is only observable in-process (as here); over the wire the Go FSM sees the
// transport error instead. Phase 1 deliberately did NOT change which of the
// two shapes a malformed payload produces - CppFSM.Apply (R1.4) handles both.

TEST(StateMachineServiceTest, ApplyReportsMalformedMsgpackAsInternal) {
  FakeKVStore store;
  StateMachineService service(store);
  // 0xc1 is the one byte the MsgPack spec marks "never used".
  const consensus::Command request =
      command_with_payload(std::string(1, static_cast<char>(0xc1)));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(status.error_message().empty());
  EXPECT_FALSE(reply.success());
  EXPECT_FALSE(reply.error().empty());
  EXPECT_EQ(reply.error(), status.error_message());
  EXPECT_EQ(store.writes, 0);
  EXPECT_TRUE(store.data.empty());
}

TEST(StateMachineServiceTest, ApplyReportsAnEmptyPayloadAsInternal) {
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload("");
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(reply.success());
  EXPECT_FALSE(reply.error().empty());
  EXPECT_EQ(store.writes, 0);
}

TEST(StateMachineServiceTest, ApplyReportsAWrongTypedFieldAsInternal) {
  // A non-string "op" is a type error inside msgpack, not a validation
  // failure, so it arrives via the throwing path rather than as success=false.
  msgpack::sbuffer buffer;
  msgpack::packer<msgpack::sbuffer> pk(&buffer);
  pk.pack_map(3);
  pk.pack(std::string("op"));
  pk.pack(42); // integer where a string is expected
  pk.pack(std::string("key"));
  pk.pack(std::string("k"));
  pk.pack(std::string("value"));
  pk.pack(std::string("v"));

  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request =
      command_with_payload(std::string(buffer.data(), buffer.size()));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(reply.success());
  EXPECT_FALSE(reply.error().empty());
  EXPECT_EQ(store.writes, 0);
}

} // namespace
} // namespace kvdb
