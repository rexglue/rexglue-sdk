# rexglue_add_codegen(<target>
#                     MANIFEST <manifest>
#                     OUTPUT_DIR <folder>
#                     WORKING_DIRECTORY <folder>
#                     TOOL <command...>
#                     [MAX_FILE_SIZE <bytes>]
#                     [SOURCES <files...>])
# Runs the codegen tool as a build step. The step reruns when a listed input of the depfile
# changes or when MAX_FILE_SIZE changes. SOURCES are the generated files the step is ordered
# before. The step writes sources.cmake, which rexglue_project creates empty at configure when it
# is missing. The file is a configure dependency, so a run that changes the list of sources
# regenerates the build files before anything compiles them. dll_targets.cmake is a configure
# dependency too, and it lists the sources.cmake of each DLL module as one.
function (rexglue_add_codegen target_name)
  cmake_parse_arguments(
    PARSE_ARGV 1 ARG "" "MANIFEST;OUTPUT_DIR;WORKING_DIRECTORY;MAX_FILE_SIZE" "TOOL;SOURCES"
  )
  set(stamp "${ARG_OUTPUT_DIR}/codegen.build.stamp")
  set(source_list "${ARG_OUTPUT_DIR}/sources.cmake")
  set(dll_list "${ARG_OUTPUT_DIR}/dll_targets.cmake")
  set(size_option "")
  if (ARG_MAX_FILE_SIZE)
    set(size_option "--max_file_size_bytes=${ARG_MAX_FILE_SIZE}")
  endif ()
  add_custom_command(
    OUTPUT "${stamp}" "${source_list}" ${ARG_SOURCES}
    COMMAND ${ARG_TOOL} codegen "${ARG_MANIFEST}" ${size_option}
    DEPFILE "${ARG_OUTPUT_DIR}/codegen.d"
    WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}"
    COMMENT "Generating recompiled code for ${target_name}"
    VERBATIM
  )
  add_custom_target(
    ${target_name}
    COMMENT "Checking recompiled code for ${target_name}"
    DEPENDS "${stamp}"
  )
  # The step lists only sources.cmake as an output, so a change of dll_targets.cmake or of a DLL
  # module's sources.cmake regenerates the build files on the next ninja invocation.
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_list}" "${dll_list}"
  )
endfunction ()
