/**
 * @file        core/process_resources_win.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/process.h>

#include <string>

#include <windows.h>

namespace rex::platform {

Result<OpenFileLimit> RaiseOpenFileLimit() {
  return OpenFileLimit{};
}

std::string DescribeResourceUse() {
  DWORD handles = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &handles)) {
    return "handles unknown";
  }
  return "handles " + std::to_string(handles);
}

}  // namespace rex::platform
