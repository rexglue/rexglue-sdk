/**
 * @file        ui/vulkan_moltenvk.h
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <vector>

#include <rex/platform.h>

namespace rex::gpu::vulkan {

#if REX_PLATFORM_MAC

struct MacOSVulkanRuntimePaths {
  std::filesystem::path sdk_root;
  std::filesystem::path spirv_tools_library;
  std::filesystem::path moltenvk_icd;
  std::vector<std::filesystem::path> loader_candidates;
};

MacOSVulkanRuntimePaths DetectMacOSVulkanRuntimePaths();
void ConfigureMacOSVulkanEnvironment(const MacOSVulkanRuntimePaths& paths);

#endif  // REX_PLATFORM_MAC

}  // namespace rex::gpu::vulkan
