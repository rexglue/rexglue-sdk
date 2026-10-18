#==========================================================
# rexglue_module.cmake
#
#   rexglue_configure_module_target(<target> ...) - title DLL module
#==========================================================
include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/rexglue_target.cmake)

#==========================================================
# rexglue_configure_module_target(<target> [HOST <host_target>])
#   - Title DLL module
#
# Output is colocated with HOST (or CMAKE_RUNTIME_OUTPUT_DIRECTORY) so the
# host's LoadUserModule finds it; the host is wired to depend on the module
# so a top-level build of the host pulls in all title DLLs. No runtime DLL
# staging here; the host's POST_BUILD copy covers shared dependencies.
#==========================================================
function (rexglue_configure_module_target target_name)
  cmake_parse_arguments(ARG "" "HOST" "" ${ARGN})

  if (ARG_HOST)
    set_target_properties(
      ${target_name}
      PROPERTIES LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
                 RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
                 ARCHIVE_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
    )
    # Defer add_dependencies so the host target may be declared after
    # this module call. Wrapping in EVAL CODE expands the variables now,
    # which DEFER CALL otherwise treats as literal argument text.
    cmake_language(
      EVAL CODE "cmake_language(DEFER CALL add_dependencies ${ARG_HOST} ${target_name})"
    )
  elseif (CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set_target_properties(
      ${target_name}
      PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
                 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
                 ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    )
  endif ()

  if (UNIX AND NOT APPLE)
    set_target_properties(
      ${target_name} PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON
    )
  endif ()

  rexglue_apply_target_settings(${target_name})
endfunction ()
