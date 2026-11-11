#include "base/library_name.h"

#include <format>
#include <string>
#include <system_error>

#include <rex/platform.h>
#include <rex/platform/dynlib.h>
#include <rex/platform/file.h>

namespace rex::detail {

namespace {

std::string_view BuildPostfix() {
  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  if (kConfig == "Debug") {
    return "d";
  }
  if (kConfig == "RelWithDebInfo") {
    return "rd";
  }
  return "";
}

}  // namespace

std::filesystem::path LibraryFileName(std::string_view name) {
#if REX_PLATFORM_WIN32
  return std::format("{}.dll", name);
#elif REX_PLATFORM_MAC
  return std::format("lib{}.dylib", name);
#else
  return std::format("lib{}.so", name);
#endif
}

std::filesystem::path ModuleFileName(std::string_view name) {
  return LibraryFileName(std::format("{}{}", name, BuildPostfix()));
}

std::filesystem::path FindLibraryFile(const std::filesystem::path& file_name,
                                      std::vector<std::filesystem::path>& folders) {
  folders.clear();
  folders.push_back(platform::GetLibraryFolder());
  if (const std::filesystem::path executable = platform::GetExecutableFolder();
      executable != folders.front()) {
    folders.push_back(executable);
  }
  for (const std::filesystem::path& folder : folders) {
    std::error_code ec;
    if (std::filesystem::exists(folder / file_name, ec)) {
      return folder / file_name;
    }
  }
  return {};
}

}  // namespace rex::detail

namespace rex::platform {

bool DynamicLibrary::LoadNamed(std::string_view name, SymbolResolution mode) {
  const std::filesystem::path file_name = rex::detail::LibraryFileName(name);
  std::vector<std::filesystem::path> folders;
  const std::filesystem::path found = rex::detail::FindLibraryFile(file_name, folders);
  return Load(found.empty() ? folders.front() / file_name : found, mode);
}

}  // namespace rex::platform
