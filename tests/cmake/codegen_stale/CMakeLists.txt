cmake_minimum_required(VERSION 3.25)
project(codegen_stale LANGUAGES CXX)

include(${REXGLUE_ROOT}/cmake/rexglue_codegen.cmake)

set(REXGLUE_RECOMP_MAX_FILE_SIZE
    ""
    CACHE STRING "Largest generated source file in bytes"
)

set(GENERATED ${CMAKE_BINARY_DIR}/generated)
set(GENERATED_SOURCES "")
if (EXISTS ${GENERATED}/sources.cmake)
  include(${GENERATED}/sources.cmake)
endif ()

rexglue_add_codegen(
  probe_codegen
  MANIFEST ${CMAKE_CURRENT_LIST_FILE}
  OUTPUT_DIR ${GENERATED}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  TOOL ${CMAKE_COMMAND} -DGENERATED=${GENERATED} -P ${CMAKE_CURRENT_LIST_DIR}/fake_rexglue.cmake --
  MAX_FILE_SIZE "${REXGLUE_RECOMP_MAX_FILE_SIZE}"
  SOURCES ${GENERATED_SOURCES}
)

add_library(probe OBJECT ${GENERATED_SOURCES})
add_dependencies(probe probe_codegen)
