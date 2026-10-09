#include <rex/platform/file.h>

#include <array>
#include <string>
#include <system_error>

#include <pwd.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <rex/platform/env.h>
#include <rex/types.h>

namespace rex::platform {

std::filesystem::path GetExecutablePath() {
#if defined(__APPLE__)
  u32 size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size == 0) {
    return {};
  }
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    return {};
  }
  if (!path.empty() && path.back() == '\0') {
    path.pop_back();
  }
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  return ec ? std::filesystem::path(path) : canonical;
#else
  std::error_code ec;
  std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : path;
#endif
}

std::filesystem::path GetUserFolder() {
  if (auto xdg = GetEnvironmentValue("XDG_DATA_HOME")) {
    return {*xdg};
  }
  if (auto home = GetEnvironmentValue("HOME")) {
    return std::filesystem::path(*home) / ".local" / "share";
  }
  passwd entry{};
  passwd* found = nullptr;
  std::array<char, 4096> buffer{};
  if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) {
    return {};
  }
  return std::filesystem::path(found->pw_dir) / ".local" / "share";
}

}  // namespace rex::platform
