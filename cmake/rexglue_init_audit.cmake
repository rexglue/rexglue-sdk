# Fails when the object files of a provider module hold a dynamic initializer. A module loads
# before the runtime is up, so nothing in it may run until its provider is connected.
# Included, it defines rexglue_audit_provider_init. Run as a script it checks one folder of
# objects, as in
#   cmake -DOBJECT_DIR=<folder> -DCONFIG=<config> -P rexglue_init_audit.cmake
# An object file with an .init_array, .ctors or __mod_init_func section, or a .CRT$XCU section on
# Windows, has a dynamic initializer. Only the objects of the module's own sources are checked, so
# a vendored static library linked into the module keeps its own.

if (DEFINED CMAKE_SCRIPT_MODE_FILE)
  if (NOT OBJECT_DIR)
    message(
      FATAL_ERROR "usage: -DOBJECT_DIR=<folder> -DCONFIG=<config> -P rexglue_init_audit.cmake"
    )
  endif ()

  if (CONFIG AND EXISTS "${OBJECT_DIR}/${CONFIG}")
    set(SEARCH_DIR "${OBJECT_DIR}/${CONFIG}")
  else ()
    set(SEARCH_DIR "${OBJECT_DIR}")
  endif ()
  file(GLOB_RECURSE OBJECT_FILES "${SEARCH_DIR}/*.o" "${SEARCH_DIR}/*.obj")
  if (NOT OBJECT_FILES)
    message(FATAL_ERROR "no object files under ${SEARCH_DIR}")
  endif ()

  get_filename_component(COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(
    OBJDUMP_TOOL
    NAMES llvm-objdump objdump
    HINTS "${COMPILER_DIR}" REQUIRED
  )

  set(OFFENDERS "")
  foreach (object IN LISTS OBJECT_FILES)
    execute_process(
      COMMAND ${OBJDUMP_TOOL} -h "${object}"
      OUTPUT_VARIABLE SECTIONS
      RESULT_VARIABLE RESULT
    )
    if (NOT RESULT EQUAL 0)
      message(FATAL_ERROR "${OBJDUMP_TOOL} failed on ${object}")
    endif ()
    if (SECTIONS MATCHES "\\.init_array|\\.ctors|__mod_init_func|\\.CRT\\$XCU")
      get_filename_component(NAME "${object}" NAME)
      list(APPEND OFFENDERS "${NAME}")
    endif ()
  endforeach ()

  if (OFFENDERS)
    message(FATAL_ERROR "dynamic initializer in: ${OFFENDERS}")
  endif ()
  message(STATUS "no dynamic initializer in ${SEARCH_DIR}")
  return()
endif ()

set(REXGLUE_INIT_AUDIT_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

# Adds a ctest that audits the objects of a provider module.
# EXPECT_FAIL marks a module that must fail the audit, the audit's own test.
function (rexglue_audit_provider_init target)
  cmake_parse_arguments(ARG "EXPECT_FAIL" "" "" ${ARGN})
  add_test(
    NAME "init audit ${target}"
    COMMAND
      ${CMAKE_COMMAND}
      -DOBJECT_DIR=$<TARGET_PROPERTY:${target},BINARY_DIR>/CMakeFiles/${target}.dir
      -DCONFIG=$<CONFIG> -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER} -P ${REXGLUE_INIT_AUDIT_SCRIPT}
  )
  if (ARG_EXPECT_FAIL)
    set_tests_properties("init audit ${target}" PROPERTIES WILL_FAIL TRUE)
  endif ()
endfunction ()
