# cmake/ppc_test_pipeline.cmake
# PPC assembly test build pipeline
# Provides ppc_add_test_binary() for assembling, linking, and generating
# symbol maps from PPC assembly test files.

# Locate PPC toolchain in tools/ directory
set(REXGLUE_PPC_BINUTILS_DIR
    "${PROJECT_SOURCE_DIR}/tools/binutils"
    CACHE PATH "Directory holding the powerpc-none-elf binutils for the PPC instruction tests"
)
set(PPC_TOOLS_DIR "${REXGLUE_PPC_BINUTILS_DIR}")

if (WIN32)
  set(PPC_ASSEMBLER "${PPC_TOOLS_DIR}/powerpc-none-elf-as.exe")
  set(PPC_LINKER "${PPC_TOOLS_DIR}/powerpc-none-elf-ld.exe")
  set(PPC_NM "${PPC_TOOLS_DIR}/powerpc-none-elf-nm.exe")
else ()
  set(PPC_ASSEMBLER "${PPC_TOOLS_DIR}/powerpc-none-elf-as")
  set(PPC_LINKER "${PPC_TOOLS_DIR}/powerpc-none-elf-ld")
  set(PPC_NM "${PPC_TOOLS_DIR}/powerpc-none-elf-nm")
endif ()

# Verify toolchain exists
if (NOT EXISTS "${PPC_ASSEMBLER}")
  message(FATAL_ERROR "PPC assembler not found at ${PPC_ASSEMBLER}")
endif ()
if (NOT EXISTS "${PPC_LINKER}")
  message(FATAL_ERROR "PPC linker not found at ${PPC_LINKER}")
endif ()

execute_process(
  COMMAND "${PPC_ASSEMBLER}" --version
  RESULT_VARIABLE _ppc_as_result
  OUTPUT_QUIET ERROR_QUIET
)
if (NOT _ppc_as_result EQUAL 0)
  message(
    FATAL_ERROR
      "PPC assembler at ${PPC_ASSEMBLER} does not run on this host. "
      "Set REXGLUE_PPC_BINUTILS_DIR to a powerpc-none-elf binutils build with VMX128 support."
  )
endif ()

message(STATUS "PowerPC assembler: ${PPC_ASSEMBLER}")
message(STATUS "PowerPC linker: ${PPC_LINKER}")

# Convert Windows paths to Cygwin paths for binutils (Windows only)
function (_ppc_convert_to_cygwin_path windows_path output_var)
  if (WIN32 AND windows_path MATCHES "^([A-Za-z]):")
    string(REGEX REPLACE "^([A-Za-z]):" "/cygdrive/\\1" converted_path "${windows_path}")
    string(SUBSTRING "${converted_path}" 10 1 drive_letter)
    string(TOLOWER "${drive_letter}" drive_lower)
    string(REGEX REPLACE "^/cygdrive/[A-Za-z]" "/cygdrive/${drive_lower}" converted_path
                         "${converted_path}"
    )
    string(REPLACE "\\" "/" converted_path "${converted_path}")
    set(${output_var}
        "${converted_path}"
        PARENT_SCOPE
    )
  else ()
    set(${output_var}
        "${windows_path}"
        PARENT_SCOPE
    )
  endif ()
endfunction ()

# Add a PPC test binary from an assembly file.
# Assembles .s -> .o, links .o -> .bin, generates .o -> .map.
# Appends output files to PPC_TEST_BINS, PPC_TEST_MAPS, PPC_TEST_OBJS
# global properties.
function (ppc_add_test_binary asm_file obj_dir bin_dir)
  get_filename_component(base_name ${asm_file} NAME_WE)

  set(obj_file ${obj_dir}/${base_name}.o)
  set(bin_file ${bin_dir}/${base_name}.bin)
  set(map_file ${bin_dir}/${base_name}.map)

  _ppc_convert_to_cygwin_path("${obj_file}" obj_file_cygwin)
  _ppc_convert_to_cygwin_path("${bin_file}" bin_file_cygwin)
  _ppc_convert_to_cygwin_path("${map_file}" map_file_cygwin)
  _ppc_convert_to_cygwin_path("${asm_file}" asm_file_cygwin)

  # Assemble .s to .o
  add_custom_command(
    OUTPUT ${obj_file}
    COMMAND ${PPC_ASSEMBLER} -a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128 -R -o
            ${obj_file_cygwin} ${asm_file_cygwin}
    DEPENDS ${asm_file}
    COMMENT "Assembling ${base_name}.s"
    VERBATIM
  )

  # Link .o to .bin
  add_custom_command(
    OUTPUT ${bin_file}
    COMMAND ${PPC_LINKER} -A powerpc:common32 -melf32ppc -EB -nostdlib --oformat=binary
            -Ttext=0x82010000 -e 0x82010000 -o ${bin_file_cygwin} ${obj_file_cygwin}
    DEPENDS ${obj_file}
    COMMENT "Linking ${base_name}.bin"
    VERBATIM
  )

  # Generate symbol map
  add_custom_command(
    OUTPUT ${map_file}
    COMMAND ${PPC_NM} --numeric-sort ${obj_file_cygwin} > ${map_file}
    DEPENDS ${obj_file}
    COMMENT "Generating symbol map for ${base_name}"
    VERBATIM
  )

  set_property(GLOBAL APPEND PROPERTY PPC_TEST_BINS ${bin_file})
  set_property(GLOBAL APPEND PROPERTY PPC_TEST_MAPS ${map_file})
  set_property(GLOBAL APPEND PROPERTY PPC_TEST_OBJS ${obj_file})
endfunction ()
