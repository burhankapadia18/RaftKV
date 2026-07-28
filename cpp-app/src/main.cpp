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

#include "common/log.hpp"
#include "common/metrics.hpp"
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

    // 2. Initialize the persistent key-value store
    PersistentKVStore store(config.db_file, config.durability);

    // R5.2: configure the JSON logger before anything logs through it.
    log::Logger::configure(config.node_id, log::parse_level(config.log_level));
    log::info(
        log::kComponentMain, "starting",
        {log::field("http_port", static_cast<long long>(config.http_port)),
         log::field("grpc_port", config.grpc_port),
         log::field("sidecar_port", config.sidecar_port),
         log::field("db_file", config.db_file)});

    // R5.4: store gauges, read at SCRAPE time rather than snapshotted, so a
    // dashboard cannot show a stale key count. Registered here because main
    // holds the concrete PersistentKVStore — nothing was added to IKVStore for
    // this.
    metrics::Registry::global().gauge_fn(
        "raftkv_store_keys", "Keys currently held in the local store.",
        [&store]() { return static_cast<double>(store.key_count()); });
    metrics::Registry::global().gauge_fn(
        "raftkv_wal_size_bytes",
        "Current size of the write-ahead log in bytes.",
        [&store]() { return static_cast<double>(store.wal_size_bytes()); });

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
      log::info(log::kComponentMain, "shutdown signal received",
                {log::field("signal", static_cast<long long>(signal_number))});
      // Unblocks the accept loop so run() returns and main unwinds.
      http_server.request_stop();
      grpc_server.shutdown();
    });

    http_server.run();

    log::info(log::kComponentMain, "http stopped, draining workers");
    // The joining half. request_stop() only unblocked accept(); this is what
    // waits for in-flight connections to be answered.
    http_server.stop();
    log::info(log::kComponentMain, "shutdown complete");

    return 0;

  } catch (const std::exception &e) {
    log::error(log::kComponentMain, "fatal error",
               {log::field("error", e.what())});
    return 1;
  }
}
