cmake_minimum_required(VERSION 3.16)
project(minikv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINIKV_BUILD_TESTS "Build unit tests" ON)
option(MINIKV_BUILD_TOOLS "Build CLI tools" ON)

add_library(minikv
  src/status.cpp
  src/slice.cpp
  src/env_posix.cpp
  src/engine/coding.cpp
  src/engine/crc32c.cpp
  src/engine/arena.cpp
  src/engine/dbformat.cpp
  src/engine/memtable.cpp
  src/engine/wal.cpp
  src/engine/filename.cpp
  src/engine/table.cpp
  src/engine/version_edit.cpp
  src/engine/compaction.cpp
  src/engine/db_impl.cpp
  src/engine/write_batch.cpp
  src/rpc/codec.cpp
  src/rpc/client.cpp
  src/rpc/server.cpp
  src/raft/node.cpp
  src/raft/tcp_transport.cpp
  src/raft/storage.cpp
  src/raft/state_machine.cpp
  src/cluster/shard_cluster.cpp
  src/cluster/cluster_client.cpp
  src/cluster/kv_rpc.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(minikv PUBLIC Threads::Threads)

target_include_directories(minikv
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(minikv PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Debug>:-g -O0>
)

if(MINIKV_BUILD_TOOLS)
  add_executable(minikv_cli tools/minikv_cli.cpp)
  target_link_libraries(minikv_cli PRIVATE minikv)

  add_executable(minikv_bench bench/minikv_bench.cpp)
  target_link_libraries(minikv_bench PRIVATE minikv)
  target_include_directories(minikv_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_executable(minikv_wal_dump tools/minikv_wal_dump.cpp)
  target_link_libraries(minikv_wal_dump PRIVATE minikv)
  target_include_directories(minikv_wal_dump PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_executable(minikv_cluster_cli tools/minikv_cluster_cli.cpp)
  target_link_libraries(minikv_cluster_cli PRIVATE minikv)
  target_include_directories(minikv_cluster_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_executable(minikv_server tools/minikv_server.cpp)
  target_link_libraries(minikv_server PRIVATE minikv)
  target_include_directories(minikv_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_executable(minikv_remote_cli tools/minikv_remote_cli.cpp)
  target_link_libraries(minikv_remote_cli PRIVATE minikv)
  target_include_directories(minikv_remote_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_executable(minikv_node tools/minikv_node.cpp)
  target_link_libraries(minikv_node PRIVATE minikv)
  target_include_directories(minikv_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_executable(minikv_cluster_bench bench/minikv_cluster_bench.cpp)
  target_link_libraries(minikv_cluster_bench PRIVATE minikv)
  target_include_directories(minikv_cluster_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

if(MINIKV_BUILD_TESTS)
  enable_testing()
  function(minikv_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE minikv)
    target_include_directories(${name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  endfunction()

  minikv_add_test(test_skiplist tests/engine/test_skiplist.cpp)
  minikv_add_test(test_memtable tests/engine/test_memtable.cpp)
  minikv_add_test(test_wal tests/engine/test_wal.cpp)
  minikv_add_test(test_db tests/engine/test_db.cpp)
  minikv_add_test(test_sstable tests/engine/test_sstable.cpp)
  minikv_add_test(test_flush tests/engine/test_flush.cpp)
  minikv_add_test(test_compaction tests/engine/test_compaction.cpp)
  minikv_add_test(test_engine_e2e tests/engine/test_engine_e2e.cpp)
  minikv_add_test(test_wal_dump tests/engine/test_wal_dump.cpp)
  minikv_add_test(test_concurrent tests/engine/test_concurrent.cpp)
  minikv_add_test(test_crash_recovery tests/engine/test_crash_recovery.cpp)
  minikv_add_test(test_rpc tests/rpc/test_rpc.cpp)
  minikv_add_test(test_raft_election tests/raft/test_raft_election.cpp)
  minikv_add_test(test_raft_replicate tests/raft/test_raft_replicate.cpp)
  minikv_add_test(test_raft_persist tests/raft/test_raft_persist.cpp)
  minikv_add_test(test_raft_tcp tests/raft/test_raft_tcp.cpp)
  minikv_add_test(test_raft_snapshot tests/raft/test_raft_snapshot.cpp)
  minikv_add_test(test_raft_membership tests/raft/test_raft_membership.cpp)
  minikv_add_test(test_shard tests/cluster/test_shard.cpp)
  minikv_add_test(test_cluster_client tests/cluster/test_cluster_client.cpp)
  minikv_add_test(test_failover tests/cluster/test_failover.cpp)
  minikv_add_test(test_cluster_e2e tests/cluster/test_cluster_e2e.cpp)
  minikv_add_test(test_cluster_rpc tests/cluster/test_cluster_rpc.cpp)
endif()
