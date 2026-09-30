/**
 * @file        core/platform/file_win.cpp
 * @brief       Windows host folder queries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/filesystem_win.cc (Ben Vanik, 2022).
 */

#include <rex/platform/file.h>

#include <string>

#include "../platform_win.h"

#include <shlobj.h>

namespace rex::platform {

std::filesystem::path GetExecutablePath() {
  // _get_wpgmptr aborts under the debug CRT when the process started through a narrow main.
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
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

std::filesystem::path GetUserFolder() {
  std::filesystem::path result;
  PWSTR path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &path))) {
    result.assign(path);
  }
  CoTaskMemFree(path);
  return result;
}

}  // namespace rex::platform
