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

#include <iostream>
#include <memory>
#include <thread>

#include "config/config.hpp"
#include "network/http_server.hpp"
#include "raft/raft_client.hpp"
#include "raft/state_machine.hpp"
#include "storage/kv_store.hpp"

using namespace kvdb;

int main(int argc, char *argv[]) {
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
    PersistentKVStore store(config.db_file);

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
    HttpServer http_server(config.http_port, std::move(handler));
    http_server.run();

    return 0;

  } catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  }
}
