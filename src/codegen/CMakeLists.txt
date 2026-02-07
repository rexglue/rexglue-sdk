# Codegen library - PPC to C++ recompiler

# Core recompiler sources
set(CODEGEN_CORE_SOURCES
    codegen.cpp
    analyze.cpp
    code_emitter.cpp
    decoded_binary.cpp
    discovery.cpp
    recompile.cpp
    recompiler.cpp
    config.cpp
    function_graph.cpp
    function_scanner.cpp
    sig_scanner.cpp
    vtable_scanner.cpp
    binary_view.cpp
    instruction_dispatch.cpp
    output.cpp
    test_module.cpp
    test_analyze.cpp
    codegen_context.cpp
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

add_library(rexcodegen STATIC
    ${CODEGEN_CORE_SOURCES}
    ${CODEGEN_PPC_SOURCES}
    ${CODEGEN_BUILDER_SOURCES}
)
add_library(rex::codegen ALIAS rexcodegen)



target_include_directories(rexcodegen
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/disasm
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/builders
)

target_link_libraries(rexcodegen
    PUBLIC
        rexcore
        rexruntime
        rexkernel
        fmt::fmt
        spdlog::spdlog
    PRIVATE
        tomlplusplus
        disasm
        xxHash::xxhash
        simde
)

