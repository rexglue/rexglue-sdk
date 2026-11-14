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

if (REXGLUE_REGISTER_PACKAGE)
  # Register in the CMake User Package Registry after install when REXGLUE_REGISTER_PACKAGE is ON.
  # This makes find_package(rexglue) work with no REXSDK env var or CMAKE_PREFIX_PATH.
  # Multiple SxS installs coexist. Each prefix gets a unique hash entry.
  # Windows keeps it in HKCU\Software\Kitware\CMake\Packages\rexglue (REG_SZ, value name = MD5 hash)
  # Unix keeps it in ~/.cmake/packages/rexglue/<hash> (file containing prefix path)
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
