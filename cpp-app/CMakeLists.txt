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
    src/common/log.hpp
    src/common/metrics.hpp
    src/config/config.hpp
    src/commands/kv_command.hpp
    src/storage/format.hpp
    src/storage/atomic_file.hpp
    src/storage/wal.hpp
    src/storage/kv_store.hpp
    src/raft/raft_client.hpp
    src/raft/state_machine.hpp
    src/network/thread_pool.hpp
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

# --- 9. Unit Tests (opt-in) ---
#
# Everything below is guarded by KVDB_BUILD_TESTS so that the default
# configuration (and therefore the Docker build) is completely unaffected.
#   cmake -DKVDB_BUILD_TESTS=ON .. && make -j4 && ctest --output-on-failure

option(KVDB_BUILD_TESTS "Build unit tests" OFF)

if(KVDB_BUILD_TESTS)
    # Prefer an already-installed GoogleTest (Debian: libgtest-dev) so CI can run
    # offline; only fall back to fetching the sources when nothing is installed.
    find_package(GTest QUIET)

    if(TARGET GTest::gtest_main)
        message(STATUS "Tests: using system GoogleTest")
    else()
        message(STATUS "Tests: GoogleTest not found, fetching 1.15.2")
        include(FetchContent)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            # The 1.15.2 release is tagged "v1.15.2" (the "release-" prefix was
            # dropped after 1.12.1).
            GIT_TAG v1.15.2
            # A tag is a mutable ref, so without GIT_SHALLOW every build adds a
            # full-history clone plus an update fetch. This branch is what the
            # cpp-sanitizers CI job takes (it omits libgtest-dev on purpose), so
            # keep it cheap.
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()
    include(GoogleTest)

    # Listed explicitly on purpose - no globbing, so adding a file is a visible
    # change to this list.
    add_executable(kvdb_tests
        tests/kv_command_test.cpp
        tests/kv_store_test.cpp
        tests/http_request_test.cpp
        tests/state_machine_test.cpp
        tests/http_handler_test.cpp
        tests/format_test.cpp
        tests/atomic_file_test.cpp
        tests/wal_test.cpp
        tests/thread_pool_test.cpp
        tests/metrics_test.cpp
    )

    target_include_directories(kvdb_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PROTO_BINARY_DIR}
    )

    # Phase 1 links consensus_proto: StateMachineService::Apply and
    # KVHttpHandler are now under test, and both sit on the generated
    # consensus.pb.h / grpc.pb.h types. The pure units (KVCommand,
    # PersistentKVStore, HttpRequestParser) still need nothing but msgpack.
    target_link_libraries(kvdb_tests PRIVATE
        GTest::gtest_main
        consensus_proto
        msgpackc
    )

    # <filesystem> (used by the store tests for temp files) needs an explicit
    # library only on GCC older than 9.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(kvdb_tests PRIVATE stdc++fs)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(kvdb_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
        )
    endif()

    # DISCOVERY_TIMEOUT defaults to 5s and discovery runs the freshly built
    # binary at build time. Under ASan+UBSan on a loaded CI runner that can
    # overrun, and the failure surfaces as a *build* error rather than a test
    # failure, which is badly misleading. 120s costs nothing when it passes.
    gtest_discover_tests(kvdb_tests DISCOVERY_TIMEOUT 120)
endif()