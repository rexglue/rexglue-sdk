/**
 * @file        kernel/xam/xam_string.cpp
 * @brief       Win32 string routines served to the Xam import table and to the shim layer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstring>

#include <rex/hook.h>
#include <rex/kernel/shim/define.h>
#include <rex/platform.h>

namespace rex::kernel::xam {

REX_SHIM_FN(lstrlenA, int, const char* s) {
  return s ? static_cast<int>(std::strlen(s)) : 0;
}

REX_SHIM_FN(lstrcpyA, char*, char* dst, const char* src) {
#if REX_PLATFORM_WIN32
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  return std::strcpy(dst, src);
}

REX_SHIM_FN(lstrcpynA, char*, char* dst, const char* src, int maxlen) {
  if (maxlen <= 0)
    return dst;
#if REX_PLATFORM_WIN32
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  std::strncpy(dst, src, static_cast<size_t>(maxlen) - 1);
  dst[maxlen - 1] = '\0';
  return dst;
}

REX_SHIM_FN(lstrcatA, char*, char* dst, const char* src) {
#if REX_PLATFORM_WIN32
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  return std::strcat(dst, src);
}

}  // namespace rex::kernel::xam
