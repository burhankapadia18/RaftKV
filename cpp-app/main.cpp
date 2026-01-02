#include <algorithm>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

// 1. Include MsgPack for zero-copy deserialization in StateMachine
#include <msgpack.hpp>

#include "consensus.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using consensus::ApplyResponse;
using consensus::Command;
using consensus::ProposeResponse;
using consensus::RaftNode;
using consensus::StateMachine;

// Global config
std::string DB_FILE = "kv.db";
std::string MY_GRPC_PORT = "50051";
std::string SIDECAR_PORT = "50052";
int HTTP_PORT = 8080;

std::unordered_map<std::string, std::string> store;
std::mutex db_mutex;

// Define the Command Structure matching your Python Dict
struct KVCommand {
  std::string op;
  std::string key;
  std::string value;
  // Maps to {'op': '...', 'key': '...', 'value': '...'}
  MSGPACK_DEFINE_MAP(op, key, value);
};

void save_db() {
  std::ofstream f(DB_FILE, std::ios::trunc);
  for (auto &kv : store)
    f << kv.first << "=" << kv.second << "\n";
}

void load_db() {
  std::ifstream f(DB_FILE);
  if (!f.is_open())
    return;
  std::string line;
  while (std::getline(f, line)) {
    size_t eq = line.find('=');
    if (eq != std::string::npos) {
      store[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
}

class StateMachineImpl final : public StateMachine::Service {

  Status Apply(ServerContext *context, const Command *request,
               ApplyResponse *reply) override {
    std::lock_guard<std::mutex> lock(db_mutex);

    // Deserialize MsgPack directly from Raft Log
    try {
      KVCommand cmd;
      // Unpack the binary data directly into the struct
      msgpack::object_handle oh =
          msgpack::unpack(request->data().data(), request->data().size());
      msgpack::object obj = oh.get();
      obj.convert(cmd);

      std::cout << "[C++] Applied (MsgPack): " << cmd.op << " " << cmd.key
                << std::endl;

      if (cmd.op == "SET") {
        store[cmd.key] = cmd.value;
      } else if (cmd.op == "DELETE") {
        store.erase(cmd.key);
      }
      save_db();
      reply->set_success(true);
    } catch (const std::exception &e) {
      std::cerr << "[C++] MsgPack Deserialize Error: " << e.what() << std::endl;
      return Status::CANCELLED;
    }

    return Status::OK;
  }
};

class RaftClient {
  std::unique_ptr<RaftNode::Stub> stub_;

public:
  RaftClient(std::shared_ptr<Channel> channel)
      : stub_(RaftNode::NewStub(channel)) {}

  bool Propose(const std::string &raw_payload) {
    Command cmd;
    cmd.set_data(raw_payload);

    ProposeResponse reply;
    ClientContext context;
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(5);
    context.set_deadline(deadline);

    Status status = stub_->Propose(&context, cmd, &reply);
    return status.ok() && reply.success();
  }
};

// --- HTTP Server ---

class HttpServer {
  int server_fd;
  RaftClient &raft_client;

public:
  HttpServer(RaftClient &rc) : raft_client(rc) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(HTTP_PORT);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 3);
  }

  // Helper to parse query
  std::map<std::string, std::string> parse_query(const std::string &q) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start < q.size()) {
      size_t eq = q.find('=', start);
      if (eq == std::string::npos)
        break;
      size_t amp = q.find('&', eq);
      std::string key = q.substr(start, eq - start);
      std::string val = q.substr(
          eq + 1, (amp == std::string::npos ? q.size() : amp) - (eq + 1));
      out[key] = val;
      start = (amp == std::string::npos) ? std::string::npos : amp + 1;
    }
    return out;
  }

  void handle_client(int new_socket) {
    std::vector<char> buffer(4096);
    int bytes_received = recv(new_socket, buffer.data(), buffer.size(), 0);
    if (bytes_received <= 0) {
      close(new_socket);
      return;
    }

    std::string raw_request(buffer.data(), bytes_received);

    // 1. Split Headers and Body
    size_t header_end = raw_request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      close(new_socket);
      return;
    }

    std::string headers = raw_request.substr(0, header_end);
    std::string body = raw_request.substr(header_end + 4);

    // 2. Parse Method and Full Path
    std::istringstream iss(headers);
    std::string method, full_path;
    iss >> method >> full_path;

    // 3. Separate Path from Query String (The Fix)
    std::string path = full_path;
    std::string query_str;
    size_t q_pos = full_path.find('?');
    if (q_pos != std::string::npos) {
      path = full_path.substr(0, q_pos);       // "/get"
      query_str = full_path.substr(q_pos + 1); // "key=user_123"
    }

    // 4. Parse Content-Length and Type
    int content_length = 0;
    bool is_msgpack = false;
    std::string line;
    // Reset stream to parse headers line by line (skipping first line)
    iss.seekg(0);
    std::getline(iss, line);

    while (std::getline(iss, line)) {
      std::transform(line.begin(), line.end(), line.begin(), ::tolower);
      if (line.find("content-length:") != std::string::npos) {
        size_t colon = line.find(':');
        content_length = std::stoi(line.substr(colon + 1));
      }
      if (line.find("content-type:") != std::string::npos &&
          line.find("application/msgpack") != std::string::npos) {
        is_msgpack = true;
      }
    }

    // Read remaining body if needed
    while (body.size() < (size_t)content_length) {
      int n = recv(new_socket, buffer.data(), buffer.size(), 0);
      if (n <= 0)
        break;
      body.append(buffer.data(), n);
    }

    std::string resp_body;

    // 5. Routing Logic
    if (method == "POST" && path == "/insert-val" && is_msgpack) {
      // INSERT key-val Path
      bool success = raft_client.Propose(body);
      resp_body = success ? "ok" : "error";
    } else if (method == "GET" && path == "/get-val") {
      // GET key Path
      auto params = parse_query(query_str); // Use the separated query string
      std::lock_guard<std::mutex> lock(db_mutex);

      // Return value or empty string (not 404) if key missing
      if (store.count(params["key"])) {
        resp_body = store[params["key"]];
      } else {
        resp_body = "Key Not Found";
      }
    } else {
      resp_body = "404 Not Found";
    }

    std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(resp_body.size()) + "\r\n\r\n" +
                       resp_body;
    send(new_socket, resp.c_str(), resp.size(), 0);
    close(new_socket);
  }

  void run() {
    std::cout << "HTTP Server running on port " << HTTP_PORT << "..."
              << std::endl;
    while (true) {
      int new_socket = accept(server_fd, NULL, NULL);
      if (new_socket >= 0)
        handle_client(new_socket);
    }
  }
};

void RunGrpcServer() {
  std::string server_address("0.0.0.0:" + MY_GRPC_PORT);
  StateMachineImpl service;
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "gRPC StateMachine listening on " << server_address << std::endl;
  server->Wait();
}

int main(int argc, char *argv[]) {
  if (argc > 1)
    HTTP_PORT = std::stoi(argv[1]);
  if (argc > 2)
    MY_GRPC_PORT = argv[2];
  if (argc > 3)
    SIDECAR_PORT = argv[3];
  if (argc > 4)
    DB_FILE = argv[4];

  std::cout << "Starting Node: HTTP=" << HTTP_PORT << " gRPC=" << MY_GRPC_PORT
            << " Sidecar=" << SIDECAR_PORT << " DB=" << DB_FILE << std::endl;

  load_db();

  std::thread grpc_thread(RunGrpcServer);
  grpc_thread.detach();

  RaftClient raft_client(grpc::CreateChannel(
      "localhost:" + SIDECAR_PORT, grpc::InsecureChannelCredentials()));

  HttpServer http(raft_client);
  http.run();

  return 0;
}