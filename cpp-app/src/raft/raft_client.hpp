#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "consensus.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace kvdb {

/**
 * @brief Outcome of a propose attempt.
 *
 * A bare bool cannot distinguish "this node is not the leader" (retry
 * elsewhere, 503) from "the sidecar is unreachable" (502), so every failure
 * carries the reason that produced it. Invariant: @c error is empty if and
 * only if @c success is true.
 */
struct ProposeResult {
  bool success = false;
  std::string error;

  /** @brief The proposal was accepted and committed. */
  [[nodiscard]] static ProposeResult ok() { return ProposeResult{true, ""}; }

  /** @brief The proposal failed for the given reason. */
  [[nodiscard]] static ProposeResult failure(std::string error) {
    return ProposeResult{false, std::move(error)};
  }
};

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
   * @return Success, or failure carrying the reason it failed
   */
  virtual ProposeResult propose(const std::string &payload) = 0;
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
   * Three outcomes are reported distinctly:
   *   - the RPC never completed: "<STATUS_CODE>: <message>", e.g.
   *     "UNAVAILABLE: failed to connect to all addresses" (sidecar down) or
   *     "DEADLINE_EXCEEDED: ..." (the 5s budget ran out);
   *   - the sidecar answered but refused: whatever it put in
   *     ProposeResponse.error, which for a follower is
   *     "not_leader:<leader address>";
   *   - committed: success with an empty error.
   *
   * @param payload The raw command data (MsgPack encoded)
   * @return Success, or failure carrying the reason it failed
   */
  ProposeResult propose(const std::string &payload) override {
    consensus::Command cmd;
    cmd.set_data(payload);

    consensus::ProposeResponse reply;
    grpc::ClientContext context;

    // Set deadline to prevent hanging
    auto deadline = std::chrono::system_clock::now() + kDefaultTimeout;
    context.set_deadline(deadline);

    grpc::Status status = stub_->Propose(&context, cmd, &reply);
    if (!status.ok()) {
      return ProposeResult::failure(
          std::string(status_code_name(status.error_code())) + ": " +
          status.error_message());
    }

    if (!reply.success()) {
      // An older sidecar (or a new bug) can report failure without a reason;
      // never let that collapse back into an empty error string.
      return ProposeResult::failure(reply.error().empty() ? kUnexplainedFailure
                                                          : reply.error());
    }

    return ProposeResult::ok();
  }

private:
  std::unique_ptr<consensus::RaftNode::Stub> stub_;

  // Must stay ABOVE the sidecar's proposeTimeout (4s, in
  // go-sidecar/internal/rpc/server.go) so a slow commit comes back as the
  // sidecar's structured reason rather than as a bare DEADLINE_EXCEEDED here.
  static constexpr std::chrono::seconds kDefaultTimeout{5};

  /** @brief Stand-in when the sidecar reports failure with no reason. */
  static constexpr const char *kUnexplainedFailure =
      "propose rejected by the raft sidecar without a reason";

  /**
   * @brief Canonical name of a gRPC status code.
   *
   * grpc::Status carries only the numeric code, and "14" in an HTTP error body
   * helps nobody.
   */
  [[nodiscard]] static const char *status_code_name(grpc::StatusCode code) {
    switch (code) {
    case grpc::StatusCode::OK:
      return "OK";
    case grpc::StatusCode::CANCELLED:
      return "CANCELLED";
    case grpc::StatusCode::UNKNOWN:
      return "UNKNOWN";
    case grpc::StatusCode::INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return "DEADLINE_EXCEEDED";
    case grpc::StatusCode::NOT_FOUND:
      return "NOT_FOUND";
    case grpc::StatusCode::ALREADY_EXISTS:
      return "ALREADY_EXISTS";
    case grpc::StatusCode::PERMISSION_DENIED:
      return "PERMISSION_DENIED";
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return "RESOURCE_EXHAUSTED";
    case grpc::StatusCode::FAILED_PRECONDITION:
      return "FAILED_PRECONDITION";
    case grpc::StatusCode::ABORTED:
      return "ABORTED";
    case grpc::StatusCode::OUT_OF_RANGE:
      return "OUT_OF_RANGE";
    case grpc::StatusCode::UNIMPLEMENTED:
      return "UNIMPLEMENTED";
    case grpc::StatusCode::INTERNAL:
      return "INTERNAL";
    case grpc::StatusCode::UNAVAILABLE:
      return "UNAVAILABLE";
    case grpc::StatusCode::DATA_LOSS:
      return "DATA_LOSS";
    case grpc::StatusCode::UNAUTHENTICATED:
      return "UNAUTHENTICATED";
    default:
      return "UNRECOGNIZED_STATUS";
    }
  }
};

} // namespace kvdb
