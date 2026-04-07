# PowerPC Instruction Test Suite
# Compiles PPC assembly tests and generates Catch2 test cases

include(ppc_test_pipeline)

# Output directories
set(PPC_OBJ_DIR ${CMAKE_BINARY_DIR}/tests/ppc/obj)
set(PPC_BIN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/bin)
set(PPC_GEN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/generated)

file(MAKE_DIRECTORY ${PPC_OBJ_DIR})
file(MAKE_DIRECTORY ${PPC_BIN_DIR})
file(MAKE_DIRECTORY ${PPC_GEN_DIR})

# Collect and build all assembly test files
file(GLOB ASM_FILES ${CMAKE_CURRENT_SOURCE_DIR}/asm/*.s)
list(LENGTH ASM_FILES NUM_TESTS)

if(NUM_TESTS EQUAL 0)
    message(WARNING "No test files found in ${CMAKE_CURRENT_SOURCE_DIR}/asm/")
    return()
endif()

message(STATUS "Found ${NUM_TESTS} PPC instruction test files")

foreach(ASM_FILE ${ASM_FILES})
    ppc_add_test_binary(${ASM_FILE} ${PPC_OBJ_DIR} ${PPC_BIN_DIR})
endforeach()

get_property(TEST_BINS GLOBAL PROPERTY PPC_TEST_BINS)
get_property(TEST_MAPS GLOBAL PROPERTY PPC_TEST_MAPS)

# Generate Catch2 test code from recompiled binaries
add_custom_command(
    OUTPUT ${PPC_GEN_DIR}/ppc_test_functions.cpp
           ${PPC_GEN_DIR}/ppc_test_cases.cpp
           ${PPC_GEN_DIR}/ppc_test_decls.h
    COMMAND $<TARGET_FILE:rexglue> recompile-tests
            --output=${PPC_GEN_DIR}
            --bin_dir=${PPC_BIN_DIR}
            --asm_dir=${CMAKE_CURRENT_SOURCE_DIR}/asm
    DEPENDS ${TEST_BINS} ${TEST_MAPS} ${ASM_FILES} rexglue
    COMMENT "Generating Catch2 test code with rexglue"
    VERBATIM
)

add_executable(ppc_tests
    ${PPC_GEN_DIR}/ppc_test_functions.cpp
    ${PPC_GEN_DIR}/ppc_test_cases.cpp
)

set_source_files_properties(
    ${PPC_GEN_DIR}/ppc_test_functions.cpp
    ${PPC_GEN_DIR}/ppc_test_cases.cpp
    ${PPC_GEN_DIR}/ppc_test_decls.h
    PROPERTIES GENERATED TRUE
)

target_include_directories(ppc_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PPC_GEN_DIR}
)

target_link_libraries(ppc_tests PRIVATE
    simde
    rexkernel
    rexcore
    fmt::fmt
    Catch2::Catch2WithMain
)

# Delay-load FidelityFX DLLs so ppc_tests can run without them.
if(WIN32)
    if(TARGET amd_fidelityfx_vk)
        target_link_options(ppc_tests PRIVATE
            "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:amd_fidelityfx_vk>")
    endif()
    if(TARGET amd_fidelityfx_dx12)
        target_link_options(ppc_tests PRIVATE
            "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:amd_fidelityfx_dx12>")
    endif()
    if(TARGET amd_fidelityfx_vk OR TARGET amd_fidelityfx_dx12)
        target_link_libraries(ppc_tests PRIVATE delayimp)
    endif()
endif()

set_target_properties(ppc_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/bin
)

target_compile_options(ppc_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS
    -msse4.1 -mssse3
)

include(Catch)
catch_discover_tests(ppc_tests
    TEST_PREFIX "ppc."
    PROPERTIES
        LABELS "ppc;instructions"
        TIMEOUT 60
)

message(STATUS "Configured PPC test suite with ${NUM_TESTS} test files")
