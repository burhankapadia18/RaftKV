#pragma once

#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "consensus.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include "../commands/kv_command.hpp"
#include "../storage/kv_store.hpp"

namespace kvdb {

/**
 * @brief gRPC service implementing the Raft StateMachine.
 *
 * This service receives committed log entries from the Raft
 * sidecar and applies them to the local key-value store.
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
        std::cerr << "[StateMachine] Rejected: " << *reason << std::endl;
        reply->set_success(false);
        reply->set_error(*reason);
        return grpc::Status::OK;
      }

      std::cout << "[StateMachine] Applied: " << cmd.op << " " << cmd.key
                << std::endl;

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
      std::cerr << "[StateMachine] Rejected: malformed payload: " << e.what()
                << std::endl;
      reply->set_success(false);
      reply->set_error(std::string("malformed payload: ") + e.what());
      return grpc::Status::OK;
    }
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
   */
  void start() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);

    server_ = builder.BuildAndStart();
    std::cout << "[gRPC] StateMachine listening on " << address_ << std::endl;
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
