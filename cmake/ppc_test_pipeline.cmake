# cmake/ppc_test_pipeline.cmake
# PPC assembly test build pipeline
# Provides ppc_add_test_binary() for assembling, linking, and generating
# symbol maps from PPC assembly test files.

# Locate PPC toolchain in tools/ directory
set(REXGLUE_PPC_BINUTILS_DIR "${PROJECT_SOURCE_DIR}/tools/binutils" CACHE PATH
    "Directory holding the powerpc-none-elf binutils for the PPC instruction tests")
set(PPC_TOOLS_DIR "${REXGLUE_PPC_BINUTILS_DIR}")

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

execute_process(
    COMMAND "${PPC_ASSEMBLER}" --version
    RESULT_VARIABLE _ppc_as_result
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT _ppc_as_result EQUAL 0)
    message(FATAL_ERROR
        "PPC assembler at ${PPC_ASSEMBLER} does not run on this host. "
        "Set REXGLUE_PPC_BINUTILS_DIR to a powerpc-none-elf binutils build with VMX128 support.")
endif()

message(STATUS "PowerPC assembler: ${PPC_ASSEMBLER}")
message(STATUS "PowerPC linker: ${PPC_LINKER}")

# Convert Windows paths to Cygwin paths for binutils (Windows only)
function(_ppc_convert_to_cygwin_path WINDOWS_PATH OUTPUT_VAR)
    if(WIN32 AND WINDOWS_PATH MATCHES "^([A-Za-z]):")
        string(REGEX REPLACE "^([A-Za-z]):" "/cygdrive/\\1" CYGWIN_PATH "${WINDOWS_PATH}")
        string(SUBSTRING "${CYGWIN_PATH}" 10 1 DRIVE_LETTER)
        string(TOLOWER "${DRIVE_LETTER}" DRIVE_LOWER)
        string(REGEX REPLACE "^/cygdrive/[A-Za-z]" "/cygdrive/${DRIVE_LOWER}" CYGWIN_PATH "${CYGWIN_PATH}")
        string(REPLACE "\\" "/" CYGWIN_PATH "${CYGWIN_PATH}")
        set(${OUTPUT_VAR} "${CYGWIN_PATH}" PARENT_SCOPE)
    else()
        set(${OUTPUT_VAR} "${WINDOWS_PATH}" PARENT_SCOPE)
    endif()
endfunction()

# Add a PPC test binary from an assembly file.
# Assembles .s -> .o, links .o -> .bin, generates .o -> .map.
# Appends output files to PPC_TEST_BINS, PPC_TEST_MAPS, PPC_TEST_OBJS
# global properties.
function(ppc_add_test_binary ASM_FILE OBJ_DIR BIN_DIR)
    get_filename_component(BASE_NAME ${ASM_FILE} NAME_WE)

    set(OBJ_FILE ${OBJ_DIR}/${BASE_NAME}.o)
    set(BIN_FILE ${BIN_DIR}/${BASE_NAME}.bin)
    set(MAP_FILE ${BIN_DIR}/${BASE_NAME}.map)

    _ppc_convert_to_cygwin_path("${OBJ_FILE}" OBJ_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${BIN_FILE}" BIN_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${MAP_FILE}" MAP_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${ASM_FILE}" ASM_FILE_CYGWIN)

    # Assemble .s to .o
    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${PPC_ASSEMBLER}
                -a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128 -R
                -o ${OBJ_FILE_CYGWIN}
                ${ASM_FILE_CYGWIN}
        DEPENDS ${ASM_FILE}
        COMMENT "Assembling ${BASE_NAME}.s"
        VERBATIM
    )

    # Link .o to .bin
    add_custom_command(
        OUTPUT ${BIN_FILE}
        COMMAND ${PPC_LINKER}
                -A powerpc:common32 -melf32ppc -EB -nostdlib
                --oformat=binary -Ttext=0x82010000 -e 0x82010000
                -o ${BIN_FILE_CYGWIN}
                ${OBJ_FILE_CYGWIN}
        DEPENDS ${OBJ_FILE}
        COMMENT "Linking ${BASE_NAME}.bin"
        VERBATIM
    )

    # Generate symbol map
    add_custom_command(
        OUTPUT ${MAP_FILE}
        COMMAND ${PPC_NM} --numeric-sort ${OBJ_FILE_CYGWIN} > ${MAP_FILE}
        DEPENDS ${OBJ_FILE}
        COMMENT "Generating symbol map for ${BASE_NAME}"
        VERBATIM
    )

    set_property(GLOBAL APPEND PROPERTY PPC_TEST_BINS ${BIN_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_MAPS ${MAP_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_OBJS ${OBJ_FILE})
endfunction()
