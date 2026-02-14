/**
 * @file        platform/dynlib.h
 * @brief       Platform-agnostic dynamic library loading
 */
#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/platform.h>

namespace rex::platform {

class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  bool Load(const std::filesystem::path& path);
  void Close();
  explicit operator bool() const { return handle_ != nullptr; }

  void* GetRawSymbol(const char* name) const;

  template <typename T>
  T GetSymbol(const char* name) const {
    return reinterpret_cast<T>(GetRawSymbol(name));
  }

 private:
  void* handle_ = nullptr;
};

namespace lib_names {

#if REX_PLATFORM_WIN32

inline constexpr const char* kVulkanLoader = "vulkan-1.dll";
inline constexpr const char* kRenderDoc = "renderdoc.dll";
inline constexpr const char* kSpirvToolsSdkPath = "Bin/SPIRV-Tools-shared.dll";

#elif REX_PLATFORM_ANDROID

inline constexpr const char* kVulkanLoader = "libvulkan.so";
inline constexpr const char* kRenderDoc = "libVkLayer_GLES_RenderDoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_LINUX

inline constexpr const char* kVulkanLoader = "libvulkan.so.1";
inline constexpr const char* kRenderDoc = "librenderdoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#else
#error No library names provided for the target platform.
#endif

}  // namespace lib_names

}  // namespace rex::platform
