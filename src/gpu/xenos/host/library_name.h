/**
 * @file        gpu/xenos/host/library_name.h
 * @brief       File names of the host libraries the Xenos host layer loads
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

namespace rex::gpu {

#if REX_PLATFORM_WIN32

inline constexpr const char* kVulkanLoader = "vulkan-1.dll";
inline constexpr const char* kRenderDoc = "renderdoc.dll";
inline constexpr const char* kSPIRVToolsSDKPath = "Bin/SPIRV-Tools-shared.dll";

#elif REX_PLATFORM_ANDROID

inline constexpr const char* kVulkanLoader = "libvulkan.so";
inline constexpr const char* kRenderDoc = "libVkLayer_GLES_RenderDoc.so";
inline constexpr const char* kSPIRVToolsSDKPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_LINUX

inline constexpr const char* kVulkanLoader = "libvulkan.so.1";
inline constexpr const char* kRenderDoc = "librenderdoc.so";
inline constexpr const char* kSPIRVToolsSDKPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_MAC

inline constexpr const char* kVulkanLoader = "libvulkan.1.dylib";
inline constexpr const char* kRenderDoc = "librenderdoc.dylib";
inline constexpr const char* kSPIRVToolsSDKPath = "lib/libSPIRV-Tools-shared.dylib";

#else
#error No library names provided for the target platform.
#endif

}  // namespace rex::gpu
