#include "base/library_name.h"

#include <format>
#include <string>

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

}  // namespace rex::detail

namespace rex::platform {

bool DynamicLibrary::LoadNamed(std::string_view name, SymbolResolution mode) {
  return Load(GetExecutableFolder() / rex::detail::LibraryFileName(name), mode);
}

}  // namespace rex::platform
