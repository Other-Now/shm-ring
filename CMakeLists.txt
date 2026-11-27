cmake_minimum_required(VERSION 3.20)
project(shm_ring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SHMRING_BENCH "Build Google Benchmark microbenchmarks (fetches google/benchmark)" ON)
option(SHMRING_NATIVE "Compile with -march=native" ON)

# Header-only library.
add_library(shmring INTERFACE)
target_include_directories(shmring INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(shmring INTERFACE -Wall -Wextra -Wpedantic)
if(SHMRING_NATIVE)
  target_compile_options(shmring INTERFACE -march=native)
endif()
find_package(Threads REQUIRED)
target_link_libraries(shmring INTERFACE Threads::Threads rt)

# Code reused from tick2trade and algo-risk-gateway (same author), see third_party/README.md.
add_library(vendored INTERFACE)
target_include_directories(vendored SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_executable(ipc_latency bench/ipc_latency.cpp)
target_link_libraries(ipc_latency PRIVATE shmring)

add_executable(ring_throughput bench/ring_throughput.cpp)
target_link_libraries(ring_throughput PRIVATE shmring)

add_executable(hiccup bench/hiccup.cpp)
target_link_libraries(hiccup PRIVATE shmring)

add_executable(t2t_split apps/t2t_split.cpp)
target_link_libraries(t2t_split PRIVATE shmring vendored)
target_link_options(t2t_split PRIVATE -rdynamic)  # symbol names in SHMRING_ALLOC_TRACE backtraces
# Same app with the risk gate's original heap-allocating containers, for the
# before/after allocation measurement (results/allocations.md).
add_executable(t2t_split_heapgate apps/t2t_split.cpp)
target_link_libraries(t2t_split_heapgate PRIVATE shmring vendored)
target_compile_definitions(t2t_split_heapgate PRIVATE SHMRING_GATE_HEAP)

enable_testing()
add_executable(shmring_tests tests/test_shmring.cpp)
target_link_libraries(shmring_tests PRIVATE shmring vendored)
add_test(NAME unit COMMAND shmring_tests)
add_test(NAME t2t_parity
         COMMAND ${CMAKE_COMMAND} -E env T2T=$<TARGET_FILE:t2t_split>
                 bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/parity.sh ${CMAKE_CURRENT_SOURCE_DIR}/data/sample.NASDAQ_ITCH50)

if(SHMRING_BENCH)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(benchmark)
  add_executable(micro bench/micro.cpp)
  target_link_libraries(micro PRIVATE shmring vendored benchmark::benchmark)
endif()
