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
            --bin-dir=${PPC_BIN_DIR}
            --asm-dir=${CMAKE_CURRENT_SOURCE_DIR}/asm
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
    rex-runtime
    Catch2::Catch2WithMain
)

set_target_properties(ppc_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(ppc_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

target_compile_options(ppc_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS
)
if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
    target_compile_options(ppc_tests PRIVATE -msse4.1 -mssse3)
endif()

include(Catch)
catch_discover_tests(ppc_tests
    TEST_PREFIX "ppc."
    PROPERTIES
        LABELS "ppc;instructions"
        TIMEOUT 60
)

message(STATUS "Configured PPC test suite with ${NUM_TESTS} test files")
