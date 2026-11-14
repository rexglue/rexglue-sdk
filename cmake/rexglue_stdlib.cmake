# rexglue_stdlib.cmake
#   rexglue_detect_stdlib(<name_var> <release_var>) - which C++ standard library the compiler uses
#   rexglue_check_stdlib() - fails the consumer's configure on a library the SDK was not built with
include_guard(GLOBAL)

# rexglue_detect_stdlib(<name_var> <release_var>)
# Preprocesses a one line file with the C++ compiler and the C++ flags in use. <name_var> receives
# libstdc++, libc++ or unknown. <release_var> receives the major release of libstdc++ (the value of
# _GLIBCXX_RELEASE) and is empty for another library. The result is cached per compiler and flags.
function (rexglue_detect_stdlib name_var release_var)
  set(_key "${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_ARG1};${CMAKE_CXX_FLAGS}")
  if (NOT "${_REXGLUE_STDLIB_DETECTED_KEY}" STREQUAL "${_key}")
    set(_detect_dir "${CMAKE_BINARY_DIR}/CMakeFiles/rexglue_stdlib")
    file(MAKE_DIRECTORY "${_detect_dir}")
    file(
      WRITE "${_detect_dir}/detect.cpp"
      "#include <version>\n"
      "#if defined(_GLIBCXX_RELEASE)\n"
      "rex_stdlib libstdc++ _GLIBCXX_RELEASE\n"
      "#elif defined(_LIBCPP_VERSION)\n"
      "rex_stdlib libc++ 0\n"
      "#else\n"
      "rex_stdlib unknown 0\n"
      "#endif\n"
    )
    separate_arguments(_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
    separate_arguments(_arg1 UNIX_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" ${_arg1} ${_flags} -E -P -x c++ "${_detect_dir}/detect.cpp"
      RESULT_VARIABLE _result
      OUTPUT_VARIABLE _output
      ERROR_VARIABLE _error
    )
    if (NOT _result EQUAL 0 OR NOT _output MATCHES "rex_stdlib ([^ ]+) ([0-9]+)")
      message(
        FATAL_ERROR
          "ReXGlue could not read the C++ standard library from ${CMAKE_CXX_COMPILER}: ${_error}"
      )
    endif ()
    set(_REXGLUE_STDLIB_DETECTED_NAME
        "${CMAKE_MATCH_1}"
        CACHE INTERNAL ""
    )
    if (CMAKE_MATCH_1 STREQUAL "libstdc++")
      set(_REXGLUE_STDLIB_DETECTED_RELEASE
          "${CMAKE_MATCH_2}"
          CACHE INTERNAL ""
      )
    else ()
      set(_REXGLUE_STDLIB_DETECTED_RELEASE
          ""
          CACHE INTERNAL ""
      )
    endif ()
    set(_REXGLUE_STDLIB_DETECTED_KEY
        "${_key}"
        CACHE INTERNAL ""
    )
  endif ()
  set(${name_var}
      "${_REXGLUE_STDLIB_DETECTED_NAME}"
      PARENT_SCOPE
  )
  set(${release_var}
      "${_REXGLUE_STDLIB_DETECTED_RELEASE}"
      PARENT_SCOPE
  )
endfunction ()

# rexglue_check_stdlib()
# On Linux, compares the C++ standard library of the project's compiler with the one the SDK package
# was built against (REXGLUE_STDLIB and REXGLUE_LIBSTDCXX_RELEASE, set by rexglueConfig.cmake).
# Only the major release is compared. Does nothing before the C++ language is enabled, when the SDK
# is added as a subdirectory, or when the package records no library.
function (rexglue_check_stdlib)
  if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_LOADED)
    return()
  endif ()
  if (NOT REXGLUE_STDLIB)
    return()
  endif ()
  get_property(_checked GLOBAL PROPERTY REXGLUE_STDLIB_CHECKED)
  if (_checked)
    return()
  endif ()
  set_property(GLOBAL PROPERTY REXGLUE_STDLIB_CHECKED TRUE)

  rexglue_detect_stdlib(_name _release)
  if (_name STREQUAL REXGLUE_STDLIB AND _release STREQUAL REXGLUE_LIBSTDCXX_RELEASE)
    return()
  endif ()

  if (REXGLUE_LIBSTDCXX_RELEASE)
    set(_built "libstdc++ ${REXGLUE_LIBSTDCXX_RELEASE}")
  else ()
    set(_built "${REXGLUE_STDLIB}")
  endif ()
  if (_release)
    set(_uses "libstdc++ ${_release}")
  else ()
    set(_uses "${_name}")
  endif ()
  if (REXGLUE_LIBSTDCXX_RELEASE)
    set(_fix
        "Install g++-${REXGLUE_LIBSTDCXX_RELEASE} and libstdc++-${REXGLUE_LIBSTDCXX_RELEASE}-dev. "
        "With clang, also pass --gcc-install-dir=/usr/lib/gcc/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu/"
        "${REXGLUE_LIBSTDCXX_RELEASE} in CMAKE_C_FLAGS and CMAKE_CXX_FLAGS. "
        "Or build the SDK from source with REXSDK_DIR, which uses this project's compiler."
    )
  else ()
    set(_fix
        "Build the project against ${REXGLUE_STDLIB}, as the SDK package was, or build the SDK "
        "from source with REXSDK_DIR, which uses this project's compiler."
    )
  endif ()
  list(JOIN _fix "" _fix)
  message(
    FATAL_ERROR
      "The ReXGlue SDK package was built against ${_built}, but the C++ compiler of this project "
      "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) uses ${_uses}. Both sides emit the "
      "same standard library classes with different layouts, and the process crashes at startup.\n"
      "${_fix}\n"
      "The package records its values in ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglueConfig.cmake."
  )
endfunction ()
