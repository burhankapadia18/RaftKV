cmake_minimum_required(VERSION 3.15)
project(KVDB_Raft)

set(CMAKE_CXX_STANDARD 17)

# --- 1. Dependencies ---

find_package(gRPC CONFIG REQUIRED)

# --- NEW: Find MsgPack ---
# If this fails inside Docker, ensure `libmsgpack-dev` is installed.
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

# --- 3. Find gRPC Plugin (PATCHED) ---

# Attempt 1: Try to get it from the CMake Target (common in Config mode)
if(TARGET gRPC::grpc_cpp_plugin)
    get_target_property(gRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin IMPORTED_LOCATION_RELEASE)
    if(NOT gRPC_CPP_PLUGIN_EXECUTABLE)
         get_target_property(gRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin IMPORTED_LOCATION)
    endif()
endif()

# Attempt 2: If Target failed, search the system PATH (Fixes Docker/Debian issue)
if(NOT gRPC_CPP_PLUGIN_EXECUTABLE)
    find_program(gRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin HINTS /usr/bin /usr/local/bin)
endif()

message(STATUS "Using gRPC Plugin: ${gRPC_CPP_PLUGIN_EXECUTABLE}")

# --- 4. Generate Sources ---

get_filename_component(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto" ABSOLUTE)
set(PROTO_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
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
)

include_directories("${PROTO_BINARY_DIR}")
include_directories(${Protobuf_INCLUDE_DIRS}) 

# --- 5. Build Executable ---

add_executable(kvdb_node main.cpp
    "${PROTO_BINARY_DIR}/consensus.pb.cc"
    "${PROTO_BINARY_DIR}/consensus.grpc.pb.cc"
)

# Handle Linking Differences
if(TARGET protobuf::libprotobuf)
    set(PROTOBUF_LIB_TARGET protobuf::libprotobuf)
else()
    set(PROTOBUF_LIB_TARGET ${Protobuf_LIBRARIES})
endif()

target_link_libraries(kvdb_node
    ${PROTOBUF_LIB_TARGET}
    gRPC::grpc++
    gRPC::grpc++_reflection
    # --- NEW: Link MsgPack ---
    msgpackc
)