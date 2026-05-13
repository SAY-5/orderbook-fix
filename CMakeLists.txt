cmake_minimum_required(VERSION 3.20)
project(orderbook_fix CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OBFIX_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(OBFIX_TSAN "Build with ThreadSanitizer" OFF)
option(OBFIX_FUZZ "Build libFuzzer harnesses (clang only)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

if(OBFIX_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
if(OBFIX_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

# Core library
add_library(obfix_core STATIC
  src/core/order_book.cpp
  src/core/matcher.cpp
  src/fix/parser.cpp
  src/fix/serializer.cpp
  src/fix/session.cpp
  src/net/tcp_server.cpp
)
target_include_directories(obfix_core PUBLIC src)

# Server binary
add_executable(obfix_server src/main.cpp)
target_link_libraries(obfix_server PRIVATE obfix_core)

# GoogleTest via FetchContent
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Unit tests: one executable per file keeps failures isolated.
foreach(t fix_parser_test fix_session_test fix_session_decision_table_test fix_session_resend_test fix_session_heartbeat_test pro_rata_test matcher_test order_book_test price_level_test property_test)
  add_executable(${t} tests/unit/${t}.cpp)
  target_link_libraries(${t} PRIVATE obfix_core gtest gtest_main)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Integration tests
add_executable(session_e2e_test tests/integration/session_e2e_test.cpp)
target_link_libraries(session_e2e_test PRIVATE obfix_core gtest gtest_main)
add_test(NAME session_e2e_test COMMAND session_e2e_test)

add_executable(concurrent_sessions_stress_test tests/integration/concurrent_sessions_stress_test.cpp)
target_link_libraries(concurrent_sessions_stress_test PRIVATE obfix_core gtest gtest_main)
add_test(NAME concurrent_sessions_stress_test COMMAND concurrent_sessions_stress_test)
set_tests_properties(concurrent_sessions_stress_test PROPERTIES TIMEOUT 240)

# Bench
add_executable(replay_bench bench/replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE obfix_core)

add_executable(cacheline_study bench/cacheline_study.cpp)
target_link_libraries(cacheline_study PRIVATE obfix_core)

# Fuzzers (clang only, opt-in)
if(OBFIX_FUZZ)
  add_executable(fix_parser_fuzz tests/fuzz/fix_parser_fuzz.cpp)
  target_link_libraries(fix_parser_fuzz PRIVATE obfix_core)
  target_compile_options(fix_parser_fuzz PRIVATE -fsanitize=fuzzer,address)
  target_link_options(fix_parser_fuzz PRIVATE -fsanitize=fuzzer,address)
endif()
