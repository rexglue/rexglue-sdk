#include <rex/platform.h>

#if REX_PLATFORM_MAC

#include "vulkan_moltenvk.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <system_error>
#include <string>

#include <rex/filesystem.h>

namespace rex::ui::vulkan {
namespace {

namespace fs = std::filesystem;

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

  const std::array<fs::path, 2> roots = {candidate, candidate / "macOS"};
  for (const fs::path& root : roots) {
    if (PathExists(root / "lib" / "libvulkan.1.dylib") ||
        PathExists(root / "Frameworks" / "vulkan.framework" / "vulkan") ||
        PathExists(root / "lib" / "libMoltenVK.dylib")) {
      return root;
    }
  }

  return {};
}

void AppendNormalizedRoot(std::vector<fs::path>& roots, const fs::path& candidate) {
  AppendUnique(roots, NormalizeSdkRoot(candidate));
}

void CollectExecutableRoots(std::vector<fs::path>& roots) {
  const fs::path executable_dir = rex::filesystem::GetExecutableFolder();
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

void AssignIfExists(fs::path& destination, std::initializer_list<fs::path> candidates) {
  if (!destination.empty()) {
    return;
  }

  for (const fs::path& candidate : candidates) {
    if (PathExists(candidate)) {
      destination = candidate;
      return;
    }
  }
}

std::vector<fs::path> CollectSdkRoots() {
  std::vector<fs::path> roots;

  CollectExecutableRoots(roots);

  const char* rex_vulkan_sdk = std::getenv("REX_VULKAN_SDK");
  if (rex_vulkan_sdk && rex_vulkan_sdk[0]) {
    AppendNormalizedRoot(roots, rex_vulkan_sdk);
  }

  const char* vulkan_sdk = std::getenv("VULKAN_SDK");
  if (vulkan_sdk && vulkan_sdk[0]) {
    AppendNormalizedRoot(roots, vulkan_sdk);
  }

  const char* home = std::getenv("HOME");
  if (home && home[0]) {
    fs::path user_sdk_dir = fs::path(home) / "VulkanSDK";
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

  AppendNormalizedRoot(roots, "/usr/local");
  AppendNormalizedRoot(roots, "/opt/homebrew");

  return roots;
}

}  // namespace

MacOSVulkanRuntimePaths DetectMacOSVulkanRuntimePaths() {
  MacOSVulkanRuntimePaths paths;

  for (const std::filesystem::path& root : CollectSdkRoots()) {
    if (paths.sdk_root.empty()) {
      paths.sdk_root = root;
    }

    AppendExisting(paths.loader_candidates, root / "lib" / "libvulkan.1.dylib");
    AppendExisting(paths.loader_candidates, root / "lib" / "libvulkan.dylib");
    AppendExisting(paths.loader_candidates, root / "Frameworks" / "vulkan.framework" / "vulkan");
    AppendExisting(paths.loader_candidates, root / "lib" / "libMoltenVK.dylib");

    AssignIfExists(paths.spirv_tools_library,
                   {root / "lib" / "libSPIRV-Tools-shared.dylib",
                    root / "Frameworks" / "libSPIRV-Tools-shared.dylib"});

    AssignIfExists(paths.moltenvk_icd,
                   {root / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json",
                    root / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json",
                    root / "vulkan" / "icd.d" / "MoltenVK_icd.json"});
  }

  return paths;
}

void ConfigureMacOSVulkanEnvironment(const MacOSVulkanRuntimePaths& paths) {
  if (!paths.sdk_root.empty()) {
    const char* vulkan_sdk = std::getenv("VULKAN_SDK");
    if (!vulkan_sdk || !vulkan_sdk[0]) {
      std::string sdk_root = paths.sdk_root.string();
      setenv("VULKAN_SDK", sdk_root.c_str(), 1);
    }
  }

  if (!paths.moltenvk_icd.empty()) {
    std::string moltenvk_icd = paths.moltenvk_icd.string();
    const char* vk_driver_files = std::getenv("VK_DRIVER_FILES");
    if (!vk_driver_files || !vk_driver_files[0]) {
      setenv("VK_DRIVER_FILES", moltenvk_icd.c_str(), 1);
    }

    const char* vk_icd_filenames = std::getenv("VK_ICD_FILENAMES");
    if (!vk_icd_filenames || !vk_icd_filenames[0]) {
      setenv("VK_ICD_FILENAMES", moltenvk_icd.c_str(), 1);
    }
  }
}

}  // namespace rex::ui::vulkan

#endif  // REX_PLATFORM_MAC
