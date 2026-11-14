#include <rex/platform/file.h>

#include <atomic>
#include <format>
#include <string>
#include <system_error>

#include "../platform_win.h"

#include <shlobj.h>

#include <rex/string/text.h>

namespace rex::platform {

namespace {

std::filesystem::path GetModulePath(HMODULE module) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size()) {
      buffer.resize(length);
      return {buffer};
    }
    buffer.resize(buffer.size() * 2);
  }
}

}  // namespace

std::filesystem::path GetExecutablePath() {
  // _get_wpgmptr aborts under the debug CRT when the process started through a narrow main.
  return GetModulePath(nullptr);
}

std::filesystem::path GetLibraryFolder() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&GetLibraryFolder), &module)) {
    const std::filesystem::path file = GetModulePath(module);
    if (!file.empty()) {
      return file.parent_path();
    }
  }
  return GetExecutableFolder();
}

std::filesystem::path GetUserFolder() {
  std::filesystem::path result;
  PWSTR path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &path))) {
    result.assign(path);
  }
  CoTaskMemFree(path);
  return result;
}

bool IsPackagedApp() {
  return false;
}

std::filesystem::path GetUserConfigFolder(std::string_view app_name) {
  const std::string name = SanitizeFolderName(app_name, {});
  if (name.empty()) {
    return {};
  }
  std::filesystem::path result;
  PWSTR path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &path))) {
    result.assign(path);
  }
  CoTaskMemFree(path);
  return result.empty() ? result : result / string::to_path(name);
}

bool IsDirectoryWritable(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return false;
  }
  static std::atomic<u32> counter{0};
  const std::filesystem::path temp_file =
      dir / std::format(L".rex_write_check_{}_{}", GetCurrentProcessId(), counter.fetch_add(1));
  const HANDLE file = CreateFileW(temp_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  CloseHandle(file);
  return true;
}

}  // namespace rex::platform
