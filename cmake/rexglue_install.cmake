# SDK Install/Export Rules
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_target.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_module.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_metadata.cmake)

install(CODE "file(REMOVE_RECURSE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/include/rex\")")

set(_header_set_targets "")
set(_plain_targets "")
foreach (_install_target IN LISTS REXGLUE_INSTALL_TARGETS)
  get_target_property(_header_sets ${_install_target} INTERFACE_HEADER_SETS)
  if (_header_sets)
    list(APPEND _header_set_targets ${_install_target})
  else ()
    list(APPEND _plain_targets ${_install_target})
  endif ()
endforeach ()

set(_header_set_install FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  TARGETS ${_header_set_targets}
  EXPORT rexglueTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} ${_header_set_install}
)
install(
  TARGETS ${_plain_targets}
  EXPORT rexglueTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if (WIN32)
  foreach (_install_target IN LISTS REXGLUE_INSTALL_TARGETS)
    get_target_property(_install_type ${_install_target} TYPE)
    if (_install_type STREQUAL "SHARED_LIBRARY" OR _install_type STREQUAL "MODULE_LIBRARY")
      install(
        FILES $<TARGET_PDB_FILE:${_install_target}>
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        CONFIGURATIONS Debug RelWithDebInfo
        OPTIONAL
      )
    endif ()
  endforeach ()
endif ()

set(REXGLUE_RUNTIME_FILE_TARGETS ${REXGLUE_MODULE_TARGETS})
set(REXGLUE_IMPORTED_MODULES "")
set(REXGLUE_MODULES_CONTENT "")
string(ASCII 10 _nl)
# Windows keeps DLLs in bin. Linux and macOS keep shared libraries in lib.
if (WIN32)
  set(_module_dir ${CMAKE_INSTALL_BINDIR})
else ()
  set(_module_dir ${CMAKE_INSTALL_LIBDIR})
endif ()
set(_config_upper "$<UPPER_CASE:$<CONFIG>>")
foreach (_module IN LISTS REXGLUE_RUNTIME_FILE_TARGETS)
  get_target_property(_export_name ${_module} EXPORT_NAME)
  set(_imported rex::${_export_name})
  list(APPEND REXGLUE_IMPORTED_MODULES ${_imported})

  if (WIN32)
    install(FILES $<TARGET_FILE:${_module}> DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(
      FILES $<TARGET_PDB_FILE:${_module}>
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      CONFIGURATIONS Debug RelWithDebInfo
      OPTIONAL
    )
  else ()
    install(TARGETS ${_module} LIBRARY DESTINATION ${_module_dir} NAMELINK_SKIP)
  endif ()

  set(_set "  set_property(TARGET ${_imported}")
  set(_location "\${PACKAGE_PREFIX_DIR}/${_module_dir}/$<TARGET_FILE_NAME:${_module}>")
  set(_soname "$<TARGET_SONAME_FILE_NAME:${_module}>")
  string(APPEND REXGLUE_MODULES_CONTENT
         "if (\"${_imported}\" IN_LIST _rexglue_created_targets)${_nl}"
  )
  string(APPEND REXGLUE_MODULES_CONTENT "${_set} APPEND PROPERTY IMPORTED_CONFIGURATIONS ")
  string(APPEND REXGLUE_MODULES_CONTENT "${_config_upper})${_nl}")
  string(APPEND REXGLUE_MODULES_CONTENT "${_set} PROPERTY IMPORTED_LOCATION_${_config_upper} ")
  string(APPEND REXGLUE_MODULES_CONTENT "\"${_location}\")${_nl}")
  if (NOT WIN32 AND NOT APPLE)
    string(APPEND REXGLUE_MODULES_CONTENT "${_set} PROPERTY IMPORTED_SONAME_${_config_upper} ")
    string(APPEND REXGLUE_MODULES_CONTENT "\"${_soname}\")${_nl}")
  endif ()
  string(APPEND REXGLUE_MODULES_CONTENT "endif ()${_nl}")
endforeach ()

file(
  GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rexglueModules-$<CONFIG>.cmake
  CONTENT "${REXGLUE_MODULES_CONTENT}"
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rexglueModules-$<CONFIG>.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)

# A Debug codegen tool runs an order of magnitude slower, so only Release is installed.
install(
  TARGETS rexglue
  EXPORT rexglueTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} CONFIGURATIONS Release
)
# The tool includes the GNU binutils PowerPC disassembler and is distributed under GPL 2.0 or later.
install(
  FILES docs/licenses/disasm/COPYING
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  CONFIGURATIONS Release
  RENAME rexglue-COPYING.txt
)
install(
  FILES src/rexglue/NOTICE
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  CONFIGURATIONS Release
  RENAME rexglue-NOTICE.txt
)

# Install public headers
install(DIRECTORY include/rex DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install generated version header
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/rex/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rex
)

# Install vendored header-only library headers
install(
  DIRECTORY thirdparty/simde/simde
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h"
)
install(FILES thirdparty/imgui/imgui.h thirdparty/imgui/imconfig.h thirdparty/imgui/imgui_internal.h
              thirdparty/imgui/imstb_rectpack.h thirdparty/imgui/imstb_textedit.h
              thirdparty/imgui/imstb_truetype.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# The license files of what the install includes, as the page appendix/third-party.md lists them.
set(_licenses licenses)
install(FILES thirdparty/libmspack/libmspack/COPYING.LIB DESTINATION ${_licenses}/libmspack)
install(FILES thirdparty/glslang/LICENSE.txt DESTINATION ${_licenses}/glslang)
install(FILES thirdparty/FFmpeg/COPYING.LGPLv2.1 thirdparty/FFmpeg/LICENSE.md
        DESTINATION ${_licenses}/FFmpeg
)
install(FILES thirdparty/tomlplusplus/LICENSE DESTINATION ${_licenses}/tomlplusplus)
install(FILES thirdparty/simde/COPYING DESTINATION ${_licenses}/simde)
install(FILES thirdparty/xxHash/LICENSE DESTINATION ${_licenses}/xxHash)
install(FILES thirdparty/imgui/LICENSE.txt DESTINATION ${_licenses}/imgui)
install(FILES thirdparty/cli11/LICENSE DESTINATION ${_licenses}/cli11)
install(FILES thirdparty/sdl3/LICENSE.txt DESTINATION ${_licenses}/SDL3)
if (NOT WIN32 OR MINGW)
  # SDL installs a second copy of its license under the data folder here. The copy above is the
  # one the install keeps.
  install(
    CODE "
    set(_sdl_copy \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/licenses\")
    file(REMOVE_RECURSE \"\${_sdl_copy}/SDL3\")
    file(GLOB _sdl_left \"\${_sdl_copy}/*\")
    if (NOT _sdl_left)
      file(REMOVE_RECURSE \"\${_sdl_copy}\")
    endif ()
    get_filename_component(_sdl_data \"\${_sdl_copy}\" DIRECTORY)
    file(GLOB _sdl_left \"\${_sdl_data}/*\")
    if (NOT _sdl_left)
      file(REMOVE_RECURSE \"\${_sdl_data}\")
    endif ()
    "
  )
endif ()
install(FILES thirdparty/inja/LICENSE DESTINATION ${_licenses}/inja)
install(FILES thirdparty/excrypt/LICENSE DESTINATION ${_licenses}/excrypt)
install(FILES thirdparty/spdlog/LICENSE DESTINATION ${_licenses}/spdlog)
install(FILES thirdparty/VulkanMemoryAllocator/LICENSE.txt
        DESTINATION ${_licenses}/VulkanMemoryAllocator
)
install(FILES thirdparty/Vulkan-Headers/LICENSE.md thirdparty/Vulkan-Headers/LICENSES/Apache-2.0.txt
              thirdparty/Vulkan-Headers/LICENSES/MIT.txt DESTINATION ${_licenses}/Vulkan-Headers
)
install(
  FILES docs/licenses/disasm/COPYING
  DESTINATION ${_licenses}/disasm
  CONFIGURATIONS Release
)
if (REXGLUE_ENABLE_TRACY)
  install(FILES thirdparty/tracy/LICENSE DESTINATION ${_licenses}/tracy)
endif ()
install(FILES LICENSE DESTINATION ${_licenses})
install(FILES thirdparty/inja/third_party/include/nlohmann/LICENSE.MIT
        DESTINATION ${_licenses}/nlohmann-json
)
install(FILES docs/licenses/tlsf/LICENSE DESTINATION ${_licenses}/tlsf)
install(FILES docs/licenses/renderdoc/LICENSE DESTINATION ${_licenses}/renderdoc)
install(FILES docs/licenses/stb/LICENSE DESTINATION ${_licenses}/stb)
install(FILES docs/licenses/ProggyFonts/LICENSE DESTINATION ${_licenses}/ProggyFonts)
if (REXGLUE_GPU_D3D12)
  install(FILES docs/licenses/dxbc/LICENSE DESTINATION ${_licenses}/dxbc)
  install(FILES docs/licenses/dxc/LICENSE.TXT DESTINATION ${_licenses}/dxc)
endif ()
if (APPLE)
  install(
    DIRECTORY docs/licenses/Vulkan-Loader/
    DESTINATION ${_licenses}/Vulkan-Loader
    CONFIGURATIONS Release
  )
  install(
    FILES docs/licenses/MoltenVK/LICENSE
    DESTINATION ${_licenses}/MoltenVK
    CONFIGURATIONS Release
  )
endif ()

if (APPLE)
  install(
    FILES "$<TARGET_FILE:Vulkan::Vulkan>"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    CONFIGURATIONS Release
    RENAME libvulkan.1.dylib
  )
  install(
    FILES "$<TARGET_FILE:Vulkan::MoltenVK>"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    CONFIGURATIONS Release
    RENAME libMoltenVK.dylib
  )
  install(
    FILES cmake/MoltenVK_icd.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/icd.d
    CONFIGURATIONS Release
  )
endif ()

# Generate and install package config files
configure_package_config_file(
  cmake/rexglueConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfigVersion.cmake
  VERSION ${REXGLUE_NUMERIC_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)

install(
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_target.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_codegen.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_recomp_options.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_module.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_metadata.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_project.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_stdlib.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_stage_pdbs.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_optimus.cpp.in
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_host_alloc_stub.cpp
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)

# Export targets with rex:: namespace
install(
  EXPORT rexglueTargets
  FILE rexglueTargets.cmake
  NAMESPACE rex::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)

# Register in the CMake User Package Registry after install.
# This makes find_package(rexglue) work with no REXSDK env var or CMAKE_PREFIX_PATH.
# Multiple SxS installs coexist. Each prefix gets a unique hash entry.
# Windows keeps it in HKCU\Software\Kitware\CMake\Packages\rexglue (REG_SZ, value name = MD5 hash)
# Unix keeps it in ~/.cmake/packages/rexglue/<hash> (file containing prefix path)
install(
  CODE [[
    # A relative prefix, as in cmake --install --prefix ../x, is made absolute against the folder
    # the install runs from. Windows paths are lower cased before hashing to avoid duplicates.
    cmake_path(ABSOLUTE_PATH CMAKE_INSTALL_PREFIX NORMALIZE OUTPUT_VARIABLE _reg_prefix)
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${_reg_prefix}" _reg_key)
    else()
        set(_reg_key "${_reg_prefix}")
    endif()
    string(MD5 _hash "${_reg_key}")

    if(CMAKE_HOST_WIN32)
        # Windows CMake User Package Registry is stored in HKCU (not the filesystem)
        set(_reg_root "HKCU\\Software\\Kitware\\CMake\\Packages\\rexglue")
        execute_process(
            COMMAND reg add "${_reg_root}" /v "${_hash}" /t REG_SZ /d "${_reg_prefix}" /f
            OUTPUT_QUIET ERROR_QUIET
        )
    else()
        set(_reg_dir "$ENV{HOME}/.cmake/packages/rexglue")
        file(MAKE_DIRECTORY "${_reg_dir}")
        file(WRITE "${_reg_dir}/${_hash}" "${_reg_prefix}")
    endif()
    message(STATUS "Registered rexglue in CMake user package registry")
    message(STATUS "  -> ${_reg_prefix}")
]]
)
