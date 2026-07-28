#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "consensus.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include "../commands/kv_command.hpp"
#include "../common/log.hpp"
#include "../common/metrics.hpp"
#include "../storage/kv_store.hpp"

namespace kvdb {

/**
 * @brief Transport-free halves of the snapshot RPCs (R3.3).
 *
 * grpc::ServerWriter and grpc::ServerReader cannot be constructed outside a
 * running server, so everything that could get the bytes wrong - the chunking,
 * the decode, the "do not touch the store until it has fully decoded" rule -
 * lives here as plain functions a unit test can call directly, and the two RPC
 * bodies below are deliberately thin adapters over them.
 */
namespace snapshot {

/**
 * @brief Bytes per chunk on the wire (R3.1). 64 KiB.
 *
 * A snapshot is the Phase 2 base-file encoding, unchanged: one format for disk
 * and wire, so what streams out of GetSnapshot is byte-identical to what kv.db
 * holds.
 */
inline constexpr size_t kChunkSize = 65536;

/**
 * @brief Hand @p payload to @p emit in pieces of at most @p chunk_size bytes.
 *
 * Emits one chunk per @p chunk_size bytes plus one more for any remainder, and
 * **never zero chunks**: an empty payload still emits a single empty chunk.
 * That rule matters because an empty store does not serialize to zero bytes -
 * it serializes to the KVB1 header with a zero entry count - so a receiver must
 * be able to tell "a snapshot of nothing" apart from "the stream died before
 * the first chunk". Short-circuiting an empty store to no chunks at all would
 * erase exactly that distinction.
 *
 * @param payload    Bytes to stream; may contain NULs.
 * @param chunk_size Maximum bytes handed to @p emit at once.
 * @param emit       Called with each chunk as a view into @p payload; returns
 *                   false to abort the stream.
 * @return false if @p emit aborted, in which case no further chunk was
 *         produced - a write the peer refused stops the stream rather than
 *         spinning on a dead connection.
 * @throws std::invalid_argument if @p chunk_size is 0, which would otherwise
 *         make no progress at all.
 */
template <typename Emit>
[[nodiscard]] bool for_each_chunk(const std::string &payload, size_t chunk_size,
                                  Emit emit) {
  if (chunk_size == 0) {
    throw std::invalid_argument("snapshot chunk size must be non-zero");
  }

  size_t offset = 0;
  // do/while, not while: the empty payload has to produce its one empty chunk.
  do {
    const size_t length = std::min(chunk_size, payload.size() - offset);
    if (!emit(std::string_view(payload.data() + offset, length))) {
      return false;
    }
    offset += length;
  } while (offset < payload.size());

  return true;
}

/**
 * @brief Decode a received snapshot and install it in @p store.
 *
 * The payload is decoded IN FULL before the store is touched, so a stream that
 * was truncated, corrupted, or never a snapshot at all leaves the node exactly
 * as it was. Half-restoring is the worst outcome available here: the node would
 * then serve a state no member of the cluster ever had, and unlike a refused
 * restore nothing would say so.
 *
 * @return std::nullopt on success; otherwise a description of why the snapshot
 *         was refused, suitable for RestoreResponse.error.
 */
[[nodiscard]] inline std::optional<std::string>
restore_from_payload(IKVStore &store, const std::string &payload) {
  StateMap state;

  // deserialize_state() checks the KVB1 magic and bounds every length it reads
  // against the bytes actually present, throwing rather than trusting any of
  // them. It is the only validation needed here - a second magic check in this
  // function would just be a copy of that rule, free to drift away from it.
  try {
    state = deserialize_state(payload);
  } catch (const std::exception &e) {
    const std::string reason = std::string("invalid snapshot: ") + e.what();
    log::warn(
        log::kComponentStateMachine, "refused snapshot",
        {log::field("bytes", payload.size()), log::field("reason", reason)});
    return reason;
  }

  const size_t entries = state.size();

  // Decoded cleanly, so commit it. restore_state() swaps the map in, rewrites
  // the base file atomically and resets the WAL, which is what makes a crash
  // immediately after a restore recover to exactly this snapshot rather than to
  // the snapshot plus a stale WAL replayed over it (R3.7).
  try {
    store.restore_state(std::move(state));
  } catch (const std::exception &e) {
    const std::string reason =
        std::string("could not install snapshot: ") + e.what();
    log::error(log::kComponentStateMachine, reason);
    return reason;
  }

  log::info(
      log::kComponentStateMachine, "restored snapshot",
      {log::field("entries", entries), log::field("bytes", payload.size())});
  return std::nullopt;
}

} // namespace snapshot

/**
 * @brief gRPC service implementing the Raft StateMachine.
 *
 * This service receives committed log entries from the Raft
 * sidecar and applies them to the local key-value store.
 *
 * Since Phase 3 it also serves the snapshot lifecycle in both directions: the
 * C++ side owns the state, so the sidecar asks it for a snapshot to hand to
 * raft (GetSnapshot) and hands one back when raft restores one
 * (RestoreSnapshot).
 *
 * Dependency Injection: Takes an IKVStore reference rather than
 * creating its own storage, allowing for testing and flexibility.
 */
class StateMachineService final : public consensus::StateMachine::Service {
public:
  /**
   * @brief Construct the state machine service.
   * @param store Reference to the key-value store to apply changes to
   */
  explicit StateMachineService(IKVStore &store) : store_(store) {}

  /**
   * @brief Apply a committed command from the Raft log.
   *
   * Deserializes the MsgPack command, validates it, and applies it to the
   * store. This is the only place the client payload is ever inspected (the
   * HTTP layer forwards the body opaquely), so it is also the only place a
   * bad command can be caught.
   *
   * Rejections (R1.2/R1.3) are reported in the response body
   * (`success=false` plus a reason in `error`) with gRPC status OK, so the
   * caller can tell "the state machine refused this entry" apart from "the
   * call did not get through".
   *
   * @param context gRPC server context (unused)
   * @param request The command containing MsgPack-encoded data
   * @param reply Response indicating success/failure and why
   * @return gRPC status
   */
  grpc::Status Apply(grpc::ServerContext *context,
                     const consensus::Command *request,
                     consensus::ApplyResponse *reply) override {
    // R5.4: applies are timed here, on the C++ side of the boundary. The Go
    // side times the same call from outside; the difference between the two is
    // the gRPC round trip, which is worth being able to see separately.
    const auto started = std::chrono::steady_clock::now();
    struct ApplyTimer {
      std::chrono::steady_clock::time_point started;
      ~ApplyTimer() {
        metrics::Registry::global()
            .histogram("raftkv_apply_duration_seconds",
                       "Time to apply one committed entry to the local store.")
            .observe(std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - started)
                         .count());
        metrics::Registry::global()
            .counter("raftkv_apply_total", "Committed entries applied locally.")
            .inc();
      }
    } timer{started};

    try {
      // Deserialize the command from MsgPack
      KVCommand cmd = KVCommand::from_msgpack(request->data().data(),
                                              request->data().size());

      // R1.2/R1.3: validate BEFORE touching the store. This check used to be
      // dead code, which is why {op:"SET", key:""} was written to the store
      // and reported as a success, and why an unknown op failed with no
      // reason attached.
      const std::optional<std::string> reason = cmd.validation_error();
      if (reason.has_value()) {
        log::warn(log::kComponentStateMachine, "rejected command",
                  {log::field("reason", *reason)});
        reply->set_success(false);
        reply->set_error(*reason);
        return grpc::Status::OK;
      }

      log::debug(log::kComponentStateMachine, "applied",
                 {log::field("op", cmd.op), log::field("key", cmd.key)});

      // Apply the operation to the store
      switch (cmd.operation_type()) {
      case Operation::SET:
        store_.set(cmd.key, cmd.value);
        break;
      case Operation::DELETE:
        store_.remove(cmd.key);
        break;
      case Operation::UNKNOWN:
        // Unreachable: validation_error() rejects UNKNOWN above. Kept so the
        // switch stays exhaustive for -Wswitch and can never fall through to
        // success=true should the two ever drift apart.
        reply->set_success(false);
        reply->set_error("unhandled operation: \"" + cmd.op + "\"");
        return grpc::Status::OK;
      }

      reply->set_success(true);
      return grpc::Status::OK;

    } catch (const std::exception &e) {
      // Malformed MsgPack is a DETERMINISTIC rejection, exactly like a failed
      // validation: the verdict is a pure function of the entry's bytes, so
      // every replica decoding this entry reaches it independently and
      // identically. It is reported the same way — gRPC OK, success=false,
      // reason in `error`.
      //
      // Returning a non-OK status here (as this did before) was wrong twice
      // over. gRPC does not deliver a response message alongside an error
      // status, so the `error` field was unobservable on the wire; and the Go
      // FSM routes transport errors down its "THIS REPLICA MAY NOW BE
      // DIVERGED" branch, so every bad-msgpack write any client sent raised a
      // false divergence alarm on all three nodes. Nothing was diverged.
      log::warn(log::kComponentStateMachine, "rejected malformed payload",
                {log::field("error", e.what())});
      reply->set_success(false);
      reply->set_error(std::string("malformed payload: ") + e.what());
      return grpc::Status::OK;
    }
  }

  /**
   * @brief Stream the whole store as a snapshot (R3.3).
   *
   * The consistent copy is taken by snapshot_state() under the store mutex and
   * the lock is released before a single byte goes on the wire, so the lock is
   * held O(copy) and never O(network) - a slow or stalled reader cannot block
   * writes (R3.2).
   *
   * An empty store is a perfectly good snapshot and streams as the bare KVB1
   * header. There is deliberately no "nothing to send" early return: the
   * receiver has to be able to tell an empty snapshot from a stream that failed
   * before its first chunk.
   *
   * Failures here have nowhere to be reported in-band - the response is a
   * stream of chunks with no error field - so unlike Apply they are returned as
   * a non-OK status.
   *
   * @param context gRPC server context (unused)
   * @param request Empty request (unused)
   * @param writer Sink for the 64 KiB chunks
   * @return gRPC status; non-OK if the state could not be serialized or the
   *         peer went away mid-stream
   */
  /**
   * @brief Read one key from the local store (R4.5).
   *
   * Deliberately NOT a linearizable read on its own — it is the second half of
   * one. The sidecar calls this only after it has run a Barrier and confirmed
   * with a quorum that it still leads; at that point the local store provably
   * contains every acknowledged write, which is what makes the surrounding read
   * linearizable. Called directly it gives exactly what
   * `GET ?consistency=local` gives you.
   *
   * @param context gRPC server context (unused)
   * @param request The key to look up
   * @param reply found plus the value when present
   * @return Always OK. A miss is a successful read that found nothing, not an
   *         error, so there is nothing to report here.
   */
  grpc::Status Get(grpc::ServerContext *context,
                   const consensus::GetRequest *request,
                   consensus::GetResponse *reply) override {
    const std::optional<std::string> value = store_.get(request->key());
    if (!value.has_value()) {
      reply->set_found(false);
      return grpc::Status::OK;
    }

    reply->set_found(true);
    // The two-argument form: `value` is a proto `bytes` field, and passing the
    // length explicitly is what lets a value containing NUL bytes survive.
    // Those have round-tripped since Phase 2 and must not start truncating
    // here.
    reply->set_value(value->data(), value->size());
    return grpc::Status::OK;
  }

  grpc::Status
  GetSnapshot(grpc::ServerContext *context,
              const consensus::SnapshotRequest *request,
              grpc::ServerWriter<consensus::SnapshotChunk> *writer) override {
    std::string payload;
    try {
      payload = serialize_state(store_.snapshot_state());
    } catch (const std::exception &e) {
      log::error(log::kComponentStateMachine, "snapshot failed",
                 {log::field("error", e.what())});
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          std::string("snapshot serialization failed: ") +
                              e.what());
    }

    size_t sent = 0;
    const bool complete = snapshot::for_each_chunk(
        payload, snapshot::kChunkSize, [writer, &sent](std::string_view chunk) {
          consensus::SnapshotChunk message;
          message.set_data(chunk.data(), chunk.size());
          // Write() returns false once the peer is gone or the call was
          // cancelled. Stop there instead of pushing the rest at a dead
          // connection.
          if (!writer->Write(message)) {
            return false;
          }
          sent += chunk.size();
          return true;
        });

    if (!complete) {
      log::warn(log::kComponentStateMachine, "snapshot stream aborted",
                {log::field("sent_bytes", sent),
                 log::field("total_bytes", payload.size())});
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "snapshot stream aborted before completion");
    }

    log::info(log::kComponentStateMachine, "snapshot sent",
              {log::field("bytes", payload.size())});
    return grpc::Status::OK;
  }

  /**
   * @brief Install a snapshot streamed in from the sidecar (R3.3).
   *
   * Chunks are accumulated, decoded in full and only then applied, so a stream
   * that is truncated or is not a snapshot at all leaves the node's state
   * untouched.
   *
   * A refusal is reported in the response (`success=false` plus a reason),
   * exactly like a rejected Apply and for the same reason: it is a
   * deterministic verdict on the bytes, not a transport failure, and the caller
   * has to be able to tell the two apart.
   *
   * The accumulated size is bounded only by what the peer sends. That is the
   * same trust model as the rest of this gRPC surface, which is localhost-only
   * and unauthenticated (see the known limitations in CLAUDE.md).
   *
   * @param context gRPC server context (unused)
   * @param reader Source of the snapshot chunks
   * @param reply Response indicating whether the snapshot was installed
   * @return gRPC status, always OK - see above
   */
  grpc::Status
  RestoreSnapshot(grpc::ServerContext *context,
                  grpc::ServerReader<consensus::SnapshotChunk> *reader,
                  consensus::RestoreResponse *reply) override {
    std::string payload;
    consensus::SnapshotChunk chunk;
    while (reader->Read(&chunk)) {
      // chunk.data() is the protobuf `data` field; appending it by pointer and
      // length (the same idiom Apply() uses on Command.data) keeps NUL bytes
      // inside a chunk intact.
      payload.append(chunk.data().data(), chunk.data().size());
    }

    const std::optional<std::string> refusal =
        snapshot::restore_from_payload(store_, payload);
    if (refusal.has_value()) {
      reply->set_success(false);
      reply->set_error(*refusal);
      return grpc::Status::OK;
    }

    reply->set_success(true);
    return grpc::Status::OK;
  }

private:
  IKVStore &store_;
};

/**
 * @brief Wrapper class for managing the gRPC StateMachine server.
 *
 * Provides lifecycle management (start, stop, wait) for the
 * gRPC server hosting the StateMachine service.
 */
class StateMachineServer {
public:
  /**
   * @brief Construct the server with a bound address and store.
   * @param address The address to listen on (e.g., "0.0.0.0:50051")
   * @param store Reference to the key-value store
   */
  StateMachineServer(const std::string &address, IKVStore &store)
      : address_(address), service_(store) {}

  /**
   * @brief Start the gRPC server.
   *
   * This is non-blocking. Call wait() to block until shutdown.
   *
   * Registration is per-service, not per-method, so GetSnapshot and
   * RestoreSnapshot are served the moment StateMachineService overrides them -
   * there is nothing to add here for a new RPC.
   */
  void start() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);

    server_ = builder.BuildAndStart();
    log::info(log::kComponentStateMachine, "gRPC listening",
              {log::field("address", address_)});
  }

  /**
   * @brief Block until the server shuts down.
   */
  void wait() {
    if (server_) {
      server_->Wait();
    }
  }

  /**
   * @brief Initiate graceful shutdown.
   */
  void shutdown() {
    if (server_) {
      server_->Shutdown();
    }
  }

private:
  std::string address_;
  StateMachineService service_;
  std::unique_ptr<grpc::Server> server_;
};

} // namespace kvdb
