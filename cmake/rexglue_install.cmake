#==========================================================
# SDK Install/Export Rules
#==========================================================
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_target.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_module.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_metadata.cmake)

install(
  TARGETS ${REXGLUE_INSTALL_TARGETS}
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

# A Debug codegen tool runs an order of magnitude slower, so only Release is installed.
install(
  TARGETS rexglue
  EXPORT rexglueTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} CONFIGURATIONS Release
)

# Install public headers
install(DIRECTORY include/rex DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install generated version header
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/rex/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rex
)

# Install vendored header-only library headers
install(
  DIRECTORY thirdparty/renderdoc/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/renderdoc
  FILES_MATCHING
  PATTERN "*.h"
)
install(DIRECTORY thirdparty/tomlplusplus/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  DIRECTORY thirdparty/simde/simde
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h"
)
install(FILES thirdparty/xxHash/xxhash.h thirdparty/xxHash/xxh3.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES thirdparty/o1heap/o1heap/o1heap.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES thirdparty/imgui/imgui.h thirdparty/imgui/imconfig.h thirdparty/imgui/imgui_internal.h
              thirdparty/imgui/imstb_rectpack.h thirdparty/imgui/imstb_textedit.h
              thirdparty/imgui/imstb_truetype.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install SPIRV-Tools headers (only the public API, not opt/linker)
install(FILES thirdparty/spirv-tools/include/spirv-tools/libspirv.h
              thirdparty/spirv-tools/include/spirv-tools/libspirv.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/spirv-tools
)

# Install DXC API headers (vendored, for D3D12 backend)
if (REXGLUE_GPU_D3D12)
  install(FILES thirdparty/dxc/include/DxbcConverter.h thirdparty/dxc/include/dxcapi.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dxc
  )
endif ()

if (APPLE)
  install(
    FILES "$<TARGET_FILE:Vulkan::Loader>"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    CONFIGURATIONS Release
    RENAME libvulkan.1.dylib
  )
  install(
    FILES "$<TARGET_FILE:MoltenVK::MoltenVK>"
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

if (REXGLUE_REGISTER_PACKAGE)
  # Register in the CMake User Package Registry after install when REXGLUE_REGISTER_PACKAGE is ON.
  # This makes find_package(rexglue) work with no REXSDK env var or CMAKE_PREFIX_PATH.
  # Multiple SxS installs coexist. Each prefix gets a unique hash entry.
  #
  # Windows: HKCU\Software\Kitware\CMake\Packages\rexglue  (REG_SZ, value name = MD5 hash)
  # Unix:    ~/.cmake/packages/rexglue/<hash>               (file containing prefix path)
  install(
    CODE [[
      # Normalize path casing on Windows before hashing to avoid duplicate entries
      if(CMAKE_HOST_WIN32)
          string(TOLOWER "${CMAKE_INSTALL_PREFIX}" _reg_key)
      else()
          set(_reg_key "${CMAKE_INSTALL_PREFIX}")
      endif()
      string(MD5 _hash "${_reg_key}")

      if(CMAKE_HOST_WIN32)
          # Windows CMake User Package Registry is stored in HKCU (not the filesystem)
          set(_reg_root "HKCU\\Software\\Kitware\\CMake\\Packages\\rexglue")
          execute_process(
              COMMAND reg add "${_reg_root}" /v "${_hash}" /t REG_SZ /d "${CMAKE_INSTALL_PREFIX}" /f
              OUTPUT_QUIET ERROR_QUIET
          )
      else()
          set(_reg_dir "$ENV{HOME}/.cmake/packages/rexglue")
          file(MAKE_DIRECTORY "${_reg_dir}")
          file(WRITE "${_reg_dir}/${_hash}" "${CMAKE_INSTALL_PREFIX}")
      endif()
      message(STATUS "Registered rexglue in CMake user package registry")
      message(STATUS "  -> ${CMAKE_INSTALL_PREFIX}")
  ]]
  )
endif ()
