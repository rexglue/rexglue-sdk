/**
 * @file        platform/dynlib.h
 * @brief       Dynamic library loading with OS error text
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <rex/platform.h>

namespace rex::platform {

/**
 * Controls when a loaded library's imported symbols are resolved.
 */
enum class SymbolResolution {
  /**
   * Resolve symbols on first use. Maps to RTLD_LAZY on POSIX; the only mode
   * on Windows.
   */
  kLazy,
  /**
   * Resolve all symbols at load time. Load fails if any unresolved symbol
   * exists. Maps to RTLD_NOW on POSIX; the only mode on Windows.
   */
  kImmediate,
};

/**
 * Loads a shared library and resolves symbols from it. Move only.
 */
class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  /**
   * Loads the library at the path, closing any library held before.
   *
   * @param path Full path to the library file.
   * @param mode Symbol resolution mode. Informational on Windows.
   * @return True on success. On failure last_error() holds the OS text.
   */
  bool Load(const std::filesystem::path& path, SymbolResolution mode = SymbolResolution::kLazy);

  /**
   * Closes the library. Safe when nothing is loaded.
   */
  void Close();

  /**
   * @return True when a library is loaded.
   */
  explicit operator bool() const { return handle_ != nullptr; }

  /**
   * @param name Exported symbol name.
   * @return The symbol address, or null when the library is not loaded or has no such symbol.
   */
  void* GetRawSymbol(const char* name) const;

  /**
   * Typed form of GetRawSymbol.
   */
  template <typename T>
  T GetSymbol(const char* name) const {
    return reinterpret_cast<T>(GetRawSymbol(name));
  }

  /**
   * @return The OS error text of the last failed Load, empty after a successful Load.
   */
  const std::string& last_error() const { return last_error_; }

 private:
  void* handle_ = nullptr;
  std::string last_error_;
};

namespace lib_names {

#if REX_PLATFORM_WIN32

/**
 * File name of the Vulkan loader library.
 */
inline constexpr const char* kVulkanLoader = "vulkan-1.dll";
/**
 * File name of the RenderDoc capture library.
 */
inline constexpr const char* kRenderDoc = "renderdoc.dll";
/**
 * Path to the SPIRV-Tools shared library within the SDK install.
 */
inline constexpr const char* kSpirvToolsSdkPath = "Bin/SPIRV-Tools-shared.dll";

#elif REX_PLATFORM_ANDROID

/**
 * File name of the Vulkan loader library.
 */
inline constexpr const char* kVulkanLoader = "libvulkan.so";
/**
 * File name of the RenderDoc capture library.
 */
inline constexpr const char* kRenderDoc = "libVkLayer_GLES_RenderDoc.so";
/**
 * Path to the SPIRV-Tools shared library within the SDK install.
 */
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_LINUX

/**
 * File name of the Vulkan loader library.
 */
inline constexpr const char* kVulkanLoader = "libvulkan.so.1";
/**
 * File name of the RenderDoc capture library.
 */
inline constexpr const char* kRenderDoc = "librenderdoc.so";
/**
 * Path to the SPIRV-Tools shared library within the SDK install.
 */
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_MAC

/**
 * File name of the Vulkan loader library.
 */
inline constexpr const char* kVulkanLoader = "libvulkan.1.dylib";
/**
 * File name of the RenderDoc capture library.
 */
inline constexpr const char* kRenderDoc = "librenderdoc.dylib";
/**
 * Path to the SPIRV-Tools shared library within the SDK install.
 */
inline constexpr const char* kSpirvToolsSdkPath = "lib/libSPIRV-Tools-shared.dylib";

#else
#error No library names provided for the target platform.
#endif

}  // namespace lib_names

}  // namespace rex::platform
