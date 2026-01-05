#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "consensus.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace kvdb {

/**
 * @brief Abstract interface for Raft consensus client.
 *
 * Allows for easy mocking in unit tests and potential
 * alternative implementations.
 */
class IRaftClient {
public:
  virtual ~IRaftClient() = default;

  /**
   * @brief Propose a command to the Raft cluster.
   *
   * @param payload The raw (MsgPack-encoded) command data
   * @return true if the proposal was accepted and committed
   */
  virtual bool propose(const std::string &payload) = 0;
};

/**
 * @brief gRPC-based Raft client implementation.
 *
 * Communicates with the Go sidecar to propose commands
 * to the Raft cluster for consensus.
 */
class GrpcRaftClient : public IRaftClient {
public:
  /**
   * @brief Construct a Raft client with a gRPC channel.
   * @param channel Shared gRPC channel to the Raft sidecar
   */
  explicit GrpcRaftClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(consensus::RaftNode::NewStub(channel)) {}

  /**
   * @brief Create a Raft client connected to the specified address.
   * @param address The sidecar address (e.g., "localhost:50052")
   */
  static std::unique_ptr<GrpcRaftClient> connect(const std::string &address) {
    auto channel =
        grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    return std::make_unique<GrpcRaftClient>(channel);
  }

  /**
   * @brief Propose a command to the Raft cluster.
   *
   * Includes a 5-second timeout to prevent indefinite blocking.
   *
   * @param payload The raw command data (MsgPack encoded)
   * @return true if proposal succeeded and was committed
   */
  bool propose(const std::string &payload) override {
    consensus::Command cmd;
    cmd.set_data(payload);

    consensus::ProposeResponse reply;
    grpc::ClientContext context;

    // Set deadline to prevent hanging
    auto deadline = std::chrono::system_clock::now() + kDefaultTimeout;
    context.set_deadline(deadline);

    grpc::Status status = stub_->Propose(&context, cmd, &reply);
    return status.ok() && reply.success();
  }

private:
  std::unique_ptr<consensus::RaftNode::Stub> stub_;

  static constexpr std::chrono::seconds kDefaultTimeout{5};
};

} // namespace kvdb
