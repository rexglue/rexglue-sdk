/**
 * @file        ui/vulkan_moltenvk.cpp
 * @brief       MoltenVK Vulkan runtime detection and configuration for macOS
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_MAC

#include "vulkan_moltenvk.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/platform/env.h>
#include <rex/platform/file.h>

namespace rex::gpu::vulkan {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 2> kRootSuffixes = {".", "macOS"};
constexpr std::array<std::string_view, 3> kRootMarkers = {
    "lib/libvulkan.1.dylib", "Frameworks/vulkan.framework/vulkan", "lib/libMoltenVK.dylib"};
constexpr std::array<std::string_view, 4> kLoaderFiles = {
    "lib/libvulkan.1.dylib", "lib/libvulkan.dylib", "Frameworks/vulkan.framework/vulkan",
    "lib/libMoltenVK.dylib"};
constexpr std::array<std::string_view, 2> kSpirvToolsFiles = {
    "lib/libSPIRV-Tools-shared.dylib", "Frameworks/libSPIRV-Tools-shared.dylib"};
constexpr std::array<std::string_view, 3> kIcdFiles = {"share/vulkan/icd.d/MoltenVK_icd.json",
                                                       "Resources/vulkan/icd.d/MoltenVK_icd.json",
                                                       "vulkan/icd.d/MoltenVK_icd.json"};
constexpr std::array<std::string_view, 2> kSdkEnvironmentVariables = {"REX_VULKAN_SDK",
                                                                      "VULKAN_SDK"};
constexpr std::array<std::string_view, 2> kFallbackRoots = {"/usr/local", "/opt/homebrew"};

bool PathExists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

bool IsDirectory(const fs::path& path) {
  std::error_code ec;
  return fs::is_directory(path, ec);
}

void AppendUnique(std::vector<fs::path>& paths, const fs::path& path) {
  if (path.empty()) {
    return;
  }
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    paths.push_back(path);
  }
}

void AppendExisting(std::vector<fs::path>& paths, const fs::path& path) {
  if (PathExists(path)) {
    AppendUnique(paths, path);
  }
}

fs::path NormalizeSdkRoot(const fs::path& candidate) {
  if (candidate.empty()) {
    return {};
  }

  for (std::string_view suffix : kRootSuffixes) {
    fs::path root = suffix == "." ? candidate : candidate / suffix;
    for (std::string_view marker : kRootMarkers) {
      if (PathExists(root / marker)) {
        return root;
      }
    }
  }

  return {};
}

void AppendNormalizedRoot(std::vector<fs::path>& roots, const fs::path& candidate) {
  AppendUnique(roots, NormalizeSdkRoot(candidate));
}

void CollectExecutableRoots(std::vector<fs::path>& roots) {
  const fs::path executable_dir = rex::platform::GetExecutableFolder();
  if (executable_dir.empty()) {
    return;
  }

  AppendNormalizedRoot(roots, executable_dir);
  AppendNormalizedRoot(roots, executable_dir / "vulkan");

  const fs::path install_prefix = executable_dir.parent_path();
  if (!install_prefix.empty()) {
    AppendNormalizedRoot(roots, install_prefix);
    AppendNormalizedRoot(roots, install_prefix / "vulkan");
  }

  if (executable_dir.filename() == "MacOS") {
    const fs::path bundle_contents = executable_dir.parent_path();
    AppendNormalizedRoot(roots, bundle_contents);
    AppendNormalizedRoot(roots, bundle_contents / "Resources" / "vulkan");
  }
}

template <typename Candidates>
void AssignIfExists(fs::path& destination, const fs::path& root, const Candidates& candidates) {
  if (!destination.empty()) {
    return;
  }

  for (std::string_view relative_path : candidates) {
    fs::path candidate = root / relative_path;
    if (PathExists(candidate)) {
      destination = candidate;
      return;
    }
  }
}

std::vector<fs::path> CollectSdkRoots() {
  std::vector<fs::path> roots;

  CollectExecutableRoots(roots);

  for (std::string_view environment_variable : kSdkEnvironmentVariables) {
    auto sdk_root = rex::platform::GetEnvironmentValue(environment_variable);
    if (sdk_root && !sdk_root->empty()) {
      AppendNormalizedRoot(roots, *sdk_root);
    }
  }

  auto home = rex::platform::GetEnvironmentValue("HOME");
  if (home && !home->empty()) {
    fs::path user_sdk_dir = fs::path(*home) / "VulkanSDK";
    if (IsDirectory(user_sdk_dir)) {
      std::vector<fs::path> version_dirs;
      std::error_code ec;
      for (const fs::directory_entry& entry : fs::directory_iterator(user_sdk_dir, ec)) {
        if (entry.is_directory(ec)) {
          version_dirs.push_back(entry.path());
        }
      }
      std::sort(version_dirs.begin(), version_dirs.end(),
                [](const fs::path& a, const fs::path& b) { return a > b; });
      for (const fs::path& version_dir : version_dirs) {
        AppendNormalizedRoot(roots, version_dir);
      }
    }
  }

  for (std::string_view fallback_root : kFallbackRoots) {
    AppendNormalizedRoot(roots, fallback_root);
  }

  return roots;
}

void SetIfUnset(std::string_view name, const fs::path& value) {
  auto current = rex::platform::GetEnvironmentValue(name);
  if (!current || current->empty()) {
    rex::platform::SetEnvironmentValue(name, value.string());
  }
}

}  // namespace

MacOSVulkanRuntimePaths DetectMacOSVulkanRuntimePaths() {
  MacOSVulkanRuntimePaths paths;

  for (const std::filesystem::path& root : CollectSdkRoots()) {
    if (paths.sdk_root.empty()) {
      paths.sdk_root = root;
    }

    for (std::string_view loader_file : kLoaderFiles) {
      AppendExisting(paths.loader_candidates, root / loader_file);
    }

    AssignIfExists(paths.spirv_tools_library, root, kSpirvToolsFiles);
    AssignIfExists(paths.moltenvk_icd, root, kIcdFiles);
  }

  return paths;
}

void ConfigureMacOSVulkanEnvironment(const MacOSVulkanRuntimePaths& paths) {
  if (!paths.sdk_root.empty()) {
    SetIfUnset("VULKAN_SDK", paths.sdk_root);
  }
  if (!paths.moltenvk_icd.empty()) {
    SetIfUnset("VK_DRIVER_FILES", paths.moltenvk_icd);
    SetIfUnset("VK_ICD_FILENAMES", paths.moltenvk_icd);
  }
}

}  // namespace rex::gpu::vulkan

#endif  // REX_PLATFORM_MAC
