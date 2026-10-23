# ReXGlue SDK Recompilation Toolkit
cmake_minimum_required(VERSION 3.25)

if (APPLE)
  set(REXGLUE_LANGUAGES C CXX OBJC OBJCXX)
else ()
  set(REXGLUE_LANGUAGES C CXX)
endif ()

project(
  ReXGlue
  VERSION 0.11.0
  LANGUAGES ${REXGLUE_LANGUAGES}
  DESCRIPTION "Xbox 360 Recompilation SDK"
)
# Floor only. Bump MAJOR.MINOR for API changes. Patch is derived from git tags
# in cmake/rex_version.cmake. See docs/RELEASING.md.

#==========================================================
# Build Options
#==========================================================
option(REXGLUE_BUILD_TESTS "Build unit tests" ON)
option(REXGLUE_BUILD_PPC_TESTS "Build PPC instruction tests" OFF)
option(REXGLUE_ENABLE_SANITIZERS "Enable UndefinedBehaviorSanitizer" OFF)
option(REXGLUE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(REXGLUE_ENABLE_TRACY "Enable Tracy profiler integration" ON)
option(REXGLUE_ENABLE_PERF_COUNTERS "Enable lightweight perf counter registry and overlay" ON)
# Propagate profiling defines globally (compiled out in Release)
if (REXGLUE_ENABLE_TRACY)
  add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif ()
if (REXGLUE_ENABLE_PERF_COUNTERS)
  add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif ()

if (WIN32)
  set(REXGLUE_GPU_D3D12 ON)
else ()
  set(REXGLUE_GPU_D3D12 OFF)
endif ()

# Root of the SDK source tree
set(REXGLUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# SDK install support
include(GNUInstallDirs)
file(RELATIVE_PATH REXGLUE_LIBDIR_FROM_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_LIBDIR}"
)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_target.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_module.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_metadata.cmake)

if (UNIX AND NOT APPLE)
  # Keep installed binaries self-contained by resolving shared libs relative
  # to the install prefix (for example, bin -> ../lib).
  set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
  list(APPEND CMAKE_INSTALL_RPATH "$ORIGIN" "$ORIGIN/${REXGLUE_LIBDIR_FROM_BINDIR}")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
elseif (APPLE)
  # Use loader-relative install names on macOS so installed binaries remain
  # self-contained within the install prefix.
  list(APPEND CMAKE_INSTALL_RPATH "@loader_path" "@loader_path/${REXGLUE_LIBDIR_FROM_BINDIR}")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif ()

# Enforce a Clang toolchain (LLVM Clang or Apple's Clang)
if (NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
  message(FATAL_ERROR "ReXGlue requires the Clang compiler (LLVM Clang or AppleClang). "
                      "Please set CMAKE_CXX_COMPILER to clang++"
  )
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
    message(FATAL_ERROR "ReXGlue requires Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
  endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.0")
    message(FATAL_ERROR "ReXGlue requires AppleClang 16 or newer / Xcode 16+ for C++23 support "
                        "(found ${CMAKE_CXX_COMPILER_VERSION})"
    )
  endif ()
endif ()

# Require a 64 bit architecture
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ReXGlue only supports 64-bit architectures (x86_64 or ARM64)")
endif ()

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX rd)

# CMAKE_<CONFIG>_POSTFIX applies to libraries only, so without this every config of an
# executable installs over the last one.
function (rex_config_postfix target)
  set_target_properties(
    ${target} PROPERTIES DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}" RELWITHDEBINFO_POSTFIX
                                                                "${CMAKE_RELWITHDEBINFO_POSTFIX}"
  )
endfunction ()

if (NOT WIN32)
  # Use the code model for the architecture
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64" AND NOT APPLE)
    # Large code model on Linux x86_64 for linking with large recompiled executables (35MB+).
    # macOS uses small: the ld64 linker on macOS 15+ rejects text relocations produced
    # by -mcmodel=large, and macOS maps binaries in the low address space so +/-2GB is sufficient.
    add_compile_options(-mcmodel=large)
  else ()
    add_compile_options(-mcmodel=small)
  endif ()
  # Position-independent code for all targets
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

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

if (NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-msse4.1)
endif ()

# Platform detection (supports AMD64 and ARM64)
set(REX_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if (APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES REX_OSX_ARCH_COUNT)
  if (REX_OSX_ARCH_COUNT GREATER 1)
    message(FATAL_ERROR "ReXGlue macOS presets expect a single CMAKE_OSX_ARCHITECTURES value")
  endif ()
  list(GET CMAKE_OSX_ARCHITECTURES 0 REX_TARGET_PROCESSOR)
endif ()

if (WIN32)
  if (REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
    set(REX_PLATFORM "win-amd64")
  elseif (REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    set(REX_PLATFORM "win-arm64")
  else ()
    message(FATAL_ERROR "Unsupported Windows architecture: ${REX_TARGET_PROCESSOR}")
  endif ()
  add_compile_definitions(REX_PLATFORM_WINDOWS=1)
elseif (UNIX AND NOT APPLE)
  if (REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
    set(REX_PLATFORM "linux-amd64")
  elseif (REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    set(REX_PLATFORM "linux-arm64")
  else ()
    set(REX_PLATFORM "linux-${REX_TARGET_PROCESSOR}")
  endif ()
  add_compile_definitions(REX_PLATFORM_LINUX=1)
elseif (APPLE)
  if (REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
    set(REX_PLATFORM "mac-amd64")
  elseif (REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    set(REX_PLATFORM "mac-arm64")
  else ()
    set(REX_PLATFORM "mac-${REX_TARGET_PROCESSOR}")
  endif ()
  add_compile_definitions(REX_PLATFORM_MAC=1)
else ()
  message(FATAL_ERROR "Unsupported platform. ReXGlue supports Windows, Linux, and macOS only.")
endif ()

# Build config as compile definition (multi-config generators don't know config at configure time)
add_compile_definitions(REXGLUE_BUILD_CONFIG="$<CONFIG>")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_pch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_version.cmake)
rex_resolve_version(
  REXGLUE_FULL_VERSION
  FLOOR_MAJOR
  ${PROJECT_VERSION_MAJOR}
  FLOOR_MINOR
  ${PROJECT_VERSION_MINOR}
  SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Strip the -id trailer (if any) for the numeric-only version.
string(REGEX REPLACE "-.*$" "" REXGLUE_NUMERIC_VERSION "${REXGLUE_FULL_VERSION}")

# Parse up to 4 numeric components for consumers that want integer access.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?" _
             "${REXGLUE_NUMERIC_VERSION}"
)
set(REXGLUE_DERIVED_MAJOR "${CMAKE_MATCH_1}")
set(REXGLUE_DERIVED_MINOR "${CMAKE_MATCH_2}")
if (CMAKE_MATCH_4)
  set(REXGLUE_DERIVED_PATCH "${CMAKE_MATCH_4}")
else ()
  set(REXGLUE_DERIVED_PATCH "0")
endif ()
if (CMAKE_MATCH_6)
  set(REXGLUE_DERIVED_TWEAK "${CMAKE_MATCH_6}")
else ()
  set(REXGLUE_DERIVED_TWEAK "0")
endif ()

set(REXGLUE_FLOOR_VERSION
    "${REXGLUE_DERIVED_MAJOR}.${REXGLUE_DERIVED_MINOR}.${REXGLUE_DERIVED_PATCH}"
)

rex_version_channel(REXGLUE_VERSION_CHANNEL VERSION "${REXGLUE_FULL_VERSION}")

# Generate version header. Must run after REX_PLATFORM is set. The nonce stays the same across
# reconfigures of one build tree, so the header only changes when the version does.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_build_nonce.cmake)
rexglue_get_build_nonce(REXGLUE_BUILD_NONCE)
set(REXGLUE_BUILD_IDENTITY "${REXGLUE_FULL_VERSION}-${REXGLUE_BUILD_NONCE}")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/rex/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/rex/version.h @ONLY
)

# The timestamp lives in one source of rexcore, so a reconfigure recompiles that file only.
string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/build_stamp.cpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/core/build_stamp.cpp @ONLY
)

# Output directories (multi-config appends /<Config>/ automatically)
set(REX_OUTPUT_DIR "${REXGLUE_ROOT}/out/${REX_PLATFORM}")
if (REXGLUE_ENABLE_TSAN)
  string(APPEND REX_OUTPUT_DIR "-tsan")
endif ()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")

# Sanitizer support for debugging memory corruption and undefined behavior
if (REXGLUE_ENABLE_SANITIZERS)
  message(
    STATUS
      "Enabling UndefinedBehaviorSanitizer (UBSan only - ASan conflicts with custom memory mapping)"
  )
  # ASan does not work with the custom mmap at 0x100000000, so only UBSan is used
  add_compile_options(-fsanitize=undefined -fno-sanitize=vptr -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=undefined -fno-sanitize=vptr)
endif ()

if (REXGLUE_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif ()

if (REXGLUE_BUILD_TESTS)
  list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/cmake")
  list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/thirdparty/catch2/extras")
endif ()

# Third party dependencies (vendored sources only)
add_subdirectory(thirdparty)

# Enable warnings after thirdparty
add_compile_options(
  -Wall -Wextra -Wshadow -Wshadow-field -Wshadow-field-in-constructor-modified -Werror
)

if (MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHs> $<$<COMPILE_LANGUAGE:CXX>:/EHc->)
endif ()

include_directories(${REXGLUE_ROOT}/include)

add_subdirectory(src/core)
add_subdirectory(src/filesystem)
add_subdirectory(src/imgui)
add_subdirectory(src/gpu)
add_subdirectory(src/gpu/xenos)
add_subdirectory(src/apu)
add_subdirectory(src/hid)
add_subdirectory(src/ui)
add_subdirectory(src/runtime)
add_subdirectory(src/ui/sdl3)
add_subdirectory(src/kernel)
add_subdirectory(src/codegen)
add_subdirectory(src/rexglue)
include(cmake/rexglue_sdk.cmake)

# Add tests subdirectory if tests are enabled
if (REXGLUE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif ()

include(cmake/rexglue_link_audit.cmake)

# Print configuration summary
message(STATUS "===========================================")
message(STATUS "ReXGlue Configuration Summary:")
message(STATUS "  Version: ${REXGLUE_FULL_VERSION} (${REXGLUE_VERSION_CHANNEL})")
message(STATUS "  Platform: ${REX_PLATFORM}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Configurations: ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Output Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  GPU: D3D12=${REXGLUE_GPU_D3D12} Vulkan=ON")
message(STATUS "  Tracy: ${REXGLUE_ENABLE_TRACY}")
message(STATUS "  Perf Counters: ${REXGLUE_ENABLE_PERF_COUNTERS}")
message(STATUS "===========================================")

include(cmake/rexglue_export_targets.cmake)

# SDK install/export rules (skipped when consumed as a subdirectory)
if (PROJECT_IS_TOP_LEVEL)
  include(cmake/rexglue_devenv.cmake)
  include(cmake/rexglue_install.cmake)
else ()
  # When consumed via add_subdirectory(), provide the same interface as find_package().
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_target.cmake)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_module.cmake)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_metadata.cmake)
  foreach (target IN LISTS REXGLUE_INSTALL_TARGETS)
    get_target_property(_export_name ${target} EXPORT_NAME)
    if (NOT _export_name)
      set(_export_name ${target})
    endif ()
    if (NOT TARGET rex::${_export_name})
      add_library(rex::${_export_name} ALIAS ${target})
    endif ()
  endforeach ()
endif ()
