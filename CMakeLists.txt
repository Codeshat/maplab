cmake_minimum_required(VERSION 3.25)
project(maplab
        VERSION 0.1.0
        DESCRIPTION "A Swiss-table-style flat hash map built as a measurement laboratory"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Default the build-anything switches to PROJECT_IS_TOP_LEVEL so a downstream
# FetchContent of this project does not build our tests.
option(MAPLAB_BUILD_TESTS       "Build the Catch2 test suite"        ${PROJECT_IS_TOP_LEVEL})
option(MAPLAB_BUILD_BENCH       "Build the Google Benchmark suite"   OFF)
option(MAPLAB_BUILD_EXPERIMENTS "Build the ablation experiments"     OFF)
option(MAPLAB_BUILD_FUZZERS     "Requires Clang (libFuzzer)"         OFF)
option(MAPLAB_WERROR            "Treat warnings as errors"           ${PROJECT_IS_TOP_LEVEL})
option(MAPLAB_WITH_ABSEIL       "Add absl::flat_hash_map as a baseline (heavy dependency)" OFF)
option(MAPLAB_NATIVE            "Add -march=native (benchmarks only; never installed)"     OFF)
set(MAPLAB_SANITIZE "" CACHE STRING "e.g. 'address;undefined'")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++23, never -std=gnu++23

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
  endif()
endif()

include(CompilerWarnings)
include(Sanitizers)

# --------------------------------------------------------------------------------------
# Fail fast, at configure time, on the two things the library cannot work without:
# an SSE2 intrinsic path and C++20 <bit> bit-scan helpers. Better here than in 200
# lines of template spew.
# --------------------------------------------------------------------------------------
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles("
  #include <emmintrin.h>
  int main() {
    __m128i v = _mm_set1_epi8(3);
    return _mm_movemask_epi8(_mm_cmpeq_epi8(v, v)) == 0xFFFF ? 0 : 1;
  }" MAPLAB_HAVE_SSE2)

check_cxx_source_compiles("
  #include <bit>
  #include <cstdint>
  int main() { return std::countr_zero(std::uint32_t{8}) - 3; }" MAPLAB_HAVE_BIT)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT MAPLAB_HAVE_SSE2)
  message(FATAL_ERROR
    "maplab needs SSE2 intrinsics (<emmintrin.h>).\n"
    "  SSE2 is part of the x86-64 baseline, so on x86-64 this should never fire.\n"
    "  On 32-bit x86, configure with -DCMAKE_CXX_FLAGS=-msse2. ARM is not yet supported\n"
    "  (a NEON port of the group probe is on the roadmap).")
endif()
if(NOT MAPLAB_HAVE_BIT)
  message(FATAL_ERROR "maplab needs C++20 <bit> (std::countr_zero). Found "
                      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

# --------------------------------------------------------------------------------------
# The library itself: header-only.
# --------------------------------------------------------------------------------------
add_library(maplab INTERFACE)
add_library(maplab::maplab ALIAS maplab)
target_include_directories(maplab INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(maplab INTERFACE cxx_std_23)

if(MAPLAB_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # BUILD_INTERFACE only: never leak a machine-specific flag to consumers of an
  # installed package. Shipping -march=native is how you get SIGILL on an older CPU.
  # maplab's own probe is fixed SSE2 by design; this flag exists so the *baselines*
  # can be given the same compilation environment when we want that comparison.
  target_compile_options(maplab INTERFACE $<BUILD_INTERFACE:-march=native>)
endif()

# --------------------------------------------------------------------------------------
# Reference implementations. Context lines on graphs, not targets.
# --------------------------------------------------------------------------------------
add_library(maplab_baselines INTERFACE)
add_library(maplab::baselines ALIAS maplab_baselines)
# ankerl::unordered_dense is a single vendored header (MIT). See third_party/README.md.
target_include_directories(maplab_baselines SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_definitions(maplab_baselines INTERFACE MAPLAB_HAVE_ANKERL=1)

if(MAPLAB_WITH_ABSEIL)
  include(FetchContent)
  FetchContent_Declare(absl
    URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20250127.0.tar.gz
    URL_HASH SHA256=16242f394245627e508ec6bb296b433c90f8d914f73b9c026fddb905e27276e8
    SYSTEM EXCLUDE_FROM_ALL)
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(absl)
  target_link_libraries(maplab_baselines INTERFACE absl::flat_hash_map)
  target_compile_definitions(maplab_baselines INTERFACE MAPLAB_HAVE_ABSEIL=1)
endif()

# Shared workload/key generation, used by tests, benches and experiments alike.
# Scaffolding, so it is allowed to allocate and throw even where the library does not.
add_library(maplab_workloads INTERFACE)
add_library(maplab::workloads ALIAS maplab_workloads)
target_include_directories(maplab_workloads INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/include")
target_link_libraries(maplab_workloads INTERFACE maplab::maplab)

if(MAPLAB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(MAPLAB_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(MAPLAB_BUILD_EXPERIMENTS)
  add_subdirectory(experiments)
endif()
if(MAPLAB_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# Make clangd work with zero editor config.
if(PROJECT_IS_TOP_LEVEL AND CMAKE_EXPORT_COMPILE_COMMANDS AND UNIX)
  file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
       "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
endif()

message(STATUS "maplab ${PROJECT_VERSION}: tests=${MAPLAB_BUILD_TESTS} bench=${MAPLAB_BUILD_BENCH} "
               "experiments=${MAPLAB_BUILD_EXPERIMENTS} abseil=${MAPLAB_WITH_ABSEIL} "
               "native=${MAPLAB_NATIVE} sanitize='${MAPLAB_SANITIZE}'")
