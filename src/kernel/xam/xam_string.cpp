/**
 * @file        kernel/xam/xam_string.cpp
 * @brief       Win32 string routines served to the XAM import table and to the shim layer
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
  std::memcpy(dst, src, std::strlen(src) + 1);
  return dst;
}

REX_SHIM_FN(lstrcpynA, char*, char* dst, const char* src, int maxlen) {
  if (maxlen <= 0)
    return dst;
  const size_t limit = static_cast<size_t>(maxlen) - 1;
  const size_t length = strnlen(src, limit);
  std::memcpy(dst, src, length);
  std::memset(dst + length, 0, limit - length);
  dst[maxlen - 1] = '\0';
  return dst;
}

REX_SHIM_FN(lstrcatA, char*, char* dst, const char* src) {
  std::memcpy(dst + std::strlen(dst), src, std::strlen(src) + 1);
  return dst;
}

}  // namespace rex::kernel::xam
