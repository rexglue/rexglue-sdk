# rexglue_add_codegen(<target>
#                     MANIFEST <manifest>
#                     OUTPUT_DIR <folder>
#                     WORKING_DIRECTORY <folder>
#                     TOOL <command...>
#                     [MAX_FILE_SIZE <bytes>]
#                     [SOURCES <files...>])
# Runs the codegen tool as a build step. The tool writes the depfile into the build folder, and the
# step reruns when a listed input of the depfile changes or when MAX_FILE_SIZE changes. SOURCES are
# the generated files the step is ordered before. The step writes sources.cmake, which
# rexglue_project creates empty at configure when it is missing. The file is a configure
# dependency, so a run that changes the list of sources regenerates the build files before anything
# compiles them. dll_targets.cmake is a configure dependency too, and it lists the sources.cmake of
# each DLL module as one. Stops the configure when generated/rexglue.cmake calls it, which is the
# file an earlier SDK wrote into a project.
function (rexglue_add_codegen target_name)
  cmake_parse_arguments(
    PARSE_ARGV 1 ARG "" "MANIFEST;OUTPUT_DIR;WORKING_DIRECTORY;MAX_FILE_SIZE" "TOOL;SOURCES"
  )
  # In a function, CMAKE_CURRENT_LIST_FILE is the file of the caller. rexglue_project calls this
  # function from the project's CMakeLists.txt, and only the earlier file calls it from a file
  # named generated/rexglue.cmake, so a project that follows the current layout never matches.
  cmake_path(GET CMAKE_CURRENT_LIST_FILE FILENAME _list_name)
  cmake_path(GET CMAKE_CURRENT_LIST_DIR FILENAME _list_folder)
  if (_list_name STREQUAL "rexglue.cmake" AND _list_folder STREQUAL "generated")
    string(REGEX REPLACE "_codegen$" "" _project_name "${target_name}")
    file(RELATIVE_PATH _output_folder "${CMAKE_CURRENT_SOURCE_DIR}" "${ARG_OUTPUT_DIR}")
    set(_output_option "")
    if (NOT _output_folder STREQUAL "generated")
      set(_output_option " OUTPUT_DIR ${_output_folder}")
    endif ()
    set(_version "<version>")
    if (REXGLUE_VERSION_STRING MATCHES "^([0-9]+[.][0-9]+[.][0-9]+)")
      set(_version "${CMAKE_MATCH_1}")
    endif ()
    message(
      FATAL_ERROR
        "This project includes generated/rexglue.cmake, which an earlier SDK wrote. The file holds "
        "an old copy of the setup functions, and the SDK no longer uses it. Make two edits:\n"
        "1. Delete generated/rexglue.cmake and generated/*/codegen.d.\n"
        "2. In CMakeLists.txt, replace include(generated/rexglue.cmake) with\n"
        "  if (REXSDK_DIR)\n"
        "    add_subdirectory(\"\${REXSDK_DIR}\" rexglue-sdk)\n"
        "  else ()\n"
        "    find_package(rexglue ${_version} REQUIRED CONFIG)\n"
        "  endif ()\n"
        "  rexglue_project(${_project_name}${_output_option})\n"
        "Leave out the REXSDK_DIR branch when the project only uses an installed SDK. "
        "See Updating a Project in the CMake reference page."
    )
  endif ()
  set(stamp "${ARG_OUTPUT_DIR}/codegen.build.stamp")
  set(source_list "${ARG_OUTPUT_DIR}/sources.cmake")
  set(dll_list "${ARG_OUTPUT_DIR}/dll_targets.cmake")
  set(depfile "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.d")
  set(size_option "")
  if (ARG_MAX_FILE_SIZE)
    set(size_option "--max_file_size_bytes=${ARG_MAX_FILE_SIZE}")
  endif ()
  add_custom_command(
    OUTPUT "${stamp}" "${source_list}" ${ARG_SOURCES}
    COMMAND ${ARG_TOOL} codegen "${ARG_MANIFEST}" --depfile "${depfile}" ${size_option}
    DEPFILE "${depfile}"
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
