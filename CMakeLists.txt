cmake_minimum_required(VERSION 3.22)

project(matchbook
    VERSION 0.1.0
    DESCRIPTION "Deterministic limit order book and matching engine"
    LANGUAGES CXX)

# --------------------------------------------------------------------------
# NOTHING IN THIS PROJECT HAS BEEN COMPILED. This file is written to be correct
# on inspection and has never been run through CMake. See docs/BUILD_NOTES.md
# for the places most likely to need adjustment on the first attempt.
# --------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(MATCHBOOK_BUILD_TESTS   "Build the Catch2 test suite"        ON)
option(MATCHBOOK_BUILD_SERVER  "Build matchbook-server"             ON)
option(MATCHBOOK_BUILD_BENCH   "Build matchbook-bench"              ON)
option(MATCHBOOK_USE_HDR_LIB   "Use HdrHistogram_c instead of the bundled header" OFF)
option(MATCHBOOK_LIBFUZZER     "Build the fuzz target with libFuzzer (clang only)" OFF)
option(MATCHBOOK_NATIVE_ARCH   "Compile with -march=native (benchmarks only)" OFF)

# --------------------------------------------------------------------------
# Warning and optimisation flags
# --------------------------------------------------------------------------

add_library(matchbook_flags INTERFACE)

target_compile_options(matchbook_flags INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:
        -Wall -Wextra -Wpedantic
        -Wshadow
        -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
    >
)

# -march=native is off by default. It can produce a meaningfully faster binary,
# but it also makes results incomparable between machines, and the benchmark's
# whole purpose is comparison. Turn it on deliberately, and say so in the
# results file when you do.
if(MATCHBOOK_NATIVE_ARCH)
    target_compile_options(matchbook_flags INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native>)
endif()

# Release builds keep frame pointers. The cost is roughly one register; the
# benefit is that a profiler can produce a usable stack. Since `perf` is assumed
# unavailable under the stock WSL2 kernel this matters less than usual, but it
# costs almost nothing and makes gdb backtraces trustworthy.
target_compile_options(matchbook_flags INTERFACE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang>>:-fno-omit-frame-pointer>)

# --------------------------------------------------------------------------
# Core library: the four engines, validation, and the multi-symbol front door
# --------------------------------------------------------------------------

add_library(matchbook_core STATIC
    src/engine/engine.cpp
    src/engine/factory.cpp
    src/engine/v0_naive/v0_book.cpp
    src/engine/v1_pool/v1_book.cpp
    src/engine/v2_flat/v2_book.cpp
    src/engine/v3_tuned/v3_book.cpp
    src/validation/validator.cpp
    src/validation/invariants.cpp
)

target_include_directories(matchbook_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(matchbook_core PUBLIC matchbook_flags)

# Recorded in the binary so every benchmark results file can state the exact
# build it came from.
target_compile_definitions(matchbook_core PUBLIC
    MATCHBOOK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    MATCHBOOK_BUILD_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}}"
)

# --------------------------------------------------------------------------
# Third-party dependencies
# --------------------------------------------------------------------------

include(FetchContent)

# All FetchContent happens at configure time and needs network access on the
# first configure only. In Docker that is the image build; natively it is the
# first `cmake --preset`.
set(FETCHCONTENT_QUIET OFF)

if(MATCHBOOK_BUILD_SERVER)
    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        v0.15.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(httplib)
endif()

if(MATCHBOOK_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

if(MATCHBOOK_BUILD_BENCH AND MATCHBOOK_USE_HDR_LIB)
    FetchContent_Declare(hdr_histogram
        GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
        GIT_TAG        0.11.8
        GIT_SHALLOW    TRUE
    )
    set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(HDR_HISTOGRAM_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(hdr_histogram)
endif()

# --------------------------------------------------------------------------
# matchbook-server
# --------------------------------------------------------------------------

if(MATCHBOOK_BUILD_SERVER)
    add_executable(matchbook-server src/server/server_main.cpp)
    target_include_directories(matchbook-server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(matchbook-server PRIVATE matchbook_core httplib::httplib)
endif()

# --------------------------------------------------------------------------
# matchbook-bench
# --------------------------------------------------------------------------

if(MATCHBOOK_BUILD_BENCH)
    add_executable(matchbook-bench bench/bench_main.cpp)
    target_include_directories(matchbook-bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(matchbook-bench PRIVATE matchbook_core)

    if(MATCHBOOK_USE_HDR_LIB)
        target_compile_definitions(matchbook-bench PRIVATE MATCHBOOK_USE_HDR_LIB=1)
        target_link_libraries(matchbook-bench PRIVATE hdr_histogram_static)
    endif()

    # The benchmark is meaningless in a Debug build; say so at configure time
    # rather than letting someone measure -O0 and draw conclusions from it.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(WARNING
            "matchbook-bench is being built in Debug. Numbers from an -O0 build "
            "measure the absence of optimisation, not the design. Configure with "
            "--preset release before benchmarking.")
    endif()
endif()

# --------------------------------------------------------------------------
# matchbook-replay (differential oracle + determinism check)
# --------------------------------------------------------------------------

add_executable(matchbook-replay tests/replay_main.cpp)
target_include_directories(matchbook-replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(matchbook-replay PRIVATE matchbook_core)

# --------------------------------------------------------------------------
# matchbook-fuzz
# --------------------------------------------------------------------------

add_executable(matchbook-fuzz tests/fuzz_validation.cpp)
target_include_directories(matchbook-fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(matchbook-fuzz PRIVATE matchbook_core)

if(MATCHBOOK_LIBFUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
            "MATCHBOOK_LIBFUZZER requires clang. Configure with "
            "-DCMAKE_CXX_COMPILER=clang++ or leave the option OFF to build the "
            "standalone corpus fuzzer instead.")
    endif()
    target_compile_definitions(matchbook-fuzz PRIVATE MATCHBOOK_LIBFUZZER=1)
    target_compile_options(matchbook-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(matchbook-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

if(MATCHBOOK_BUILD_TESTS)
    enable_testing()
    include(CTest)

    add_executable(matchbook-tests
        tests/test_price_time.cpp
        tests/test_order_types.cpp
        tests/test_cancel_replace.cpp
        tests/test_v2_boundary.cpp
        tests/test_validation.cpp
    )
    target_include_directories(matchbook-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(matchbook-tests PRIVATE
        matchbook_core
        Catch2::Catch2WithMain)

    # Catch2's CTest integration registers each TEST_CASE separately, so a
    # failure names the case rather than just "the test binary failed".
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(matchbook-tests)

    # Determinism: the same fixture through all four engines must produce four
    # identical logs. Driven by the Makefile's diff-test target, which has the
    # Python oracle available; this registers the C++-only half with CTest.
    add_test(NAME replay_v0_runs
             COMMAND matchbook-replay --engine v0
                     --input ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recorded_stream.csv
                     --output ${CMAKE_BINARY_DIR}/replay_v0.log)
    add_test(NAME replay_v3_runs
             COMMAND matchbook-replay --engine v3
                     --input ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recorded_stream.csv
                     --output ${CMAKE_BINARY_DIR}/replay_v3.log)
endif()

# --------------------------------------------------------------------------
# Configure summary
# --------------------------------------------------------------------------

message(STATUS "")
message(STATUS "matchbook ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests        : ${MATCHBOOK_BUILD_TESTS}")
message(STATUS "  server       : ${MATCHBOOK_BUILD_SERVER}")
message(STATUS "  bench        : ${MATCHBOOK_BUILD_BENCH}")
message(STATUS "  hdr library  : ${MATCHBOOK_USE_HDR_LIB} (OFF = bundled header)")
message(STATUS "  march=native : ${MATCHBOOK_NATIVE_ARCH}")
message(STATUS "")
message(STATUS "  NOTE: this project has never been compiled. Expect first-build")
message(STATUS "        friction; see docs/BUILD_NOTES.md.")
message(STATUS "")
