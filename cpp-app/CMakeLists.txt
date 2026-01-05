cmake_minimum_required(VERSION 3.15)
project(KVDB_Raft CXX)

# --- Project Configuration ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- 1. Dependencies ---

find_package(gRPC CONFIG REQUIRED)
find_package(msgpack REQUIRED)

if(APPLE)
    message(STATUS "macOS detected: Using Protobuf CONFIG mode")
    find_package(Protobuf CONFIG REQUIRED)
else()
    message(STATUS "Linux detected: Using Protobuf MODULE mode")
    find_package(Protobuf REQUIRED)
endif()

# --- 2. Find Protobuf Compiler (protoc) ---

if(TARGET protobuf::protoc)
    get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
    if(NOT Protobuf_PROTOC_EXECUTABLE)
        get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_NOCONFIG)
    endif()
endif()

# Fallback: Find program manually
if(NOT Protobuf_PROTOC_EXECUTABLE)
    find_program(Protobuf_PROTOC_EXECUTABLE protoc HINTS /usr/bin /usr/local/bin)
endif()

message(STATUS "Using Protobuf Compiler: ${Protobuf_PROTOC_EXECUTABLE}")

# --- 3. Find gRPC Plugin ---

if(TARGET gRPC::grpc_cpp_plugin)
    get_target_property(gRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin IMPORTED_LOCATION_RELEASE)
    if(NOT gRPC_CPP_PLUGIN_EXECUTABLE)
         get_target_property(gRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin IMPORTED_LOCATION)
    endif()
endif()

# Fallback: Search the system PATH
if(NOT gRPC_CPP_PLUGIN_EXECUTABLE)
    find_program(gRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin HINTS /usr/bin /usr/local/bin)
endif()

message(STATUS "Using gRPC Plugin: ${gRPC_CPP_PLUGIN_EXECUTABLE}")

# --- 4. Generate Protobuf Sources ---

get_filename_component(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto" ABSOLUTE)
set(PROTO_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY ${PROTO_BINARY_DIR})

add_custom_command(
    OUTPUT "${PROTO_BINARY_DIR}/consensus.pb.cc"
           "${PROTO_BINARY_DIR}/consensus.pb.h"
           "${PROTO_BINARY_DIR}/consensus.grpc.pb.cc"
           "${PROTO_BINARY_DIR}/consensus.grpc.pb.h"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --grpc_out "${PROTO_BINARY_DIR}"
         --cpp_out "${PROTO_BINARY_DIR}"
         -I "${PROTO_SRC_DIR}"
         --plugin=protoc-gen-grpc="${gRPC_CPP_PLUGIN_EXECUTABLE}"
         "${PROTO_SRC_DIR}/consensus.proto"
    DEPENDS "${PROTO_SRC_DIR}/consensus.proto"
    COMMENT "Generating gRPC/Protobuf sources for consensus.proto"
)

# Create a library for generated protobuf files
add_library(consensus_proto STATIC
    "${PROTO_BINARY_DIR}/consensus.pb.cc"
    "${PROTO_BINARY_DIR}/consensus.grpc.pb.cc"
)

target_include_directories(consensus_proto PUBLIC
    ${PROTO_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

# Handle Linking Differences
if(TARGET protobuf::libprotobuf)
    set(PROTOBUF_LIB_TARGET protobuf::libprotobuf)
else()
    set(PROTOBUF_LIB_TARGET ${Protobuf_LIBRARIES})
endif()

target_link_libraries(consensus_proto PUBLIC
    ${PROTOBUF_LIB_TARGET}
    gRPC::grpc++
)

# --- 5. Application Source Files ---

set(KVDB_SOURCES
    src/main.cpp
)

set(KVDB_HEADERS
    src/config/config.hpp
    src/commands/kv_command.hpp
    src/storage/kv_store.hpp
    src/raft/raft_client.hpp
    src/raft/state_machine.hpp
    src/network/http_request.hpp
    src/network/http_server.hpp
)

# --- 6. Build Executable ---

add_executable(kvdb_node ${KVDB_SOURCES} ${KVDB_HEADERS})

target_include_directories(kvdb_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROTO_BINARY_DIR}
)

target_link_libraries(kvdb_node PRIVATE
    consensus_proto
    gRPC::grpc++_reflection
    msgpackc
)

# --- 7. Compiler Warnings (Optional but Recommended) ---

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(kvdb_node PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
endif()

# --- 8. Installation (Optional) ---

install(TARGETS kvdb_node DESTINATION bin)