cmake_minimum_required(VERSION 3.24)
project(harbor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HARBOR_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(HARBOR_TSAN "Build with ThreadSanitizer" OFF)
if(HARBOR_SANITIZE AND HARBOR_TSAN)
    message(FATAL_ERROR "HARBOR_SANITIZE and HARBOR_TSAN cannot be combined")
endif()

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Warnings and sanitizers apply to Harbor's own targets only, never to googletest.
function(harbor_configure_target target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(HARBOR_SANITIZE)
        # -fno-sanitize-recover makes any UBSan report abort the test instead
        # of printing and exiting 0.
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    if(HARBOR_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
    if(HARBOR_SANITIZE OR HARBOR_TSAN)
        target_compile_definitions(${target} PRIVATE HARBOR_SANITIZED)
    endif()
endfunction()

add_library(harbor STATIC
    src/reference_book.cpp
    src/fast_book.cpp
    src/crc32c.cpp
    src/command.cpp
    src/file_io.cpp
    src/wal.cpp
    src/snapshot.cpp
    src/protocol.cpp
    src/feed.cpp
    src/net.cpp
    src/poller.cpp
    src/engine_loop.cpp
    src/gateway.cpp
    src/server.cpp
    src/feed_listener.cpp
    src/wakeup.cpp
)
target_include_directories(harbor PUBLIC include)
target_link_libraries(harbor PUBLIC Threads::Threads)
harbor_configure_target(harbor)

enable_testing()
add_executable(harbor_tests
    tests/test_types.cpp
    tests/test_flat_hash_map.cpp
    tests/test_book_behavior.cpp
    tests/test_differential.cpp
    tests/test_crc32c.cpp
    tests/test_command_codec.cpp
    tests/test_wal.cpp
    tests/test_snapshot.cpp
    tests/test_engine.cpp
    tests/test_recovery.cpp
    tests/test_crash.cpp
    tests/test_spsc_ring.cpp
    tests/test_protocol.cpp
    tests/test_feed.cpp
    tests/test_gateway.cpp
    tests/test_end_to_end.cpp
)
target_link_libraries(harbor_tests PRIVATE harbor GTest::gtest_main)
harbor_configure_target(harbor_tests)

include(GoogleTest)
gtest_discover_tests(harbor_tests)

# Benchmarks: build with -DCMAKE_BUILD_TYPE=Release for meaningful numbers.
# Skipped under ThreadSanitizer: the allocation counter replaces operator new,
# which TSan also defines, and timings under TSan are meaningless anyway.
if(NOT HARBOR_TSAN)
    add_executable(harbor_bench bench/bench_books.cpp bench/alloc_counter.cpp)
    target_include_directories(harbor_bench PRIVATE tests)
    target_link_libraries(harbor_bench PRIVATE harbor)
    harbor_configure_target(harbor_bench)
    add_executable(harbor_bench_network bench/bench_network.cpp)
    target_include_directories(harbor_bench_network PRIVATE tests)
    target_link_libraries(harbor_bench_network PRIVATE harbor)
    harbor_configure_target(harbor_bench_network)
    add_executable(harbor_bench_engine bench/bench_engine.cpp bench/alloc_counter.cpp)
    target_include_directories(harbor_bench_engine PRIVATE tests)
    target_link_libraries(harbor_bench_engine PRIVATE harbor)
    harbor_configure_target(harbor_bench_engine)
endif()

# Demo tools: harbor_server, harbor_client, harbor_listen
foreach(tool harbor_server harbor_client harbor_listen)
    add_executable(${tool} tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE harbor)
    harbor_configure_target(${tool})
endforeach()

# Crash campaign: ./harbor_crash_matrix [trials] [seed]
add_executable(harbor_crash_matrix tools/crash_matrix.cpp)
target_include_directories(harbor_crash_matrix PRIVATE tests)
target_link_libraries(harbor_crash_matrix PRIVATE harbor)
harbor_configure_target(harbor_crash_matrix)

foreach(target harbor harbor_bench harbor_bench_engine harbor_bench_network harbor_crash_matrix)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O2 -march=native>)
    endif()
endforeach()
