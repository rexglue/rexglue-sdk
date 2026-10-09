/**
 * @file        core/signal_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <ucontext.h>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::platform::detail {

/**
 * Returns the instruction pointer a signal interrupted.
 *
 * @param signal_context The ucontext_t the signal handler receives.
 * @return The interrupted instruction pointer.
 */
inline u64 SignalPc(void* signal_context) {
  auto* ucontext = static_cast<ucontext_t*>(signal_context);
#if REX_ARCH_AMD64 && REX_PLATFORM_MAC
  return ucontext->uc_mcontext->__ss.__rip;
#elif REX_ARCH_AMD64
  return static_cast<u64>(ucontext->uc_mcontext.gregs[REG_RIP]);
#elif REX_PLATFORM_MAC
  return __darwin_arm_thread_state64_get_pc(ucontext->uc_mcontext->__ss);
#else
  return ucontext->uc_mcontext.pc;
#endif
}

}  // namespace rex::platform::detail
