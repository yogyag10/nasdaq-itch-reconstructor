cmake_minimum_required(VERSION 3.20)
project(itch_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

option(ITCH_SANITIZE "Build with ASan + UBSan" OFF)
option(ITCH_FUZZ "Build the libFuzzer target (needs clang with libFuzzer)" OFF)
option(ITCH_BENCH "Build Google Benchmark micro-benchmarks and the e2e harness" ON)
option(ITCH_TESTS "Build unit tests" ON)

add_library(itch INTERFACE)
target_include_directories(itch INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(itch INTERFACE cxx_std_20)
target_compile_options(itch INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

if(ITCH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(ZLIB REQUIRED)

add_executable(replay src/replay.cpp)
target_link_libraries(replay PRIVATE itch ZLIB::ZLIB)

add_executable(synth_gen tests/synth_gen.cpp)
target_link_libraries(synth_gen PRIVATE itch)

include(FetchContent)

if(ITCH_TESTS)
  enable_testing()
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_executable(itch_tests tests/test_parser.cpp tests/test_book.cpp tests/test_stores.cpp)
  target_link_libraries(itch_tests PRIVATE itch GTest::gtest_main)
  target_include_directories(itch_tests PRIVATE tests)
  include(GoogleTest)
  gtest_discover_tests(itch_tests)
endif()

if(ITCH_BENCH)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.0.tar.gz)
  FetchContent_MakeAvailable(benchmark)
  add_executable(micro_bench bench/micro_bench.cpp)
  target_link_libraries(micro_bench PRIVATE itch benchmark::benchmark)
  add_executable(e2e_bench bench/e2e_bench.cpp)
  target_link_libraries(e2e_bench PRIVATE itch)
  target_include_directories(e2e_bench PRIVATE tests)
endif()

# Fuzz target: real libFuzzer when requested, otherwise a corpus-replay driver
# so the same entry point is exercised on compilers without libFuzzer (Apple clang).
add_executable(parse_fuzz fuzz/parse_fuzz.cpp)
target_link_libraries(parse_fuzz PRIVATE itch)
if(ITCH_FUZZ)
  target_compile_options(parse_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(parse_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
else()
  target_compile_definitions(parse_fuzz PRIVATE ITCH_STANDALONE_FUZZ_DRIVER)
endif()
