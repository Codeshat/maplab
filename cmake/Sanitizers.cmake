add_library(maplab_sanitizers INTERFACE)
add_library(maplab::sanitizers ALIAS maplab_sanitizers)

if(MAPLAB_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  string(REPLACE "," ";" _san "${MAPLAB_SANITIZE}")
  list(REMOVE_DUPLICATES _san)
  if("thread" IN_LIST _san AND "address" IN_LIST _san)
    message(FATAL_ERROR "'thread' and 'address' are mutually exclusive.")
  endif()
  list(JOIN _san "," _flag)

  set(_opts -fsanitize=${_flag} -fno-omit-frame-pointer -fno-optimize-sibling-calls)
  # Without this, UB is *logged* and the test still passes green, which is worse than
  # not running UBSan at all.
  if("undefined" IN_LIST _san)
    list(APPEND _opts -fno-sanitize-recover=undefined)
  endif()

  target_compile_options(maplab_sanitizers INTERFACE ${_opts})
  target_link_options(maplab_sanitizers INTERFACE -fsanitize=${_flag})
  message(STATUS "maplab: sanitizers enabled: ${_flag}")
endif()
