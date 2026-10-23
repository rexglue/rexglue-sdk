#include <rex/paths.h>

#include <string>
#include <system_error>

#include <rex/platform/file.h>
#include <rex/string.h>

namespace rex {

Result<PathsConfig> ResolvePaths(const PathsConfig& paths, std::string_view app_name) {
  if (app_name.empty()) {
    return Err<PathsConfig>(ErrorCategory::kConfig, "the application name is empty");
  }
  PathsConfig resolved = paths;
  if (resolved.user_data_root.empty()) {
    const std::filesystem::path user = platform::GetUserFolder();
    if (user.empty()) {
      return Err<PathsConfig>(ErrorCategory::kConfig, "the host reports no user folder");
    }
    resolved.user_data_root = user / to_path(app_name);
  }
  if (resolved.cache_root.empty()) {
    resolved.cache_root = resolved.user_data_root / "cache";
  }
  if (resolved.config_file.empty()) {
    const std::filesystem::path root = platform::GetAppRoot(app_name);
    if (root.empty()) {
      return Err<PathsConfig>(ErrorCategory::kConfig, "the host reports no application folder");
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
      return Err<PathsConfig>(ErrorCategory::kIO,
                              "cannot create the application folder " + PathToUTF8(root));
    }
    resolved.config_file = root / to_path(std::string(app_name) + ".toml");
  }
  return resolved;
}

}  // namespace rex
