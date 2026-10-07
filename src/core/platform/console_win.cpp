/**
 * @file        core/platform/console_win.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#include <rex/platform/console.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "../platform_win.h"

#include <io.h>

namespace rex::platform {

bool IsTerminal(FILE* stream) {
  if (!stream) {
    return false;
  }
  return _isatty(_fileno(stream)) != 0;
}

bool EnableANSIEscapes(FILE* stream) {
  if (!stream) {
    return false;
  }
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD mode = 0;
  if (!GetConsoleMode(handle, &mode)) {
    return false;
  }
  if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
    return true;
  }
  return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

void SetUTF8OutputCodePage() {
  SetConsoleOutputCP(CP_UTF8);
}

}  // namespace rex::platform
