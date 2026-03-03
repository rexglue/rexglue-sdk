# ReXGlue SDK - Xbox 360 Recompilation Toolkit
# Modern C++23 cross-platform SDK for recompiling Xbox 360 executables
cmake_minimum_required(VERSION 3.25)

if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    execute_process(
        COMMAND /usr/sbin/sysctl -in hw.optional.arm64
        OUTPUT_VARIABLE REXGLUE_HW_ARM64
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(REXGLUE_HW_ARM64 STREQUAL "1")
        # Prefer a native Apple Silicon target even if the cmake executable
        # itself is running as x86_64.
        set(CMAKE_APPLE_SILICON_PROCESSOR "arm64" CACHE STRING "" FORCE)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target macOS architectures" FORCE)
    endif()
endif()

project(ReXGlue
    VERSION 0.2.2
    LANGUAGES C CXX
    DESCRIPTION "Xbox 360 Recompilation SDK"
)

if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    enable_language(ASM)
    string(APPEND CMAKE_ASM_FLAGS " -march=armv8-a")
endif()

#==========================================================
# Build Options
#==========================================================
option(REXGLUE_BUILD_TESTS       "Build PPC instruction and unit tests" OFF)
option(REXGLUE_ENABLE_SANITIZERS "Enable UndefinedBehaviorSanitizer" OFF)

# Graphics backend selection
# D3D12: Windows only, ON by default.
# Vulkan: Cross-platform, ON by default on non-Windows.
# Metal: macOS only, ON by default on Apple platforms.
if(WIN32)
    option(REXGLUE_USE_D3D12  "Enable D3D12 graphics backend" ON)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" OFF)
    set(REXGLUE_USE_METAL OFF)
elseif(APPLE)
    set(REXGLUE_USE_D3D12 OFF)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" ON)
    option(REXGLUE_USE_METAL  "Enable Metal graphics backend" ON)
else()
    set(REXGLUE_USE_D3D12 OFF)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" ON)
    set(REXGLUE_USE_METAL OFF)
endif()

if(WIN32 AND NOT REXGLUE_USE_D3D12 AND NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "At least one graphics backend must be enabled on Windows (REXGLUE_USE_D3D12 or REXGLUE_USE_VULKAN)")
elseif(APPLE AND NOT REXGLUE_USE_VULKAN AND NOT REXGLUE_USE_METAL)
    message(FATAL_ERROR "At least one graphics backend must be enabled on macOS (REXGLUE_USE_VULKAN or REXGLUE_USE_METAL)")
elseif(NOT WIN32 AND NOT APPLE AND NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "At least one graphics backend must be enabled on this platform (REXGLUE_USE_VULKAN)")
endif()

# Root of the SDK source tree
set(REXGLUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# SDK install support
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Enforce Clang-family compiler
if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR "ReXGlue requires Clang-family compiler. Please set CMAKE_CXX_COMPILER to clang++")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
    message(FATAL_ERROR "ReXGlue requires Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# Enforce 64-bit architecture
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ReXGlue only supports 64-bit targets")
endif()

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX rd)

if(UNIX AND NOT APPLE)
    # TODO: do we actually need this?
    # Use the large code model on x86_64 for very large recompiled executables.
    # AArch64 does not support it, so keep the small model there instead.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        add_compile_options(-mcmodel=large)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        add_compile_options(-mcmodel=small)
    endif()
endif()

if(NOT WIN32)
    # Position-independent code for non-Windows targets.
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
elseif(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES REXGLUE_APPLE_ARCH_COUNT)
        if(REXGLUE_APPLE_ARCH_COUNT GREATER 1)
            set(REXGLUE_APPLE_TARGET_ARCH "universal")
        else()
            list(GET CMAKE_OSX_ARCHITECTURES 0 REXGLUE_APPLE_TARGET_ARCH)
        endif()
    else()
        set(REXGLUE_APPLE_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(REX_PLATFORM "macos-${REXGLUE_APPLE_TARGET_ARCH}")
    add_compile_definitions(REX_PLATFORM_MAC=1)
elseif(UNIX AND NOT APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(REX_PLATFORM "linux-arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        set(REX_PLATFORM "linux-amd64")
    else()
        set(REX_PLATFORM "linux-${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_LINUX=1)
else()
    message(FATAL_ERROR "Unsupported platform. ReXGlue supports Windows, Linux, and macOS.")
endif()

# Output directories (multi-config appends /<Config>/ automatically)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REXGLUE_ROOT}/out/${REX_PLATFORM}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${REXGLUE_ROOT}/out/${REX_PLATFORM}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${REXGLUE_ROOT}/out/${REX_PLATFORM}")

# Sanitizer support for debugging memory corruption and undefined behavior
if(REXGLUE_ENABLE_SANITIZERS)
    message(STATUS "Enabling UndefinedBehaviorSanitizer (UBSan only - ASan conflicts with custom memory mapping)")
    # Note: ASan doesn't work well with the custom mmap at 0x100000000, so we only use UBSan
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

if(REXGLUE_BUILD_TESTS)
    list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/thirdparty/catch2/extras")
endif()

# Third-party dependencies (vendored sources only)
add_subdirectory(thirdparty)

# Enable warnings after thirdparty
add_compile_options(
    -Wall 
    -Wextra
)

include_directories(${REXGLUE_ROOT}/include)

add_subdirectory(src/core)
add_subdirectory(src/filesystem)
add_subdirectory(src/ui)
add_subdirectory(src/input)
add_subdirectory(src/audio)
add_subdirectory(src/graphics)
add_subdirectory(src/system)
add_subdirectory(src/kernel)
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
if(APPLE)
    message(STATUS "  macOS Arch: ${CMAKE_OSX_ARCHITECTURES}")
endif()
message(STATUS "  Configurations: ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Output Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Graphics: D3D12=${REXGLUE_USE_D3D12} Vulkan=${REXGLUE_USE_VULKAN} Metal=${REXGLUE_USE_METAL}")
message(STATUS "===========================================")

# SDK install/export rules (skipped when consumed as a subdirectory)
if(PROJECT_IS_TOP_LEVEL)
    include(cmake/rexglue_install.cmake)
else()
    # When consumed via add_subdirectory(), provide the same interface as find_package().
    # This sets REXGLUE_SHARE_DIR and makes rexglue_configure_target() available.
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui" PARENT_SCOPE)
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)
endif()
