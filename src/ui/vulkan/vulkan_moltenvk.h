#pragma once

#include <filesystem>
#include <vector>

#include <rex/platform.h>

namespace rex::ui::vulkan {

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

}  // namespace rex::ui::vulkan
