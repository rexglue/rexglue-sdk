/**
 * @file        core/platform/env_win.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#include <rex/platform/env.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "../platform_win.h"

#include <rex/string.h>

#include <cwchar>

namespace rex::platform {

std::optional<std::string> GetEnvironmentValue(std::string_view name) {
  std::u16string wname = rex::to_utf16(name);
  // First call sizes the buffer (return value is char count INCLUDING NUL on
  // success, or 0 on error / not-set).
  DWORD required = GetEnvironmentVariableW(reinterpret_cast<LPCWSTR>(wname.c_str()), nullptr, 0);
  if (required == 0) {
    return std::nullopt;
  }
  std::u16string buffer(required, u'\0');
  DWORD written = GetEnvironmentVariableW(reinterpret_cast<LPCWSTR>(wname.c_str()),
                                          reinterpret_cast<LPWSTR>(buffer.data()), required);
  if (written == 0 || written >= required) {
    return std::nullopt;
  }
  buffer.resize(written);
  return rex::to_utf8(buffer);
}

bool SetEnvironmentValue(std::string_view name, std::string_view value) {
  std::u16string wname = rex::to_utf16(name);
  std::u16string wvalue = rex::to_utf16(value);
  return SetEnvironmentVariableW(reinterpret_cast<LPCWSTR>(wname.c_str()),
                                 reinterpret_cast<LPCWSTR>(wvalue.c_str())) != 0;
}

bool UnsetEnvironmentValue(std::string_view name) {
  std::u16string wname = rex::to_utf16(name);
  return SetEnvironmentVariableW(reinterpret_cast<LPCWSTR>(wname.c_str()), nullptr) != 0;
}

std::vector<std::pair<std::string, std::string>> EnumerateEnvironment() {
  std::vector<std::pair<std::string, std::string>> out;
  wchar_t* block = GetEnvironmentStringsW();
  if (!block) {
    return out;
  }
  for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
    std::u16string_view entry(reinterpret_cast<const char16_t*>(p), wcslen(p));
    auto eq = entry.find(u'=', entry.starts_with(u'=') ? 1 : 0);
    if (eq == std::u16string_view::npos) {
      continue;
    }
    out.emplace_back(rex::to_utf8(entry.substr(0, eq)), rex::to_utf8(entry.substr(eq + 1)));
  }
  FreeEnvironmentStringsW(block);
  return out;
}

}  // namespace rex::platform
