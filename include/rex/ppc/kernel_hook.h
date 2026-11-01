/**
 * @file        ppc/kernel_hook.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/ppc/context.h>

namespace rex::ppc {

/** The kernel calls the PPC runtime makes. A null member acts as if no kernel runs. */
struct KernelHooks {
  /** Returns the register file and memory base of the calling thread. */
  BoundThread (*get_bound_thread)();
  /** Returns the function recompiled for a guest address, null when there is none. */
  PPCFunc* (*resolve_function)(u32 guest_address);
  /** Checks in Debug builds that a hook runs on the register file of its thread. */
  void (*check_hook_context)(const PPCContext& ctx, const char* name);
  /** Checks the registers a detour wrote against the codegen flags of the bound runtime. */
  void (*check_bound_detour_local_writes)(const char* name, const PPCContext& at_entry,
                                          const PPCContext& at_exit, std::atomic_flag& warned);
  /** Checks the argument registers a detour declares against the usage of the bound runtime. */
  void (*check_bound_detour_arguments)(const char* name, PPCFunc* host, u8 declared_gpr,
                                       u16 declared_fpr);
};

/**
 * Sets the kernel calls of the PPC runtime. With none set, no thread is bound, no address resolves
 * and the detour checks do nothing.
 *
 * @param hooks The calls, which must stay valid until they are replaced. Null clears them.
 */
REX_PPC_API void SetKernelHooks(const KernelHooks* hooks);

}  // namespace rex::ppc
