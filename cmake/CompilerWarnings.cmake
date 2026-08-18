# An INTERFACE target, not global flags: FetchContent pulls in Google Benchmark and
# friends, and compiling third-party code under -Werror -Wconversion is a losing game.
# Only our own targets link this.
add_library(maplab_warnings INTERFACE)
add_library(maplab::warnings ALIAS maplab_warnings)

set(_warn
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow -Wcast-qual -Wcast-align -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    -Wnull-dereference -Wunused)

# GCC-only. Kept in a separate list because clang-tidy reads compile_commands.json:
# if a GCC-configured build dir feeds clang-tidy, every -Wduplicated-cond becomes
# "error: unknown warning option". CI lints from a separate clang build dir.
set(_gcc_only -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  list(APPEND _warn ${_gcc_only})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(maplab_warnings INTERFACE ${_warn})
  if(MAPLAB_WERROR)
    target_compile_options(maplab_warnings INTERFACE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(maplab_warnings INTERFACE /W4 /permissive-)
  if(MAPLAB_WERROR)
    target_compile_options(maplab_warnings INTERFACE /WX)
  endif()
endif()
