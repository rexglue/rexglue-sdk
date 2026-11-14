#include <rex/platform/file.h>

#include <array>
#include <atomic>
#include <format>
#include <string>
#include <system_error>

#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <rex/platform/process.h>
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

std::filesystem::path GetLibraryFolder() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&GetLibraryFolder), &info) != 0 &&
      info.dli_fname != nullptr) {
    std::error_code ec;
    const std::filesystem::path file = std::filesystem::absolute(info.dli_fname, ec);
    if (!ec && !file.parent_path().empty()) {
      return file.parent_path().lexically_normal();
    }
  }
  return GetExecutableFolder();
}

namespace {

std::filesystem::path GetHomeFolder() {
  if (auto home = GetEnvironmentValue("HOME"); home && !home->empty()) {
    return {*home};
  }
  passwd entry{};
  passwd* found = nullptr;
  std::array<char, 4096> buffer{};
  if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) {
    return {};
  }
  return {found->pw_dir};
}

#if defined(__APPLE__)
bool IsMacAppBundle() {
  const std::filesystem::path executable_dir = GetExecutableFolder();
  if (executable_dir.filename() != "MacOS") {
    return false;
  }
  const std::filesystem::path contents_dir = executable_dir.parent_path();
  return contents_dir.filename() == "Contents" && contents_dir.parent_path().extension() == ".app";
}
#endif

}  // namespace

std::filesystem::path GetUserFolder() {
  if (auto xdg = GetEnvironmentValue("XDG_DATA_HOME"); xdg && !xdg->empty()) {
    return {*xdg};
  }
  const std::filesystem::path home = GetHomeFolder();
  return home.empty() ? home : home / ".local" / "share";
}

bool IsPackagedApp() {
  if (GetEnvironmentValue("APPIMAGE")) {
    return true;
  }
#if defined(__APPLE__)
  return IsMacAppBundle();
#else
  return false;
#endif
}

std::filesystem::path GetUserConfigFolder(std::string_view app_name) {
  const std::string name = SanitizeFolderName(app_name, {});
  if (name.empty()) {
    return {};
  }
#if defined(__APPLE__)
  const std::filesystem::path home = GetHomeFolder();
  return home.empty() ? home : home / "Library" / "Application Support" / name;
#else
  if (auto xdg = GetEnvironmentValue("XDG_CONFIG_HOME"); xdg && !xdg->empty()) {
    return std::filesystem::path(*xdg) / name;
  }
  const std::filesystem::path home = GetHomeFolder();
  return home.empty() ? home : home / ".config" / name;
#endif
}

bool IsDirectoryWritable(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return false;
  }
  static std::atomic<u32> counter{0};
  const std::filesystem::path temp_file =
      dir / std::format(".rex_write_check_{}_{}", getpid(), counter.fetch_add(1));
  const int fd = ::open(temp_file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return false;
  }
  ::close(fd);
  ::unlink(temp_file.c_str());
  return true;
}

}  // namespace rex::platform
