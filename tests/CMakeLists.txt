# Catch2 v3. Prefer an installed copy (CI images and most distros have one) and fall back
# to FetchContent, so a fresh clone builds either way.
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  include(FetchContent)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
    SYSTEM EXCLUDE_FROM_ALL)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

add_executable(maplab_tests
  test_basic.cpp
  test_collisions.cpp
  test_control.cpp
  test_differential.cpp
  test_lifetime.cpp
  test_strings.cpp
  test_zero_cost.cpp)

target_link_libraries(maplab_tests PRIVATE
  maplab::maplab
  maplab::workloads
  maplab::warnings
  maplab::sanitizers
  Catch2::Catch2WithMain)
target_include_directories(maplab_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(maplab_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(Catch)
catch_discover_tests(maplab_tests)  # one CTest entry per case, so `ctest -j` parallelises
