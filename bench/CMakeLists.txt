include(FetchContent)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
  SYSTEM EXCLUDE_FROM_ALL)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

if(MAPLAB_SANITIZE)
  message(WARNING "benchmarks are built without sanitizers even though "
                  "MAPLAB_SANITIZE='${MAPLAB_SANITIZE}'; sanitized timings are meaningless.")
endif()

add_executable(maplab_bench
  bench_main.cpp
  bench_ops.cpp
  bench_cache.cpp
  bench_strings.cpp
  bench_finance.cpp)

# Deliberately NOT linked against maplab::sanitizers.
target_link_libraries(maplab_bench PRIVATE
  maplab::maplab
  maplab::baselines
  maplab::workloads
  maplab::warnings
  benchmark::benchmark)
target_include_directories(maplab_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(maplab_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
