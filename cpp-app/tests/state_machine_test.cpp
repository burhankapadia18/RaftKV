/**
 * @file state_machine_test.cpp
 * @brief Unit tests for StateMachineService::Apply (spec R1.2 / R1.3) and for
 *        the snapshot helpers behind GetSnapshot / RestoreSnapshot (R3.3).
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
 *
 * The two snapshot RPCs cannot be driven the same way - grpc::ServerWriter and
 * grpc::ServerReader have no public constructor and only a running server can
 * hand you one. So the logic that can actually be wrong lives in
 * kvdb::snapshot::for_each_chunk() and kvdb::snapshot::restore_from_payload(),
 * which are covered here directly, and the RPC bodies are thin adapters over
 * them. What that leaves uncovered by unit tests is exactly the adapter layer:
 * that GetSnapshot pushes each chunk into ServerWriter::Write, that
 * RestoreSnapshot drains ServerReader into one buffer, and the status codes
 * they return. Those are covered by the e2e suite against a real cluster.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

  /** @brief Snapshot support (R3.2): a plain copy of the contents. */
  [[nodiscard]] StateMap snapshot_state() const override {
    return StateMap(data.begin(), data.end());
  }

  /** @brief Snapshot support (R3.2): replace, never merge. */
  void restore_state(StateMap state) override {
    data.clear();
    data.insert(state.begin(), state.end());
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
// The decode throws internally, but the catch-all reports it the SAME way as a
// validation failure: gRPC OK, success=false, reason in `error`. That is
// correct because the verdict is deterministic - every replica decoding these
// bytes fails identically, so no replica ends up diverged.
//
// It used to return grpc::Status(INTERNAL, ...) instead, which was wrong twice:
// gRPC drops the response message when the status is non-OK, so `error` never
// reached the sidecar at all; and CppFSM.Apply routes transport errors down its
// "THIS REPLICA MAY NOW BE DIVERGED" branch, so every malformed write any
// client sent raised a false divergence alarm on all three nodes.

TEST(StateMachineServiceTest, ApplyRejectsMalformedMsgpackDeterministically) {
  FakeKVStore store;
  StateMachineService service(store);
  // 0xc1 is the one byte the MsgPack spec marks "never used".
  const consensus::Command request =
      command_with_payload(std::string(1, static_cast<char>(0xc1)));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  // gRPC OK so the reply (and therefore the reason) actually reaches the
  // sidecar - a non-OK status would discard the message.
  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  // rfind(prefix, 0) == 0 is "starts with" without pulling in gmock (the
  // test target links gtest only, and CI installs libgtest-dev alone).
  EXPECT_EQ(reply.error().rfind("malformed payload: ", 0), 0u)
      << "error was: " << reply.error();
  EXPECT_EQ(store.writes, 0);
  EXPECT_TRUE(store.data.empty());
}

TEST(StateMachineServiceTest, ApplyRejectsAnEmptyPayloadDeterministically) {
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload("");
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  // rfind(prefix, 0) == 0 is "starts with" without pulling in gmock (the
  // test target links gtest only, and CI installs libgtest-dev alone).
  EXPECT_EQ(reply.error().rfind("malformed payload: ", 0), 0u)
      << "error was: " << reply.error();
  EXPECT_EQ(store.writes, 0);
}

TEST(StateMachineServiceTest, ApplyRejectsAWrongTypedFieldDeterministically) {
  // A non-string "op" is a type error inside msgpack rather than a validation
  // failure, so it arrives via the throwing path - but is reported identically.
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

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  // rfind(prefix, 0) == 0 is "starts with" without pulling in gmock (the
  // test target links gtest only, and CI installs libgtest-dev alone).
  EXPECT_EQ(reply.error().rfind("malformed payload: ", 0), 0u)
      << "error was: " << reply.error();
  EXPECT_EQ(store.writes, 0);
}

// --- Snapshot chunking (R3.3, the GetSnapshot half) -----------------------
//
// GetSnapshot itself is a five-line adapter: serialize, then push each chunk
// into ServerWriter::Write. Everything that decides what goes on the wire is
// for_each_chunk(), so that is what is tested here.

/** @brief Everything for_each_chunk() emitted, plus whether it finished. */
struct ChunkLog {
  std::vector<std::string> chunks;
  bool completed = false;

  [[nodiscard]] std::string joined() const {
    std::string out;
    for (const std::string &chunk : chunks) {
      out.append(chunk);
    }
    return out;
  }
};

/** @brief Drive for_each_chunk() with an emitter that accepts everything. */
ChunkLog collect_chunks(const std::string &payload, size_t chunk_size) {
  ChunkLog log;
  log.completed =
      snapshot::for_each_chunk(payload, chunk_size, [&log](std::string_view c) {
        log.chunks.emplace_back(c);
        return true;
      });
  return log;
}

TEST(SnapshotChunkingTest, AnEmptyStoreStillStreamsItsHeader) {
  // The failure this pins: short-circuiting an empty store to "no chunks".
  // An empty snapshot is a real snapshot - it says "the agreed state is
  // nothing" - and a receiver that got zero bytes has to be able to tell that
  // apart from a stream that died before its first chunk.
  const std::string payload = serialize_state({});
  ASSERT_FALSE(payload.empty());

  const ChunkLog log = collect_chunks(payload, snapshot::kChunkSize);

  EXPECT_TRUE(log.completed);
  ASSERT_EQ(log.chunks.size(), 1u);
  EXPECT_EQ(log.joined(), payload);
  EXPECT_EQ(payload.rfind("KVB1", 0), 0u) << "chunks carry the KVB1 image";
}

TEST(SnapshotChunkingTest, ASnapshotSmallerThanOneChunkIsSentWhole) {
  const std::string payload =
      serialize_state({{"user:1", "alice"}, {"user:2", "bob"}});
  ASSERT_LT(payload.size(), snapshot::kChunkSize);

  const ChunkLog log = collect_chunks(payload, snapshot::kChunkSize);

  EXPECT_TRUE(log.completed);
  ASSERT_EQ(log.chunks.size(), 1u);
  EXPECT_EQ(log.chunks.front(), payload);
}

TEST(SnapshotChunkingTest, ALargeSnapshotSpansChunksWithARaggedTail) {
  // Sized against the real 64 KiB wire chunk, not a toy one, so the constant
  // itself is under test.
  StateMap state;
  const std::string value(1000, 'v');
  for (int i = 0; i < 200; ++i) {
    state["key:" + std::to_string(100000 + i)] = value;
  }
  const std::string payload = serialize_state(state);
  ASSERT_GT(payload.size(), 2 * snapshot::kChunkSize);
  ASSERT_NE(payload.size() % snapshot::kChunkSize, 0u)
      << "this test is only interesting when the final chunk is a partial one";

  const ChunkLog log = collect_chunks(payload, snapshot::kChunkSize);

  EXPECT_TRUE(log.completed);
  const size_t expected_chunks =
      (payload.size() + snapshot::kChunkSize - 1) / snapshot::kChunkSize;
  ASSERT_EQ(log.chunks.size(), expected_chunks);
  for (size_t i = 0; i + 1 < log.chunks.size(); ++i) {
    EXPECT_EQ(log.chunks[i].size(), snapshot::kChunkSize) << "chunk " << i;
  }
  EXPECT_EQ(log.chunks.back().size(),
            payload.size() % snapshot::kChunkSize); // the ragged tail
  EXPECT_EQ(log.joined(), payload);
}

TEST(SnapshotChunkingTest, AnExactMultipleDoesNotEmitATrailingEmptyChunk) {
  const std::string payload(24, 'x');

  const ChunkLog log = collect_chunks(payload, 8);

  EXPECT_TRUE(log.completed);
  ASSERT_EQ(log.chunks.size(), 3u);
  EXPECT_EQ(log.joined(), payload);
}

TEST(SnapshotChunkingTest, AnEmptyPayloadStillEmitsExactlyOneChunk) {
  // Not reachable from a real store - serialize_state() always writes at least
  // the header - but the "never zero chunks" rule is pinned for the degenerate
  // input too, so it cannot be lost to a later "optimisation".
  const ChunkLog log = collect_chunks("", snapshot::kChunkSize);

  EXPECT_TRUE(log.completed);
  ASSERT_EQ(log.chunks.size(), 1u);
  EXPECT_TRUE(log.chunks.front().empty());
}

TEST(SnapshotChunkingTest, StopsAtTheFirstRefusedWrite) {
  // ServerWriter::Write() returning false means the peer is gone or the call
  // was cancelled. The stream has to stop there and report failure, not keep
  // pushing chunks at a dead connection.
  const std::string payload(100, 'x');
  int attempts = 0;

  const bool completed =
      snapshot::for_each_chunk(payload, 10, [&attempts](std::string_view) {
        ++attempts;
        return attempts < 3; // the third write fails
      });

  EXPECT_FALSE(completed);
  EXPECT_EQ(attempts, 3) << "nothing may be written after a refused write";
}

TEST(SnapshotChunkingTest, RejectsAZeroChunkSize) {
  // Would otherwise emit empty chunks forever.
  EXPECT_THROW((void)snapshot::for_each_chunk(
                   "data", 0, [](std::string_view) { return true; }),
               std::invalid_argument);
}

// --- Snapshot install (R3.3, the RestoreSnapshot half) --------------------
//
// The rule under test throughout: decode fully, THEN touch the store. A node
// that half-restores serves a state no member of the cluster ever had, and
// nothing says so - strictly worse than refusing the snapshot outright.

TEST(SnapshotRestoreTest, RestoresTheDecodedStateIntoTheStore) {
  FakeKVStore store;
  const std::string payload =
      serialize_state({{"user:1", "alice"}, {"user:2", "bob"}});

  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(store, payload);

  ASSERT_FALSE(refusal.has_value()) << "refused: " << refusal.value_or("");
  ASSERT_TRUE(store.get("user:1").has_value());
  EXPECT_EQ(*store.get("user:1"), "alice");
  ASSERT_TRUE(store.get("user:2").has_value());
  EXPECT_EQ(*store.get("user:2"), "bob");
}

TEST(SnapshotRestoreTest, RestoreReplacesTheStoreRatherThanMergingIntoIt) {
  // A restore means "this node's state is now exactly the snapshot". A key the
  // node held that the snapshot does not must disappear, or a wiped follower
  // rejoins carrying state the cluster never agreed on.
  FakeKVStore store;
  store.data["stale"] = "leftover";
  const std::string payload = serialize_state({{"fresh", "value"}});

  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(store, payload);

  ASSERT_FALSE(refusal.has_value()) << "refused: " << refusal.value_or("");
  EXPECT_FALSE(store.contains("stale"));
  ASSERT_TRUE(store.get("fresh").has_value());
  EXPECT_EQ(*store.get("fresh"), "value");
}

TEST(SnapshotRestoreTest, RestoringAnEmptySnapshotEmptiesTheStore) {
  FakeKVStore store;
  store.data["stale"] = "leftover";
  const std::string payload = serialize_state({});

  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(store, payload);

  ASSERT_FALSE(refusal.has_value()) << "refused: " << refusal.value_or("");
  EXPECT_FALSE(store.contains("stale"));
}

TEST(SnapshotRestoreTest, RefusesABadMagicWithoutTouchingTheStore) {
  FakeKVStore store;
  store.data["keep"] = "me";
  std::string payload = serialize_state({{"intruder", "value"}});
  ASSERT_FALSE(payload.empty());
  payload[0] = 'X'; // "XVB1..."

  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(store, payload);

  ASSERT_TRUE(refusal.has_value());
  // rfind(prefix, 0) == 0 is "starts with" without pulling in gmock (the
  // test target links gtest only, and CI installs libgtest-dev alone).
  EXPECT_EQ(refusal->rfind("invalid snapshot: ", 0), 0u) << *refusal;
  EXPECT_GT(refusal->size(), std::string("invalid snapshot: ").size())
      << "a refusal has to say what was wrong with it";
  EXPECT_FALSE(store.contains("intruder"));
  ASSERT_TRUE(store.get("keep").has_value());
  EXPECT_EQ(*store.get("keep"), "me");
  EXPECT_EQ(store.writes, 0);
}

TEST(SnapshotRestoreTest, RefusesATruncatedPayloadWithoutTouchingTheStore) {
  // A stream cut short mid-record. The header still looks plausible, so this is
  // the case where "validate the magic and go" would install a partial state.
  FakeKVStore store;
  store.data["keep"] = "me";
  const std::string payload =
      serialize_state({{"intruder", "a value long enough to cut"}});
  ASSERT_GT(payload.size(), 5u);
  const std::string truncated = payload.substr(0, payload.size() - 5);
  ASSERT_EQ(truncated.rfind("KVB1", 0), 0u) << "the magic is still intact";

  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(store, truncated);

  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->rfind("invalid snapshot: ", 0), 0u) << *refusal;
  EXPECT_FALSE(store.contains("intruder"));
  ASSERT_TRUE(store.get("keep").has_value());
  EXPECT_EQ(*store.get("keep"), "me");
  EXPECT_EQ(store.writes, 0);
}

TEST(SnapshotRestoreTest, RefusesAnEmptyPayloadWithoutTouchingTheStore) {
  // Zero bytes is what the receiver sees when the stream dies before the first
  // chunk. It is NOT an empty snapshot - that one carries the header - so it
  // must be refused rather than silently wiping the node.
  FakeKVStore store;
  store.data["keep"] = "me";

  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(store, "");

  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->rfind("invalid snapshot: ", 0), 0u) << *refusal;
  ASSERT_TRUE(store.get("keep").has_value());
  EXPECT_EQ(*store.get("keep"), "me");
  EXPECT_EQ(store.writes, 0);
}

// --- Both halves together -------------------------------------------------

TEST(SnapshotRoundTripTest, SnapshotOfOneStoreRestoresIntoAnother) {
  // The whole transfer, minus gRPC: what GetSnapshot does to produce the bytes,
  // then what RestoreSnapshot does with them on the far side.
  FakeKVStore source;
  source.set("user:1", "alice");
  source.set("user:2", "bob");

  const std::string payload = serialize_state(source.snapshot_state());
  std::string received;
  const bool completed = snapshot::for_each_chunk(
      payload, snapshot::kChunkSize, [&received](std::string_view chunk) {
        received.append(chunk.data(), chunk.size());
        return true;
      });
  ASSERT_TRUE(completed);
  ASSERT_EQ(received, payload);

  FakeKVStore target;
  target.data["obsolete"] = "state";
  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(target, received);

  ASSERT_FALSE(refusal.has_value()) << "refused: " << refusal.value_or("");
  EXPECT_FALSE(target.contains("obsolete"));
  ASSERT_TRUE(target.get("user:1").has_value());
  EXPECT_EQ(*target.get("user:1"), "alice");
  ASSERT_TRUE(target.get("user:2").has_value());
  EXPECT_EQ(*target.get("user:2"), "bob");
}

TEST(SnapshotRoundTripTest, KeysThatBreakLineFormatsSurviveChunking) {
  // The reason the wire format is the KVB1 image and not something ad hoc:
  // '=', newlines and NUL bytes ride through untouched, including when a chunk
  // boundary lands in the middle of one.
  const StateMap state = {
      {"a=b", "value=with=equals"},
      {"multi\nline", "two\nlines"},
      {std::string("nul\0key", 7), std::string("nul\0value", 9)},
  };
  const std::string payload = serialize_state(state);

  std::string received;
  const bool completed =
      snapshot::for_each_chunk(payload, 7, [&received](std::string_view chunk) {
        received.append(chunk.data(), chunk.size());
        return true;
      });
  ASSERT_TRUE(completed);
  ASSERT_EQ(received, payload);

  FakeKVStore store;
  const std::optional<std::string> refusal =
      snapshot::restore_from_payload(store, received);

  ASSERT_FALSE(refusal.has_value()) << "refused: " << refusal.value_or("");
  for (const auto &[key, value] : state) {
    ASSERT_TRUE(store.get(key).has_value())
        << "key of " << key.size() << " bytes went missing";
    EXPECT_EQ(*store.get(key), value);
  }
}

// --- The reserved key space guard -----------------------------------------
//
// The security argument for these tests: the sidecar's RaftNode port (50052) is
// peer-reachable and UNAUTHENTICATED, so anything inside the network can get a
// command committed without presenting a credential. Apply is therefore the
// only place that can stop such a caller from rewriting the user table, and the
// HTTP-layer checks are irrelevant to it. If these tests fail, the ACL system
// is bypassable by anyone who can open a TCP connection to a node.

/** @brief Apply one command and return the reply, for the terser tests below.
 */
consensus::ApplyResponse apply_payload(FakeKVStore &store,
                                       const std::string &payload) {
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(payload);
  consensus::ApplyResponse reply;
  const grpc::Status status = service.Apply(nullptr, &request, &reply);
  EXPECT_TRUE(status.ok()) << "Apply must report failures in the reply, not as "
                              "a non-OK status (the Go FSM reads a transport "
                              "error as possible divergence)";
  return reply;
}

/** @brief A valid encoded record for @p name with password @p password. */
std::string encoded_record(const std::string &name,
                           const std::string &password) {
  auth::UserUpsertRequest request;
  request.password = password;
  request.classes = {auth::kClassRead};
  request.patterns = {"app:*"};
  return request.to_record(name).to_msgpack();
}

TEST(StateMachineServiceTest, PlainSetIsRefusedOnTheReservedPrefix) {
  FakeKVStore store;
  const consensus::ApplyResponse reply =
      apply_payload(store, pack_string_map({{"op", "SET"},
                                            {"key", "__sys:user:admin"},
                                            {"value", "forged"}}));

  EXPECT_FALSE(reply.success());
  EXPECT_NE(reply.error().find("reserved prefix"), std::string::npos);
  // The store must be untouched — not merely unchanged in content.
  EXPECT_EQ(store.writes, 0);
  EXPECT_FALSE(store.contains("__sys:user:admin"));
}

TEST(StateMachineServiceTest, PlainDeleteIsRefusedOnTheReservedPrefix) {
  // The delete direction matters as much as the write: being able to remove
  // `__sys:user:alice` is being able to lock a user out, and removing every
  // record is being able to empty the ACL table.
  FakeKVStore store;
  store.data["__sys:user:alice"] = encoded_record("alice", "s3cr3t-password");

  const consensus::ApplyResponse reply = apply_payload(
      store, pack_string_map({{"op", "DELETE"}, {"key", "__sys:user:alice"}}));

  EXPECT_FALSE(reply.success());
  EXPECT_NE(reply.error().find("reserved prefix"), std::string::npos);
  EXPECT_EQ(store.writes, 0);
  EXPECT_TRUE(store.contains("__sys:user:alice"));
}

TEST(StateMachineServiceTest,
     TheGuardCoversTheWholeReservedPrefixNotJustUsers) {
  // `__sys:` is reserved as a whole, so a future subtree cannot be squatted on
  // by a client now and become someone else's state later.
  FakeKVStore store;
  for (const std::string &key :
       {std::string("__sys:"), std::string("__sys:anything"),
        std::string("__sys:user:"), std::string("__sys:x:y:z")}) {
    const consensus::ApplyResponse reply = apply_payload(
        store, pack_string_map({{"op", "SET"}, {"key", key}, {"value", "v"}}));
    EXPECT_FALSE(reply.success()) << "accepted key " << key;
  }
  EXPECT_EQ(store.writes, 0);
}

TEST(StateMachineServiceTest, OrdinaryKeysThatMerelyContainTheMarkerStillWork) {
  // Only a PREFIX is reserved. Refusing keys that merely contain "__sys:"
  // would take names clients may already be using, and this store's keys are
  // arbitrary bytes.
  FakeKVStore store;
  const consensus::ApplyResponse reply =
      apply_payload(store, pack_string_map({{"op", "SET"},
                                            {"key", "app:__sys:not-reserved"},
                                            {"value", "v"}}));

  EXPECT_TRUE(reply.success()) << reply.error();
  EXPECT_EQ(store.writes, 1);
}

// --- USER_SET / USER_DEL --------------------------------------------------

TEST(StateMachineServiceTest, UserSetStoresTheRecordUnderTheDerivedKey) {
  FakeKVStore store;
  const std::string record = encoded_record("alice", "s3cr3t-password");

  const consensus::ApplyResponse reply = apply_payload(
      store, pack_string_map(
                 {{"op", "USER_SET"}, {"key", "alice"}, {"value", record}}));

  EXPECT_TRUE(reply.success()) << reply.error();
  EXPECT_EQ(reply.error(), "");
  // The command carries the BARE name; Apply derives the storage key, which is
  // what makes a name/key mismatch unrepresentable rather than a validation
  // problem.
  ASSERT_TRUE(store.get("__sys:user:alice").has_value());
  EXPECT_EQ(*store.get("__sys:user:alice"), record)
      << "the stored bytes must be the committed bytes, verbatim";
  EXPECT_FALSE(store.contains("alice"));
}

TEST(StateMachineServiceTest, UserSetIsRejectedForAMismatchedName) {
  // The authenticator reads `name` back out of the record, so a record stored
  // under one name while claiming another is a record whose identity depends on
  // which field you read. Refuse rather than pick.
  FakeKVStore store;
  const consensus::ApplyResponse reply = apply_payload(
      store,
      pack_string_map({{"op", "USER_SET"},
                       {"key", "alice"},
                       {"value", encoded_record("bob", "s3cr3t-pass")}}));

  EXPECT_FALSE(reply.success());
  EXPECT_NE(reply.error().find("does not match"), std::string::npos);
  EXPECT_EQ(store.writes, 0);
}

TEST(StateMachineServiceTest, UserSetIsRejectedForBadNamesAndBadRecords) {
  FakeKVStore store;

  // An unusable user name.
  EXPECT_FALSE(
      apply_payload(
          store,
          pack_string_map({{"op", "USER_SET"},
                           {"key", "has space"},
                           {"value", encoded_record("has space", "pwpwpwpw")}}))
          .success());
  // A value that is not a record at all.
  const consensus::ApplyResponse malformed =
      apply_payload(store, pack_string_map({{"op", "USER_SET"},
                                            {"key", "alice"},
                                            {"value", "not msgpack"}}));
  EXPECT_FALSE(malformed.success());
  EXPECT_NE(malformed.error().find("malformed user record"), std::string::npos);
  // An empty value.
  EXPECT_FALSE(apply_payload(store, pack_string_map({{"op", "USER_SET"},
                                                     {"key", "alice"},
                                                     {"value", ""}}))
                   .success());
  // An empty key, caught by KVCommand::validation_error before any of this.
  EXPECT_FALSE(apply_payload(store, pack_string_map({{"op", "USER_SET"},
                                                     {"key", ""},
                                                     {"value", "x"}}))
                   .success());

  EXPECT_EQ(store.writes, 0) << "no rejected record may reach the store";
}

TEST(StateMachineServiceTest,
     UserSetRejectionIsDeterministicNotATransportError) {
  // Every rejection above must arrive as success=false with Status::OK. A
  // non-OK status sends the Go FSM down its "THIS REPLICA MAY NOW BE DIVERGED"
  // branch, and none of these are divergence — the verdict is a pure function
  // of the committed bytes, so all replicas reach it identically.
  FakeKVStore store;
  StateMachineService service(store);
  const consensus::Command request = command_with_payload(pack_string_map(
      {{"op", "USER_SET"}, {"key", "alice"}, {"value", "garbage"}}));
  consensus::ApplyResponse reply;

  const grpc::Status status = service.Apply(nullptr, &request, &reply);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(reply.success());
  EXPECT_NE(reply.error(), "");
}

TEST(StateMachineServiceTest, UserDelRemovesTheRecordAndIsIdempotent) {
  FakeKVStore store;
  store.data["__sys:user:alice"] = encoded_record("alice", "s3cr3t-password");

  const consensus::ApplyResponse first = apply_payload(
      store, pack_string_map({{"op", "USER_DEL"}, {"key", "alice"}}));

  EXPECT_TRUE(first.success()) << first.error();
  EXPECT_FALSE(store.contains("__sys:user:alice"));

  // Deleting an absent user SUCCEEDS. Writes are at-least-once under failure,
  // so a client retrying a delete it never saw acknowledged must not be told
  // the retry failed.
  const consensus::ApplyResponse second = apply_payload(
      store, pack_string_map({{"op", "USER_DEL"}, {"key", "alice"}}));

  EXPECT_TRUE(second.success()) << second.error();
}

TEST(StateMachineServiceTest, UserDelIsRejectedForAnUnusableName) {
  FakeKVStore store;
  const consensus::ApplyResponse reply = apply_payload(
      store, pack_string_map({{"op", "USER_DEL"}, {"key", "__sys:user:x"}}));

  EXPECT_FALSE(reply.success());
  EXPECT_EQ(store.writes, 0);
}

} // namespace
} // namespace kvdb
