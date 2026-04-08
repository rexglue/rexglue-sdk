# Codegen library - PPC to C++ recompiler

# Core recompiler sources
set(CODEGEN_CORE_SOURCES
    codegen.cpp
    codegen_flags.cpp
    analyze.cpp
    code_emitter.cpp
    decoded_binary.cpp
    codegen_writer.cpp
    config.cpp
    function_graph.cpp
    function_scanner.cpp
    sig_scanner.cpp
    vtable_scanner.cpp
    binary_view.cpp
    instruction_dispatch.cpp
    test_support.cpp
    codegen_context.cpp
    phase_register.cpp
    phase_scan.cpp
    phase_discover.cpp
    phase_gapfill.cpp
    phase_merge.cpp
    phase_validate.cpp
    template_registry.cpp
)

# PPC architecture sources (merged from rexarch)
set(CODEGEN_PPC_SOURCES
    ppc/opcode.cpp
    ppc/instruction.cpp
    ppc/disasm.cpp
)

# Instruction builder sources
set(CODEGEN_BUILDER_SOURCES
    builders/context.cpp
    builders/comparison.cpp
    builders/arithmetic.cpp
    builders/logical.cpp
    builders/control_flow.cpp
    builders/floating_point.cpp
    builders/memory.cpp
    builders/system.cpp
    builders/vector.cpp
)

# Inja templates
set(TEMPLATE_SOURCE_DIR "${REXGLUE_ROOT}/resources/templates")
set(TEMPLATE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/templates")

set(INJA_TEMPLATE_FILES
    ${TEMPLATE_SOURCE_DIR}/init/app_header.inja
    ${TEMPLATE_SOURCE_DIR}/init/config_toml.inja
    ${TEMPLATE_SOURCE_DIR}/init/main_cpp.inja
    ${TEMPLATE_SOURCE_DIR}/init/cmakelists.inja
    ${TEMPLATE_SOURCE_DIR}/init/cmake_presets.inja
    ${TEMPLATE_SOURCE_DIR}/init/rexglue_cmake.inja
    ${TEMPLATE_SOURCE_DIR}/codegen/init_cpp.inja
    ${TEMPLATE_SOURCE_DIR}/codegen/init_h.inja
    ${TEMPLATE_SOURCE_DIR}/codegen/sources_cmake.inja
    ${TEMPLATE_SOURCE_DIR}/test/ppc_test_cases_cpp.inja
    ${TEMPLATE_SOURCE_DIR}/test/ppc_test_decls_h.inja
    ${TEMPLATE_SOURCE_DIR}/test/ppc_test_functions_cpp.inja
    ${TEMPLATE_SOURCE_DIR}/test/ppc_config_h.inja
)

# Build output header paths
set(EMBEDDED_HEADERS "")
foreach(TEMPLATE_FILE ${INJA_TEMPLATE_FILES})
    file(RELATIVE_PATH REL_PATH "${TEMPLATE_SOURCE_DIR}" "${TEMPLATE_FILE}")
    string(REGEX REPLACE "\\.inja$" "" CANONICAL_ID "${REL_PATH}")
    string(REPLACE "/" "_" VAR_NAME "${CANONICAL_ID}")
    string(REPLACE "-" "_" VAR_NAME "${VAR_NAME}")
    list(APPEND EMBEDDED_HEADERS "${TEMPLATE_OUTPUT_DIR}/${VAR_NAME}.inja.h")
endforeach()
list(APPEND EMBEDDED_HEADERS "${TEMPLATE_OUTPUT_DIR}/embedded_templates.h")

add_custom_command(
    OUTPUT ${EMBEDDED_HEADERS}
    COMMAND ${CMAKE_COMMAND}
        -DTEMPLATE_SOURCE_DIR=${TEMPLATE_SOURCE_DIR}
        -DTEMPLATE_OUTPUT_DIR=${TEMPLATE_OUTPUT_DIR}
        -P ${REXGLUE_ROOT}/cmake/embed_templates.cmake
    DEPENDS ${INJA_TEMPLATE_FILES} ${REXGLUE_ROOT}/cmake/embed_templates.cmake
    COMMENT "Embedding inja templates"
    VERBATIM
)

add_custom_target(embed_templates DEPENDS ${EMBEDDED_HEADERS})

add_library(rexcodegen STATIC
    ${CODEGEN_CORE_SOURCES}
    ${CODEGEN_PPC_SOURCES}
    ${CODEGEN_BUILDER_SOURCES}
)
add_library(rex::codegen ALIAS rexcodegen)
add_dependencies(rexcodegen embed_templates)


target_include_directories(rexcodegen
    PUBLIC
        $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
        $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/inja/single_include>
        $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/inja/third_party/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${REXGLUE_ROOT}/thirdparty/disasm
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/builders
        ${CMAKE_CURRENT_BINARY_DIR}/generated/templates
)

target_link_libraries(rexcodegen
    PUBLIC
        rexcore
        rexsystem
        rexkernel
        fmt::fmt
        spdlog::spdlog
    PRIVATE
        tomlplusplus
        disasm
        xxHash::xxhash
        simde
)

