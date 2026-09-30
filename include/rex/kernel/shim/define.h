/**
 * @file        kernel/shim/define.h
 * @brief       Declaration macros for shim functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/hook.h>
#include <rex/kernel/shim/registry.h>

/**
 * Defines a shim function and its hook symbol.
 *
 * Expands to a registry check, a forward declaration of `<NAME>_shim`, the hook `rexshim_<NAME>`
 * and the definition head of `<NAME>_shim`. Used at namespace scope. The name must be registered.
 *
 * @param NAME The registered name.
 * @param RET The return type.
 * @param ... The parameter list.
 *
 * @code
 * REX_SHIM_FN(memcpy, void*, void* dst, const void* src, size_t count) {
 *   return std::memcpy(dst, src, count);
 * }
 * @endcode
 */
#define REX_SHIM_FN(NAME, RET, ...)                                          \
  static_assert(::rex::kernel::shim::IsRegistered(#NAME),                    \
                "REX_SHIM_FN(" #NAME "): name is not in the shim registry"); \
  static RET NAME##_shim(__VA_ARGS__);                                       \
  REX_HOOK(rexshim_##NAME, NAME##_shim)                                      \
  static RET NAME##_shim(__VA_ARGS__)

/**
 * Defines a shim function that reads the PPC context directly.
 *
 * @param NAME The registered name.
 *
 * @code
 * REX_SHIM_FN_RAW(SwitchToFiber) {
 *   ctx.r3.u64 = 0;
 * }
 * @endcode
 */
#define REX_SHIM_FN_RAW(NAME)                                                    \
  static_assert(::rex::kernel::shim::IsRegistered(#NAME),                        \
                "REX_SHIM_FN_RAW(" #NAME "): name is not in the shim registry"); \
  REX_HOOK_RAW(rexshim_##NAME)
