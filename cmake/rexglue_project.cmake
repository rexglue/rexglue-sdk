# rexglue_project.cmake
#   rexglue_project(<name> [OUTPUT_DIR <dir>]) - connects a project's build to codegen
#   rexglue_setup_host_library(<library>) - SDK settings, includes and codegen dependency
#   rexglue_setup_target(<target> [NO_COPY] [EXCLUDE <name>...]) - the project's host executable
#   rexglue_apply_recomp_settings(<target> <pch>) - recompiler options for a generated library
include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/rexglue_target.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_metadata.cmake)

# The package config and the add_subdirectory path both include this file, and include_guard makes
# it run once per configure, so the message appears once.
if (EXISTS "${CMAKE_SOURCE_DIR}/generated/rexglue.cmake")
  message(
    DEPRECATION
      "The SDK no longer writes or updates generated/rexglue.cmake. Delete it, and replace "
      "include(generated/rexglue.cmake) in CMakeLists.txt with find_package(rexglue <version> "
      "REQUIRED CONFIG) and rexglue_project(<name>). The CMake reference page shows the lines."
  )
endif ()

# Stops with a clear error when rexglue_project has not run.
function (_rexglue_require_project caller)
  get_property(
    _called GLOBAL
    PROPERTY REXGLUE_PROJECT_NAME
    SET
  )
  if (NOT _called)
    message(
      FATAL_ERROR "${caller} was called before rexglue_project. Call rexglue_project(<name>) after "
                  "find_package(rexglue) and before ${caller}."
    )
  endif ()
endfunction ()

# rexglue_project(<name> [OUTPUT_DIR <dir>])
# Sets up a project: the REXGLUE_HOST_TARGET default, the REXGLUE_RECOMP_MAX_FILE_SIZE setting, the
# recompiler options, the generated sources and the <name>_codegen target. <name> is the name of the
# project's manifest, <name>_manifest.toml. OUTPUT_DIR is the codegen output folder, relative to the
# current source folder, and defaults to generated. A macro, so that the variables below, and the
# ones sources.cmake and dll_targets.cmake set, stay visible to the caller:
# REXGLUE_ENTRYPOINT_GENERATED_SOURCES, REXGLUE_ENTRYPOINT_PREFIX, REXGLUE_ENTRYPOINT_INCLUDE_DIR,
# REXGLUE_RECOMP_OPTIONS and REXGLUE_RECOMP_OPTIMIZED.
macro (rexglue_project name)
  cmake_parse_arguments(_rexglue_project "" "OUTPUT_DIR" "" ${ARGN})
  if (_rexglue_project_UNPARSED_ARGUMENTS OR _rexglue_project_KEYWORDS_MISSING_VALUES)
    message(
      FATAL_ERROR
        "rexglue_project: expected rexglue_project(<name> [OUTPUT_DIR <dir>]), got extra or "
        "incomplete arguments: ${_rexglue_project_UNPARSED_ARGUMENTS} "
        "${_rexglue_project_KEYWORDS_MISSING_VALUES}"
    )
  endif ()
  if (NOT _rexglue_project_OUTPUT_DIR)
    set(_rexglue_project_OUTPUT_DIR generated)
  endif ()
  set_property(GLOBAL PROPERTY REXGLUE_PROJECT_NAME "${name}")

  # Host target name used by the dll_targets.cmake that codegen emits to colocate guest DLLs with
  # the host EXE. Defaults to PROJECT_NAME, which the init flow sets equal to the host target name.
  if (NOT DEFINED REXGLUE_HOST_TARGET)
    set(REXGLUE_HOST_TARGET ${PROJECT_NAME})
  endif ()

  set(REXGLUE_RECOMP_MAX_FILE_SIZE
      ""
      CACHE STRING "Largest generated source file in bytes (empty keeps the tool default)"
  )
  if (NOT "${REXGLUE_RECOMP_MAX_FILE_SIZE}" MATCHES "^([0-9]+)?$")
    message(FATAL_ERROR "REXGLUE_RECOMP_MAX_FILE_SIZE must be empty or a byte count "
                        "(got '${REXGLUE_RECOMP_MAX_FILE_SIZE}')"
    )
  endif ()

  rexglue_recomp_options(REXGLUE_RECOMP_OPTIONS REXGLUE_RECOMP_OPTIMIZED)

  # Include entrypoint generated code if codegen has been run.
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_rexglue_project_OUTPUT_DIR}/sources.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/${_rexglue_project_OUTPUT_DIR}/sources.cmake")
    set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES ${GENERATED_SOURCES})
    set(REXGLUE_ENTRYPOINT_PREFIX ${GENERATED_PREFIX})
    set(REXGLUE_ENTRYPOINT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${_rexglue_project_OUTPUT_DIR}")

    # Carried on the sources so they hold for whichever target compiles them.
    set_source_files_properties(
      ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS
                                                         "${REXGLUE_RECOMP_OPTIONS}"
    )
  endif ()

  # Codegen runs as part of the build, re-running only when an input in codegen.d or
  # REXGLUE_RECOMP_MAX_FILE_SIZE changes. A run that changes the list of sources regenerates the
  # build files before anything compiles them. Listing the sources as outputs orders any target
  # that compiles them after codegen, including one a project assembles itself rather than taking
  # the library rexglue_setup_target() builds.
  set(_rexglue_codegen_max_file_size)
  if (REXGLUE_RECOMP_MAX_FILE_SIZE)
    set(_rexglue_codegen_max_file_size MAX_FILE_SIZE "${REXGLUE_RECOMP_MAX_FILE_SIZE}")
  endif ()
  rexglue_add_codegen(
    ${name}_codegen
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/${name}_manifest.toml"
    OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${_rexglue_project_OUTPUT_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TOOL $<TARGET_FILE:rex::rexglue> ${_rexglue_codegen_max_file_size}
    SOURCES ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}
  )

  # Include DLL module shared library targets if codegen has generated them
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_rexglue_project_OUTPUT_DIR}/dll_targets.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/${_rexglue_project_OUTPUT_DIR}/dll_targets.cmake")
  endif ()

  unset(_rexglue_project_OUTPUT_DIR)
  unset(_rexglue_project_UNPARSED_ARGUMENTS)
  unset(_rexglue_project_KEYWORDS_MISSING_VALUES)
endmacro ()

# The options go on the target as well. CMake compiles the header from its own cmake_pch.hxx.cxx,
# which carries none of the source properties above, and clang rejects a PCH whose EH mode
# disagrees with the TU including it.
function (rexglue_apply_recomp_settings target_name pch_header)
  _rexglue_require_project(rexglue_apply_recomp_settings)
  target_precompile_headers(${target_name} PRIVATE "${pch_header}")
  target_compile_options(${target_name} PRIVATE ${REXGLUE_RECOMP_OPTIONS})
endfunction ()

# Applies the SDK settings, include paths and codegen dependency to a host library.
macro (rexglue_setup_host_library library_name)
  _rexglue_require_project(rexglue_setup_host_library)
  get_property(_rexglue_project_name GLOBAL PROPERTY REXGLUE_PROJECT_NAME)
  rexglue_apply_target_settings(${library_name})
  target_include_directories(
    ${library_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
                            ${REXGLUE_ENTRYPOINT_INCLUDE_DIR}
  )
  target_link_libraries(${library_name} PRIVATE rex::sdk)
  add_dependencies(${library_name} ${_rexglue_project_name}_codegen)
  unset(_rexglue_project_name)
endmacro ()

# Configure a rexglue target with SDK libraries and platform settings.
macro (rexglue_setup_target target_name)
  _rexglue_require_project(rexglue_setup_target)
  get_property(_rexglue_project_name GLOBAL PROPERTY REXGLUE_PROJECT_NAME)
  if (REXGLUE_ENTRYPOINT_GENERATED_SOURCES AND NOT TARGET ${_rexglue_project_name}_recompiled)
    add_library(${_rexglue_project_name}_recompiled OBJECT ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES})
    target_include_directories(
      ${_rexglue_project_name}_recompiled
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
              ${REXGLUE_ENTRYPOINT_INCLUDE_DIR}
    )
    target_link_libraries(${_rexglue_project_name}_recompiled PRIVATE rex::sdk)
    set_source_files_properties(
      ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS ""
    )
    rexglue_apply_target_settings(${_rexglue_project_name}_recompiled)
    rexglue_apply_recomp_settings(
      ${_rexglue_project_name}_recompiled
      "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}/${REXGLUE_ENTRYPOINT_PREFIX}_pch.h"
    )
    add_dependencies(${_rexglue_project_name}_recompiled ${_rexglue_project_name}_codegen)
  endif ()
  if (TARGET ${_rexglue_project_name}_recompiled)
    target_link_libraries(${target_name} PRIVATE ${_rexglue_project_name}_recompiled)
  endif ()
  # The host depends on it too, so a tree that has never run codegen generates it.
  add_dependencies(${target_name} ${_rexglue_project_name}_codegen)
  target_include_directories(
    ${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${REXGLUE_ENTRYPOINT_INCLUDE_DIR}
  )
  target_link_libraries(${target_name} PRIVATE rex::sdk)
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata/icons")
    rexglue_embed_metadata(
      ${target_name} DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/metadata/icons" PREFIX "icons"
    )
  endif ()
  rexglue_configure_target(${target_name} ${ARGN})
  unset(_rexglue_project_name)
endmacro ()
