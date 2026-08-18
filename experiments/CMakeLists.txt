if(MAPLAB_SANITIZE)
  message(WARNING "experiments are built without sanitizers even though "
                  "MAPLAB_SANITIZE='${MAPLAB_SANITIZE}'; sanitized timings are meaningless.")
endif()

function(maplab_add_experiment name)
  add_executable(${name} ${name}.cpp)
  # Deliberately NOT linked against maplab::sanitizers.
  target_link_libraries(${name} PRIVATE
    maplab::maplab maplab::workloads maplab::baselines maplab::warnings)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  list(APPEND MAPLAB_EXPERIMENT_TARGETS ${name})
  set(MAPLAB_EXPERIMENT_TARGETS "${MAPLAB_EXPERIMENT_TARGETS}" PARENT_SCOPE)
endfunction()

maplab_add_experiment(exp_simd_vs_scalar)
maplab_add_experiment(exp_load_factor)
maplab_add_experiment(exp_hash_quality)
maplab_add_experiment(exp_h2_filter)
maplab_add_experiment(exp_tombstones)
maplab_add_experiment(exp_group_size)
maplab_add_experiment(exp_memory)

# exp_memory replaces the global operator new/delete with malloc/free wrappers so it can
# count what the heap actually holds. Replacing them is explicitly allowed
# ([basic.stc.dynamic.allocation]) and free() is the correct partner for malloc(), but GCC's
# -Wmismatched-new-delete sees a `new` whose storage came from malloc and complains -- at
# the call site inside libstdc++, not at our definition, so a pragma in the source cannot
# reach it. Scoped to this one file rather than dropped from the warning set.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set_source_files_properties(exp_memory.cpp
    PROPERTIES COMPILE_OPTIONS -Wno-mismatched-new-delete)
endif()

add_custom_target(experiments DEPENDS ${MAPLAB_EXPERIMENT_TARGETS})
