#==========================================================
# SDK Install/Export Rules
#==========================================================
set_target_properties(rexcore PROPERTIES EXPORT_NAME core)
set_target_properties(rexfilesystem PROPERTIES EXPORT_NAME filesystem)
set_target_properties(rexui PROPERTIES EXPORT_NAME ui)
set_target_properties(rexinput PROPERTIES EXPORT_NAME input)
set_target_properties(rexaudio PROPERTIES EXPORT_NAME audio)
set_target_properties(rexgraphics PROPERTIES EXPORT_NAME graphics)
set_target_properties(rexkernel PROPERTIES EXPORT_NAME kernel)
set_target_properties(rexruntime PROPERTIES EXPORT_NAME runtime)
set_target_properties(rexcodegen PROPERTIES EXPORT_NAME codegen)

# Build install target list dynamically based on backend options
set(REXGLUE_INSTALL_TARGETS
    # ReX SDK libraries
    rexcore rexfilesystem rexui rexinput
    rexaudio rexgraphics rexkernel rexruntime rexcodegen
    # Vendored thirdparty libraries (required by SDK)
    disruptorplus renderdoc simde tomlplusplus  # INTERFACE (header-only)
    aes128 mspack disasm xxhash imgui  # STATIC libraries
    libavcodec libavutil           # FFmpeg (vendored build)
    # CLI tool
    rexglue
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGLUE_INSTALL_TARGETS
        SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler  # glslang
        SPIRV-Tools-static
    )
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGLUE_INSTALL_TARGETS dxc-headers)
endif()

install(TARGETS ${REXGLUE_INSTALL_TARGETS}
    EXPORT rexglueTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install public headers
install(DIRECTORY include/rex
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install vendored header-only library headers
install(DIRECTORY thirdparty/disruptorplus/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/disruptorplus
)
install(DIRECTORY thirdparty/renderdoc/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/renderdoc
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY thirdparty/tomlplusplus/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY thirdparty/simde/simde
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(FILES
    thirdparty/xxHash/xxhash.h
    thirdparty/xxHash/xxh3.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    thirdparty/imgui/imgui.h
    thirdparty/imgui/imconfig.h
    thirdparty/imgui/imgui_internal.h
    thirdparty/imgui/imstb_rectpack.h
    thirdparty/imgui/imstb_textedit.h
    thirdparty/imgui/imstb_truetype.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install SPIRV-Tools headers (only the public API, not opt/linker)
if(REXGLUE_USE_VULKAN)
    install(FILES
        thirdparty/spirv-tools/include/spirv-tools/libspirv.h
        thirdparty/spirv-tools/include/spirv-tools/libspirv.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/spirv-tools
    )
endif()

# Install platform entry point sources for SDK consumers
install(FILES
    src/ui/windowed_app_main_win.cpp
    src/ui/windowed_app_main_posix.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rexglue
)

# Install DXC API headers (vendored, for D3D12 backend)
if(REXGLUE_USE_D3D12)
    install(FILES
        thirdparty/dxc/include/DxbcConverter.h
        thirdparty/dxc/include/dxcapi.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dxc
    )
endif()

# Generate and install package config files
configure_package_config_file(
    cmake/rexglueConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rexglueConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)

# Export targets with rex:: namespace
install(EXPORT rexglueTargets
    FILE rexglueTargets.cmake
    NAMESPACE rex::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rexglue
)
