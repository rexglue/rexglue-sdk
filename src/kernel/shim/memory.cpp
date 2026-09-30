/**
 * @file        kernel/shim/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <cstring>

#include <rex/hook.h>
#include <rex/kernel/shim/define.h>

namespace rex::kernel::shim {

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

REX_SHIM_FN(memcpy, void*, void* dst, const void* src, size_t n) {
  return std::memcpy(dst, src, n);
}

REX_SHIM_FN(memmove, void*, void* dst, const void* src, size_t n) {
  return std::memmove(dst, src, n);
}

REX_SHIM_FN(memset, void*, void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

REX_SHIM_FN(memchr, void*, const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

REX_SHIM_FN(XMemSet128, void*, void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

REX_SHIM_FN(memset_vmx, void*, void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

REX_SHIM_FN(memcpy_s, int, void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  std::memcpy(dst, src, count);
  return 0;
}

REX_SHIM_FN(memmove_s, int, void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::shim
