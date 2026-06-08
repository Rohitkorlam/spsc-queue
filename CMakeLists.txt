cmake_minimum_required(VERSION 3.20)
project(spsc_ring LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard + warning flags
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-variable   # suppress in GBench/GTest generated code
)

# ---------------------------------------------------------------------------
# Build type defaults
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Release: optimised, with frame pointers retained for profiling
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -fno-omit-frame-pointer")

# Sanitizer preset — enable with:  cmake -DSANITIZE=address|thread|undefined
set(SANITIZE "" CACHE STRING "Sanitizer to enable: address, thread, undefined")
if(SANITIZE)
    add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${SANITIZE})
endif()

# ---------------------------------------------------------------------------
# Header-only library target
# ---------------------------------------------------------------------------
add_library(spsc_ring INTERFACE)
target_include_directories(spsc_ring INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(spsc_ring INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# Dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# Google Test
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Google Benchmark
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING        OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL        OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
enable_testing()

add_executable(spsc_tests tests/test_spsc_ring.cpp)
target_link_libraries(spsc_tests
    PRIVATE
        spsc_ring
        GTest::gtest
        GTest::gtest_main
        pthread
)

include(GoogleTest)
gtest_discover_tests(spsc_tests)

# ---------------------------------------------------------------------------
# Benchmark executable
# ---------------------------------------------------------------------------
add_executable(spsc_bench bench/bench.cpp)
target_link_libraries(spsc_bench
    PRIVATE
        spsc_ring
        benchmark::benchmark
        pthread
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Sanitizer  : ${SANITIZE}")
message(STATUS "CXX flags  : ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}}")
