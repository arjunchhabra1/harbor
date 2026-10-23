cmake_minimum_required(VERSION 3.24)
project(harbor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HARBOR_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

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
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(harbor STATIC
    src/reference_book.cpp
    src/fast_book.cpp
)
target_include_directories(harbor PUBLIC include)
harbor_configure_target(harbor)

enable_testing()
add_executable(harbor_tests
    tests/test_types.cpp
    tests/test_flat_hash_map.cpp
    tests/test_book_behavior.cpp
    tests/test_differential.cpp
)
target_link_libraries(harbor_tests PRIVATE harbor GTest::gtest_main)
harbor_configure_target(harbor_tests)

include(GoogleTest)
gtest_discover_tests(harbor_tests)

# Benchmarks: build with -DCMAKE_BUILD_TYPE=Release for meaningful numbers.
add_executable(harbor_bench bench/bench_books.cpp)
target_include_directories(harbor_bench PRIVATE tests)
target_link_libraries(harbor_bench PRIVATE harbor)
harbor_configure_target(harbor_bench)
foreach(target harbor harbor_bench)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O2 -march=native>)
endforeach()
