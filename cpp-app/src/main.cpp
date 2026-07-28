/**
 * @file main.cpp
 * @brief Entry point for the KVDB Raft Node.
 *
 * This file contains only the application bootstrap logic.
 * All domain logic is separated into dedicated modules:
 * - config/     : Application configuration
 * - storage/    : Key-value store implementation
 * - raft/       : Raft consensus client and state machine
 * - network/    : HTTP server and request handling
 * - commands/   : Command structures for operations
 */

#include <atomic>
#include <cerrno>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <thread>
#include <unistd.h>

#include "config/config.hpp"
#include "network/http_server.hpp"
#include "raft/raft_client.hpp"
#include "raft/state_machine.hpp"
#include "storage/kv_store.hpp"

using namespace kvdb;

namespace {

/**
 * @brief Block SIGTERM/SIGINT in this thread and every thread it later spawns.
 *
 * Must run before any other thread exists, because a thread inherits the signal
 * mask of its creator. Called first thing in main for that reason.
 *
 * @return The mask that was blocked, for the waiter thread to sigwait() on.
 */
sigset_t block_shutdown_signals() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);
  return mask;
}

/**
 * @brief Run @p on_signal on a dedicated thread when a shutdown signal arrives.
 *
 * Deliberately sigwait() on a dedicated thread rather than a signal handler
 * (R5.9). A handler may only call async-signal-safe functions, which rules out
 * everything the shutdown actually needs — joining the worker pool, gRPC's
 * Shutdown(), iostreams — and a first attempt that called them anyway produced
 * a process that died on SIGTERM without executing a single line of the
 * handler.
 *
 * With the signal blocked everywhere, it stays pending until this thread
 * accepts it, and the work then happens in ORDINARY context where locks and
 * streams are fine. This is the standard POSIX-threads pattern and it removes
 * the whole class of problem rather than tiptoeing around it.
 *
 * Detached: it outlives nothing, and main is already blocked in run().
 */
void spawn_signal_waiter(sigset_t mask, std::function<void(int)> on_signal) {
  std::thread waiter([mask, on_signal = std::move(on_signal)]() {
    int signal_number = 0;
    // EINTR is the only expected failure; anything else means the mask is
    // wrong, and retrying forever would spin, so give up and let the process
    // run on.
    while (sigwait(&mask, &signal_number) != 0) {
      if (errno != EINTR) {
        return;
      }
    }
    on_signal(signal_number);
  });
  waiter.detach();
}

} // namespace

int main(int argc, char *argv[]) {
  // Before ANY other thread exists: threads inherit the creator's signal mask,
  // so blocking here is what makes the waiter thread the only place these
  // signals can be delivered.
  const sigset_t shutdown_mask = block_shutdown_signals();

  try {
    // 1. Parse configuration
    Config config = Config::from_args(argc, argv);

    std::cout << "=== KVDB Raft Node ===" << std::endl;
    std::cout << "HTTP Port:    " << config.http_port << std::endl;
    std::cout << "gRPC Port:    " << config.grpc_port << std::endl;
    std::cout << "Sidecar Port: " << config.sidecar_port << std::endl;
    std::cout << "DB File:      " << config.db_file << std::endl;
    std::cout << "======================" << std::endl;

    // 2. Initialize the persistent key-value store
    PersistentKVStore store(config.db_file, config.durability);

    // 3. Start the gRPC StateMachine server in a background thread
    StateMachineServer grpc_server(config.grpc_address(), store);
    std::thread grpc_thread([&grpc_server]() {
      grpc_server.start();
      grpc_server.wait();
    });
    grpc_thread.detach();

    // 4. Create the Raft client for proposing commands
    auto raft_client = GrpcRaftClient::connect(config.sidecar_address());

    // 5. Create and run the HTTP server
    KVHttpHandler handler(*raft_client, store);
    HttpServer http_server(config.http_port, std::move(handler), config.limits);

    // 6. R5.9: hand shutdown to the waiter thread. Everything it touches —
    // the worker pool join, gRPC's Shutdown() — is unsafe in a signal handler
    // and perfectly fine here.
    spawn_signal_waiter(shutdown_mask, [&](int signal_number) {
      std::cout << "[main] received signal " << signal_number
                << ", shutting down gracefully" << std::endl;
      // Unblocks the accept loop so run() returns and main unwinds.
      http_server.request_stop();
      grpc_server.shutdown();
    });

    http_server.run();

    std::cout << "[main] HTTP server stopped; draining workers" << std::endl;
    // The joining half. request_stop() only unblocked accept(); this is what
    // waits for in-flight connections to be answered.
    http_server.stop();
    std::cout << "[main] Shutdown complete" << std::endl;

    return 0;

  } catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  }
}
