if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "MAPLAB_BUILD_FUZZERS requires Clang (libFuzzer). Use --preset fuzz "
                      "with CC=clang-19 CXX=clang++-19.")
endif()

function(maplab_add_fuzzer name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE
    maplab::maplab maplab::workloads maplab::warnings maplab::sanitizers)
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

maplab_add_fuzzer(fuzz_ops)
