# Codegen library that recompiles PPC to C++

# Core recompiler sources
set(CODEGEN_CORE_SOURCES
    codegen.cpp
    analyze.cpp
    decoded_binary.cpp
    codegen_writer.cpp
    output_partition.cpp
    output_stamp.cpp
    config.cpp
    config_keys.cpp
    function_graph.cpp
    function_scanner.cpp
    vtable_scanner.cpp
    binary_view.cpp
    instruction_dispatch.cpp
    mmio_analysis.cpp
    stack_forwarding.cpp
    module_stub.cpp
    codegen_context.cpp
    phase_register.cpp
    phase_scan.cpp
    phase_discover.cpp
    phase_gapfill.cpp
    phase_merge.cpp
    phase_validate.cpp
    call_usage.cpp
    template_registry.cpp
    hook_scan.cpp
    manifest.cpp
    project_recompiler.cpp
    tool_runtime.cpp
)

# PPC architecture sources (merged from rexarch)
set(CODEGEN_PPC_SOURCES ppc/opcode.cpp ppc/instruction.cpp ppc/disasm.cpp ppc/ppc_dis_access.c)

# Instruction builder sources
set(CODEGEN_BUILDER_SOURCES
    builder/context.cpp
    builder/comparison.cpp
    builder/arithmetic.cpp
    builder/logical.cpp
    builder/control_flow.cpp
    builder/floating_point.cpp
    builder/memory.cpp
    builder/system.cpp
    builder/vector.cpp
)

# Inja templates
set(TEMPLATE_SOURCE_DIR "${REXGLUE_ROOT}/resources/templates")
set(TEMPLATE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/templates")

include(${REXGLUE_ROOT}/cmake/embed_template_names.cmake)

file(GLOB_RECURSE INJA_TEMPLATE_FILES CONFIGURE_DEPENDS "${TEMPLATE_SOURCE_DIR}/*.inja")

# Build output header paths
set(EMBEDDED_HEADERS "")
foreach (template_file ${INJA_TEMPLATE_FILES})
  file(RELATIVE_PATH REL_PATH "${TEMPLATE_SOURCE_DIR}" "${template_file}")
  embed_template_names("${REL_PATH}" CANONICAL_ID HEADER_NAME)
  list(APPEND EMBEDDED_HEADERS "${TEMPLATE_OUTPUT_DIR}/${HEADER_NAME}")
endforeach ()
list(APPEND EMBEDDED_HEADERS "${TEMPLATE_OUTPUT_DIR}/embedded_templates.h")

add_custom_command(
  OUTPUT ${EMBEDDED_HEADERS}
  COMMAND
    ${CMAKE_COMMAND} -DTEMPLATE_SOURCE_DIR=${TEMPLATE_SOURCE_DIR}
    -DTEMPLATE_OUTPUT_DIR=${TEMPLATE_OUTPUT_DIR} -P ${REXGLUE_ROOT}/cmake/embed_templates.cmake
  DEPENDS ${INJA_TEMPLATE_FILES} ${REXGLUE_ROOT}/cmake/embed_templates.cmake
          ${REXGLUE_ROOT}/cmake/embed_template_names.cmake
  COMMENT "Embedding inja templates"
  VERBATIM
)

add_custom_target(
  embed_templates
  COMMENT "Embedding inja templates"
  DEPENDS ${EMBEDDED_HEADERS}
)
set_target_properties(embed_templates PROPERTIES FOLDER "sdk/utility")

add_library(
  rexcodegen-objects OBJECT ${CODEGEN_CORE_SOURCES} ${CODEGEN_PPC_SOURCES}
                            ${CODEGEN_BUILDER_SOURCES}
)
set_target_properties(rexcodegen-objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(rexcodegen STATIC $<TARGET_OBJECTS:rexcodegen-objects>)
add_library(rex::codegen ALIAS rexcodegen)
set_source_files_properties(
  ppc/ppc_dis_access.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS -w
)
add_dependencies(rexcodegen-objects embed_templates)

rex_target_pch(rexcodegen-objects <function_types.h>)

foreach (codegen_target rexcodegen-objects rexcodegen)
  target_include_directories(
    ${codegen_target}
    PUBLIC $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${REXGLUE_ROOT}/thirdparty/disasm ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/builders ${CMAKE_CURRENT_BINARY_DIR}/generated/templates
  )
  target_include_directories(
    ${codegen_target} SYSTEM
    PUBLIC $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/inja/single_include>
           $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/inja/third_party/include>
  )
endforeach ()

target_link_libraries(
  rexcodegen-objects
  PUBLIC rexkernel-objects rexapp-objects
  PRIVATE tomlplusplus disasm xxHash::xxhash simde
)
target_link_libraries(
  rexcodegen
  PUBLIC rexkernel rexapp
  PRIVATE tomlplusplus disasm xxHash::xxhash simde
)
