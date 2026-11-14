# rexglue_add_codegen(<target>
#                     MANIFEST <manifest>
#                     OUTPUT_DIR <folder>
#                     WORKING_DIRECTORY <folder>
#                     TOOL <command...>
#                     [MAX_FILE_SIZE <bytes>]
#                     [SOURCES <files...>])
# Runs the codegen tool as a build step. The step reruns when a listed input of the depfile
# changes or when MAX_FILE_SIZE changes. SOURCES are the generated files the step is ordered
# before. The step also writes sources.cmake, which is a configure dependency, so a run that
# changes the list of sources regenerates the build files before anything compiles them.
function (rexglue_add_codegen target_name)
  cmake_parse_arguments(
    PARSE_ARGV 1 ARG "" "MANIFEST;OUTPUT_DIR;WORKING_DIRECTORY;MAX_FILE_SIZE" "TOOL;SOURCES"
  )
  set(stamp "${ARG_OUTPUT_DIR}/codegen.build.stamp")
  set(source_list "${ARG_OUTPUT_DIR}/sources.cmake")
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
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_list}"
  )
endfunction ()
