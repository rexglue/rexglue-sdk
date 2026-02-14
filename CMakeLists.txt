# ReXGlue SDK - Xbox 360 Recompilation Toolkit
# Modern C++23 cross-platform SDK for recompiling Xbox 360 executables
cmake_minimum_required(VERSION 3.25)

project(ReXGlue
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Xbox 360 Recompilation SDK"
)

#==========================================================
# Build Options
#==========================================================
option(REXGLUE_BUILD_TESTS       "Build PPC instruction and unit tests" OFF)
option(REXGLUE_ENABLE_SANITIZERS "Enable UndefinedBehaviorSanitizer" OFF)

# Graphics backend selection
# D3D12: Windows only, ON by default.
# Vulkan: Cross-platform, ON by default on Linux, OFF by default on Windows.
if(WIN32)
    option(REXGLUE_USE_D3D12  "Enable D3D12 graphics backend" ON)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" OFF)
else()
    set(REXGLUE_USE_D3D12 OFF)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" ON)
endif()

if(NOT REXGLUE_USE_D3D12 AND NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "At least one graphics backend must be enabled (REXGLUE_USE_D3D12 or REXGLUE_USE_VULKAN)")
endif()

# SDK install support
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Enforce Clang18 compiler
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "ReXGlue requires Clang compiler. Please set CMAKE_CXX_COMPILER to clang++")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
    message(FATAL_ERROR "ReXGlue requires Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# Enforce x64 architecture
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ReXGlue only supports x64 architecture")
endif()

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX rd)

# Position-independent code for all targets


# Use large code model on Linux for linking with very large recompiled executables (35MB+)
if(NOT WIN32)
    add_compile_options(-mcmodel=large)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Global compile options
add_compile_options(
    -fno-strict-aliasing
    -ffp-model=strict
    $<$<CONFIG:DEBUG>:-g>
    $<$<CONFIG:DEBUG>:-O0>
    $<$<CONFIG:RELEASE>:-O3>
    $<$<CONFIG:RELEASE>:-DNDEBUG>
    -fno-char8_t
)

# Platform detection
if(WIN32)
    set(REX_PLATFORM "win-amd64")
    add_compile_definitions(REX_PLATFORM_WINDOWS=1)
elseif(UNIX AND NOT APPLE)
    set(REX_PLATFORM "linux-amd64")
    add_compile_definitions(REX_PLATFORM_LINUX=1)
else()
    message(FATAL_ERROR "Unsupported platform. ReXGlue supports Windows and Linux only.")
endif()

# Output directories (multi-config appends /<Config>/ automatically)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/${REX_PLATFORM}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/${REX_PLATFORM}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/${REX_PLATFORM}")

# Sanitizer support for debugging memory corruption and undefined behavior
if(REXGLUE_ENABLE_SANITIZERS)
    message(STATUS "Enabling UndefinedBehaviorSanitizer (UBSan only - ASan conflicts with custom memory mapping)")
    # Note: ASan doesn't work well with the custom mmap at 0x100000000, so we only use UBSan
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

if(REXGLUE_BUILD_TESTS)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/thirdparty/catch2/extras")
endif()

# Third-party dependencies (vendored sources only)
add_subdirectory(thirdparty)

# Enable warnings after thirdparty
add_compile_options(
    -Wall 
    -Wextra
)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_subdirectory(src/core)
add_subdirectory(src/filesystem)
add_subdirectory(src/ui)
add_subdirectory(src/input)
add_subdirectory(src/audio)
add_subdirectory(src/graphics)
add_subdirectory(src/kernel)
add_subdirectory(src/runtime)
add_subdirectory(src/codegen)    
add_subdirectory(src/rexglue)

# Add tests subdirectory if tests are enabled
if(REXGLUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Print configuration summary
message(STATUS "===========================================")
message(STATUS "ReXGlue Configuration Summary:")
message(STATUS "  Platform: ${REX_PLATFORM}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Configurations: ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Output Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Graphics: D3D12=${REXGLUE_USE_D3D12} Vulkan=${REXGLUE_USE_VULKAN}")
message(STATUS "===========================================")

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

# Install all library targets, vendored deps, and the rexglue CLI tool
install(TARGETS
    # Rex SDK libraries
    rexcore rexfilesystem rexui rexinput
    rexaudio rexgraphics rexkernel rexruntime rexcodegen
    # Vendored thirdparty libraries (required by SDK)
    disruptorplus renderdoc simde tomlplusplus  # INTERFACE (header-only)
    aes128 mspack disasm xxhash imgui  # STATIC libraries
    libavcodec libavutil           # FFmpeg (vendored build)
    SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler  # glslang (vendored build)
    SPIRV-Tools-static             # SPIRV-Tools (only static lib, install handled here)
    # CLI tool
    rexglue
    EXPORT rexglueTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# DXC headers target (Windows only, required by rexui export)
if(WIN32)
    install(TARGETS dxc-headers
        EXPORT rexglueTargets
    )
endif()

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
install(FILES
    thirdparty/spirv-tools/include/spirv-tools/libspirv.h
    thirdparty/spirv-tools/include/spirv-tools/libspirv.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/spirv-tools
)

# Install platform entry point sources for SDK consumers
install(FILES
    src/ui/windowed_app_main_win.cpp
    src/ui/windowed_app_main_posix.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rexglue
)

# Install DXC API headers (vendored, Windows only, for D3D12 backend)
if(WIN32)
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
