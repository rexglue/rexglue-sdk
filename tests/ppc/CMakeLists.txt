# PowerPC Instruction Test Suite
# Compiles PPC assembly tests and generates Catch2 test cases

# Locate PPC toolchain in tools/ directory
set(PPC_TOOLS_DIR "${PROJECT_SOURCE_DIR}/tools/binutils")

if(WIN32)
    set(PPC_ASSEMBLER "${PPC_TOOLS_DIR}/powerpc-none-elf-as.exe")
    set(PPC_LINKER "${PPC_TOOLS_DIR}/powerpc-none-elf-ld.exe")
    set(PPC_NM "${PPC_TOOLS_DIR}/powerpc-none-elf-nm.exe")
else()
    set(PPC_ASSEMBLER "${PPC_TOOLS_DIR}/powerpc-none-elf-as")
    set(PPC_LINKER "${PPC_TOOLS_DIR}/powerpc-none-elf-ld")
    set(PPC_NM "${PPC_TOOLS_DIR}/powerpc-none-elf-nm")
endif()

# Verify toolchain exists
if(NOT EXISTS "${PPC_ASSEMBLER}")
    message(FATAL_ERROR "PPC assembler not found at ${PPC_ASSEMBLER}")
endif()
if(NOT EXISTS "${PPC_LINKER}")
    message(FATAL_ERROR "PPC linker not found at ${PPC_LINKER}")
endif()

message(STATUS "PowerPC assembler: ${PPC_ASSEMBLER}")
message(STATUS "PowerPC linker: ${PPC_LINKER}")

# Convert Windows paths to Cygwin paths for binutils (Windows only)
function(convert_to_cygwin_path WINDOWS_PATH OUTPUT_VAR)
    if(WIN32 AND WINDOWS_PATH MATCHES "^([A-Za-z]):")
        # Extract drive letter and convert to lowercase
        string(REGEX REPLACE "^([A-Za-z]):" "/cygdrive/\\1" CYGWIN_PATH "${WINDOWS_PATH}")
        string(SUBSTRING "${CYGWIN_PATH}" 10 1 DRIVE_LETTER)
        string(TOLOWER "${DRIVE_LETTER}" DRIVE_LOWER)
        string(REGEX REPLACE "^/cygdrive/[A-Za-z]" "/cygdrive/${DRIVE_LOWER}" CYGWIN_PATH "${CYGWIN_PATH}")
        # Replace backslashes with forward slashes
        string(REPLACE "\\" "/" CYGWIN_PATH "${CYGWIN_PATH}")
        set(${OUTPUT_VAR} "${CYGWIN_PATH}" PARENT_SCOPE)
    else()
        set(${OUTPUT_VAR} "${WINDOWS_PATH}" PARENT_SCOPE)
    endif()
endfunction()

# Create output directories
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ppc/obj)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ppc/bin)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/ppc/generated)

# Collect all .s files from asm directory
file(GLOB ASM_FILES ${CMAKE_CURRENT_SOURCE_DIR}/asm/*.s)
list(LENGTH ASM_FILES NUM_TESTS)

if(NUM_TESTS EQUAL 0)
    message(WARNING "No test files found in ${CMAKE_CURRENT_SOURCE_DIR}/asm/")
    return()
endif()

message(STATUS "Found ${NUM_TESTS} PPC instruction test files")

# Function to add a PPC test binary
function(add_ppc_test_binary TEST_NAME ASM_FILE)
    get_filename_component(BASE_NAME ${ASM_FILE} NAME_WE)

    # Output paths
    set(OBJ_FILE ${CMAKE_BINARY_DIR}/tests/ppc/obj/${BASE_NAME}.o)
    set(BIN_FILE ${CMAKE_BINARY_DIR}/tests/ppc/bin/${BASE_NAME}.bin)
    set(MAP_FILE ${CMAKE_BINARY_DIR}/tests/ppc/bin/${BASE_NAME}.map)

    # Convert paths for Cygwin binutils on Windows
    convert_to_cygwin_path("${OBJ_FILE}" OBJ_FILE_CYGWIN)
    convert_to_cygwin_path("${BIN_FILE}" BIN_FILE_CYGWIN)
    convert_to_cygwin_path("${MAP_FILE}" MAP_FILE_CYGWIN)
    convert_to_cygwin_path("${ASM_FILE}" ASM_FILE_CYGWIN)

    # Step 1: Assemble .s to .o
    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${PPC_ASSEMBLER}
                -a32                # 32-bit mode
                -be                 # Big-endian
                -mregnames          # Allow register names
                -mpower7            # PowerPC 7 architecture
                -maltivec           # AltiVec SIMD
                -mvsx               # VSX extensions
                -mvmx128            # VMX 128-bit
                -R                  # Fold data section into text
                -o ${OBJ_FILE_CYGWIN}
                ${ASM_FILE_CYGWIN}
        DEPENDS ${ASM_FILE}
        COMMENT "Assembling ${BASE_NAME}.s"
        VERBATIM
    )

    # Step 2: Link .o to .bin (resolves relocations)
    add_custom_command(
        OUTPUT ${BIN_FILE}
        COMMAND ${PPC_LINKER}
                -A powerpc:common32 # Architecture
                -melf32ppc          # 32-bit PowerPC ELF
                -EB                 # Big-endian
                -nostdlib           # No standard libraries
                --oformat=binary    # Raw binary output
                -Ttext=0x82010000   # Text segment address
                -e 0x82010000       # Entry point
                -o ${BIN_FILE_CYGWIN}
                ${OBJ_FILE_CYGWIN}
        DEPENDS ${OBJ_FILE}
        COMMENT "Linking ${BASE_NAME}.bin"
        VERBATIM
    )

    # Step 3: Generate symbol map from object file
    # Note: Use ${MAP_FILE} (not CYGWIN path) for shell redirection since cmd.exe handles it
    add_custom_command(
        OUTPUT ${MAP_FILE}
        COMMAND ${PPC_NM} --numeric-sort ${OBJ_FILE_CYGWIN} > ${MAP_FILE}
        DEPENDS ${OBJ_FILE}
        COMMENT "Generating symbol map for ${BASE_NAME}"
        VERBATIM
    )

    # Add to global lists
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_BINS ${BIN_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_MAPS ${MAP_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_OBJS ${OBJ_FILE})
endfunction()

# Build all test binaries
foreach(ASM_FILE ${ASM_FILES})
    get_filename_component(TEST_NAME ${ASM_FILE} NAME_WE)
    add_ppc_test_binary(${TEST_NAME} ${ASM_FILE})
endforeach()

# Get list of all test binaries, maps, and objects
get_property(TEST_BINS GLOBAL PROPERTY PPC_TEST_BINS)
get_property(TEST_MAPS GLOBAL PROPERTY PPC_TEST_MAPS)
get_property(TEST_OBJS GLOBAL PROPERTY PPC_TEST_OBJS)

# Step 4: Generate test code using rexglue recompile-tests
# Uses linked .bin files (with relocations resolved) and .map files (for symbol addresses)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_functions.cpp
           ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_cases.cpp
           ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_decls.h
    COMMAND $<TARGET_FILE:rexglue> recompile-tests
            --output=${CMAKE_BINARY_DIR}/tests/ppc/generated
            --bin_dir=${CMAKE_BINARY_DIR}/tests/ppc/bin
            --asm_dir=${CMAKE_CURRENT_SOURCE_DIR}/asm
    DEPENDS ${TEST_BINS} ${TEST_MAPS} ${ASM_FILES} rexglue
    COMMENT "Generating Catch2 test code with rexglue"
    VERBATIM
)

# Create executable with generated sources
add_executable(ppc_tests
    ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_functions.cpp
    ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_cases.cpp
)

# Mark generated sources
set_source_files_properties(
    ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_functions.cpp
    ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_cases.cpp
    ${CMAKE_BINARY_DIR}/tests/ppc/generated/ppc_test_decls.h
    PROPERTIES GENERATED TRUE
)

# Include directories
target_include_directories(ppc_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/tests/ppc/generated
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

# Enable SSE4.1 for vector intrinsics used in generated code
target_compile_options(ppc_tests PRIVATE -msse4.1 -mssse3)

include(Catch)

# Discover individual tests from Catch2 TEST_CASE macros
catch_discover_tests(ppc_tests
    TEST_PREFIX "ppc."
    PROPERTIES
        LABELS "ppc;instructions"
        TIMEOUT 60
)

message(STATUS "Configured PPC test suite with ${NUM_TESTS} test files")
