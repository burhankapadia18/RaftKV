#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
 * @brief Outcome of a linearizable read (R4.5).
 *
 * `found` is only meaningful when `ok` is true: a failed read knows nothing
 * about whether the key exists, and conflating "no" with "could not tell" is
 * how a stale answer gets dressed up as a definitive one.
 */
struct ReadResult {
  bool ok = false;
  bool found = false;
  std::string value;
  std::string error;

  [[nodiscard]] static ReadResult hit(std::string v) {
    ReadResult r;
    r.ok = true;
    r.found = true;
    r.value = std::move(v);
    return r;
  }

  [[nodiscard]] static ReadResult miss() {
    ReadResult r;
    r.ok = true;
    r.found = false;
    return r;
  }

  [[nodiscard]] static ReadResult failure(std::string reason) {
    ReadResult r;
    r.ok = false;
    r.error = std::move(reason);
    return r;
  }
};

/** @brief One member of the committed raft configuration. */
struct RaftPeer {
  std::string id;
  std::string address;
  std::string suffrage;
};

/**
 * @brief Outcome of a cluster-status query.
 *
 * @c ok distinguishes "the sidecar answered" from "it did not". The numeric
 * fields are only meaningful when @c ok is true: a zero-valued status renders
 * as a cluster with no peers and no leader, which is indistinguishable from a
 * real loss of quorum, so a failed call must never be dressed up as one.
 *
 * @c partial_error is different: the sidecar answered, but one field inside it
 * could not be read. The rest of the response stands.
 */
struct StatusResult {
  bool ok = false;
  std::string error;

  std::string node_id;
  std::string state;
  std::string leader_id;
  std::string leader_addr;
  std::string partial_error;

  std::uint64_t term = 0;
  std::uint64_t first_log_index = 0;
  std::uint64_t last_log_index = 0;
  std::uint64_t applied_index = 0;
  std::uint64_t commit_index = 0;
  std::uint64_t last_snapshot_index = 0;

  std::vector<RaftPeer> peers;

  [[nodiscard]] static StatusResult failure(std::string reason) {
    StatusResult result;
    result.ok = false;
    result.error = std::move(reason);
    return result;
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

  /**
   * @brief Read a key linearizably via the sidecar (R4.5).
   *
   * The sidecar forwards to the leader when needed and runs Barrier +
   * VerifyLeader there, so a value returned here reflects every write
   * acknowledged before the call. Contrast with reading the local store
   * directly, which is what `consistency=local` does and may be stale.
   */
  virtual ReadResult read(const std::string &key) = 0;

  /**
   * @brief This node's own view of the cluster, for the operator console.
   *
   * Read-only and leader-free: the sidecar answers for itself and does not
   * forward, so a follower's answer is its own state (including its own belief
   * about who leads) rather than the leader's.
   */
  [[nodiscard]] virtual StatusResult status() = 0;
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
      return ProposeResult::failure(grpc_failure_reason(status));
    }

    if (!reply.success()) {
      // An older sidecar (or a new bug) can report failure without a reason;
      // never let that collapse back into an empty error string.
      return ProposeResult::failure(reply.error().empty() ? kUnexplainedFailure
                                                          : reply.error());
    }

    return ProposeResult::ok();
  }

  /**
   * @brief Ask the sidecar for a linearizable read.
   *
   * `forwarded` is left unset: this is the ORIGINAL request entering the
   * cluster, so the sidecar is free to forward it once. Setting it here would
   * disable forwarding entirely and make a follower refuse every linearizable
   * read.
   */
  ReadResult read(const std::string &key) override {
    consensus::ReadRequest request;
    request.set_key(key);

    consensus::ReadResponse reply;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kDefaultTimeout);

    const grpc::Status status = stub_->Read(&context, request, &reply);
    if (!status.ok()) {
      return ReadResult::failure(grpc_failure_reason(status));
    }
    if (!reply.error().empty()) {
      return ReadResult::failure(reply.error());
    }
    if (!reply.found()) {
      return ReadResult::miss();
    }
    return ReadResult::hit(reply.value());
  }

  /**
   * @brief Ask the sidecar for its own view of the cluster.
   *
   * No forwarding and no leader requirement: whichever node is asked answers
   * for itself, which is what makes the console's overview honest.
   */
  [[nodiscard]] StatusResult status() override {
    consensus::StatusRequest request;
    consensus::StatusResponse reply;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kStatusTimeout);

    const grpc::Status grpc_status = stub_->Status(&context, request, &reply);
    if (!grpc_status.ok()) {
      return StatusResult::failure(grpc_failure_reason(grpc_status));
    }

    StatusResult result;
    result.ok = true;
    result.node_id = reply.node_id();
    result.state = reply.state();
    result.leader_id = reply.leader_id();
    result.leader_addr = reply.leader_addr();
    result.partial_error = reply.error();
    result.term = reply.term();
    result.first_log_index = reply.first_log_index();
    result.last_log_index = reply.last_log_index();
    result.applied_index = reply.applied_index();
    result.commit_index = reply.commit_index();
    result.last_snapshot_index = reply.last_snapshot_index();
    result.peers.reserve(static_cast<size_t>(reply.peers_size()));
    for (const consensus::Peer &peer : reply.peers()) {
      result.peers.push_back(
          RaftPeer{peer.id(), peer.address(), peer.suffrage()});
    }
    return result;
  }

private:
  std::unique_ptr<consensus::RaftNode::Stub> stub_;

  // Above the sidecar's proposeTimeout (4s, go-sidecar/internal/rpc/server.go)
  // so that when that timeout DOES apply — it only bounds the raft enqueue, not
  // commit-and-apply — the sidecar wins the race and answers with a structured
  // reason instead of this deadline firing. For a wedged state machine there is
  // no bound on the sidecar side, so DEADLINE_EXCEEDED here is still reachable
  // and ProposeResult carries it as such.
  static constexpr std::chrono::seconds kDefaultTimeout{5};

  /**
   * @brief Deadline for a status query.
   *
   * Shorter than the propose deadline on purpose: this backs an interactive
   * page that polls, so a hung sidecar must surface as an error quickly rather
   * than stacking requests up behind a five-second wait.
   */
  static constexpr std::chrono::seconds kStatusTimeout{2};

  /**
   * @brief Render a failed gRPC status as "<CODE_NAME>: <message>".
   *
   * Shared by propose() and read() so both report a transport failure the same
   * way; the HTTP layer maps either onto a 502 with this text in the body.
   */
  [[nodiscard]] static std::string grpc_failure_reason(const grpc::Status &s) {
    return std::string(status_code_name(s.error_code())) + ": " +
           s.error_message();
  }

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
