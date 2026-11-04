set(CMAKE_FOLDER "tests/headers")

# Every public header belongs to exactly one library header set, which CMake verifies alone with
# the all_verify_interface_header_sets target.
get_property(_owned_headers GLOBAL PROPERTY REXGLUE_HEADER_SET_FILES)
file(GLOB_RECURSE _public_headers CONFIGURE_DEPENDS "${REXGLUE_ROOT}/include/rex/*.h")
set(_unowned_headers "")
foreach (_header IN LISTS _public_headers)
  if (NOT _header IN_LIST _owned_headers)
    list(APPEND _unowned_headers "${_header}")
  endif ()
endforeach ()
if (_unowned_headers)
  list(JOIN _unowned_headers "\n  " _unowned_text)
  message(FATAL_ERROR "Public headers in no header set:\n  ${_unowned_text}")
endif ()
set(_owned_unique ${_owned_headers})
list(REMOVE_DUPLICATES _owned_unique)
list(LENGTH _owned_headers _owned_count)
list(LENGTH _owned_unique _owned_unique_count)
if (NOT _owned_count EQUAL _owned_unique_count)
  message(FATAL_ERROR "A public header is in more than one header set")
endif ()

# Recompiled code headers stay free of the runtime, the kernel and the UI
add_test(NAME ppc_header_isolation COMMAND ${CMAKE_COMMAND} -DREXGLUE_ROOT=${REXGLUE_ROOT} -P
                                           ${CMAKE_CURRENT_SOURCE_DIR}/ppc_header_isolation.cmake
)
