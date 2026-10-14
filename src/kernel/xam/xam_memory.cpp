/**
 * @file        kernel/xam/xam_memory.cpp
 * @brief       Memory routines served to the XAM import table and to the shim layer
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

namespace rex::kernel::xam {

REX_SHIM_FN(XMemCpy, void*, void* dst, const void* src, u32 n) {
  return std::memcpy(dst, src, n);
}

REX_SHIM_FN(XMemSet, void*, void* dst, int val, u32 n) {
  return std::memset(dst, val, n);
}

}  // namespace rex::kernel::xam
