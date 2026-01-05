#pragma once

#include <iostream>
#include <memory>
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
   * Deserializes the MsgPack command and applies it to the store.
   *
   * @param context gRPC server context
   * @param request The command containing MsgPack-encoded data
   * @param reply Response indicating success/failure
   * @return gRPC status
   */
  grpc::Status Apply(grpc::ServerContext *context,
                     const consensus::Command *request,
                     consensus::ApplyResponse *reply) override {
    try {
      // Deserialize the command from MsgPack
      KVCommand cmd = KVCommand::from_msgpack(request->data().data(),
                                              request->data().size());

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
        std::cerr << "[StateMachine] Unknown operation: " << cmd.op
                  << std::endl;
        reply->set_success(false);
        return grpc::Status::OK;
      }

      reply->set_success(true);
      return grpc::Status::OK;

    } catch (const std::exception &e) {
      std::cerr << "[StateMachine] Error: " << e.what() << std::endl;
      reply->set_success(false);
      return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
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
