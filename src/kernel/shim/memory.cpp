/**
 * @file        kernel/shim/memory.cpp
 * @brief       Native memory operation hooks that replace recompiled PPC
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstring>

#include <rex/ppc/hook.h>
#include "kernel/shim/function.h"

namespace rex::kernel::shim {

REX_SHIM_FN(memcpy, void*, void* dst, const void* src, u32 n) {
  return std::memcpy(dst, src, n);
}

REX_SHIM_FN(memmove, void*, void* dst, const void* src, u32 n) {
  return std::memmove(dst, src, n);
}

REX_SHIM_FN(memset, void*, void* dst, int val, u32 n) {
  return std::memset(dst, val, n);
}

REX_SHIM_FN(memchr, void*, const void* ptr, int val, u32 n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

REX_SHIM_FN(XMemSet128, void*, void* dst, int val, u32 n) {
  return std::memset(dst, val, n);
}

REX_SHIM_FN(memset_vmx, void*, void* dst, int val, u32 n) {
  return std::memset(dst, val, n);
}

REX_SHIM_FN(memcpy_s, int, void* dst, u32 dstsz, const void* src, u32 count) {
  if (count == 0)
    return 0;
  if (!dst)
    return 22;
  if (!src || dstsz < count) {
    std::memset(dst, 0, dstsz);
    return src ? 34 : 22;
  }
  std::memcpy(dst, src, count);
  return 0;
}

REX_SHIM_FN(memmove_s, int, void* dst, u32 dstsz, const void* src, u32 count) {
  if (count == 0)
    return 0;
  if (!dst || !src)
    return 22;
  if (dstsz < count)
    return 34;
  std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::shim
