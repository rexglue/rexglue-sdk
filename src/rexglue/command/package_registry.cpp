/**
 * @file        rexglue/command/package_registry.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "package_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rexglue::cli {

namespace {

namespace fs = std::filesystem;

/** The path as the registry compares it, with forward slashes and no trailing slash. */
std::string Normalize(const fs::path& path) {
  std::error_code ec;
  std::string text = fs::weakly_canonical(path, ec).generic_string();
  if (ec) {
    text = path.generic_string();
  }
  while (text.size() > 1 && text.back() == '/') {
    text.pop_back();
  }
#if REX_PLATFORM_WIN32
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
#endif
  return text;
}

}  // namespace

bool IsPackageRegistered(const fs::path& prefix) {
  const std::string wanted = Normalize(prefix);
#if REX_PLATFORM_WIN32
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Kitware\\CMake\\Packages\\rexglue", 0, KEY_READ,
                    &key) != ERROR_SUCCESS) {
    return false;
  }
  bool found = false;
  for (DWORD index = 0; !found; ++index) {
    wchar_t name[256];
    DWORD name_size = static_cast<DWORD>(std::size(name));
    wchar_t data[4096];
    DWORD data_size = sizeof(data) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG status = RegEnumValueW(key, index, name, &name_size, nullptr, &type,
                                      reinterpret_cast<BYTE*>(data), &data_size);
    if (status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (status != ERROR_SUCCESS || type != REG_SZ) {
      continue;
    }
    data[data_size / sizeof(wchar_t)] = L'\0';
    found = Normalize(fs::path(data)) == wanted;
  }
  RegCloseKey(key);
  return found;
#else
  const char* home = std::getenv("HOME");
  if (!home) {
    return false;
  }
  std::error_code ec;
  for (const auto& entry :
       fs::directory_iterator(fs::path(home) / ".cmake" / "packages" / "rexglue", ec)) {
    std::ifstream in(entry.path());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.pop_back();
    }
    if (!text.empty() && Normalize(fs::path(text)) == wanted) {
      return true;
    }
  }
  return false;
#endif
}

}  // namespace rexglue::cli
