/**
 * @file        kernel/ppc_hooks.h
 * @brief       Registration of the kernel calls the PPC runtime makes
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

namespace rex::kernel {

/** Sets the kernel calls of the PPC runtime. Calls nest, and the last unregister clears them. */
void RegisterPPCHooks();

/** Releases one registration of the kernel calls of the PPC runtime. */
void UnregisterPPCHooks();

/** Holds the kernel calls of the PPC runtime registered for its lifetime. */
class ScopedPPCHookRegistration {
 public:
  ScopedPPCHookRegistration() { RegisterPPCHooks(); }
  ~ScopedPPCHookRegistration() { UnregisterPPCHooks(); }
  ScopedPPCHookRegistration(const ScopedPPCHookRegistration&) = delete;
  ScopedPPCHookRegistration& operator=(const ScopedPPCHookRegistration&) = delete;
};

}  // namespace rex::kernel
