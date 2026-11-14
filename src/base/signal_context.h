/**
 * @file        base/signal_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_MAC
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#include <rex/types.h>

namespace rex::platform::detail {

#if REX_PLATFORM_MAC

/** Calls the frame target with the interrupted state as its caller. */
extern "C" void rex_redirect_trampoline();

inline constexpr size_t kRedirectFrameFP = 0;
/** Link register slot, unused on x86-64. */
inline constexpr size_t kRedirectFrameLR = 8;
inline constexpr size_t kRedirectFrameSP = 16;
inline constexpr size_t kRedirectFramePC = 24;
inline constexpr size_t kRedirectFrameTarget = 32;
/** Frame size, a multiple of the 16 byte stack alignment. */
inline constexpr size_t kRedirectFrameSize = 48;

/**
 * Makes the thread call target when the signal handler returns. An exception thrown by target
 * unwinds into the interrupted frames.
 *
 * @param signal_context The ucontext_t the signal handler receives.
 * @param target The function the thread calls.
 */
inline void CallOnSignalReturn(void* signal_context, void (*target)()) {
  auto& state = static_cast<ucontext_t*>(signal_context)->uc_mcontext->__ss;
  const auto write = [](u64 frame, size_t slot, u64 value) {
    *reinterpret_cast<u64*>(frame + slot) = value;
  };
#if REX_ARCH_AMD64
  const u64 sp = state.__rsp;
  const u64 frame = (sp - kRedirectFrameSize) & ~u64{15};
  write(frame, kRedirectFrameFP, state.__rbp);
  write(frame, kRedirectFrameLR, 0);
  write(frame, kRedirectFrameSP, sp);
  write(frame, kRedirectFramePC, state.__rip);
  write(frame, kRedirectFrameTarget, reinterpret_cast<u64>(target));
  state.__rsp = frame;
  state.__rip = reinterpret_cast<u64>(&rex_redirect_trampoline);
#else
  const u64 sp = __darwin_arm_thread_state64_get_sp(state);
  const u64 frame = (sp - kRedirectFrameSize) & ~u64{15};
  write(frame, kRedirectFrameFP, __darwin_arm_thread_state64_get_fp(state));
  write(frame, kRedirectFrameLR, __darwin_arm_thread_state64_get_lr(state));
  write(frame, kRedirectFrameSP, sp);
  write(frame, kRedirectFramePC, __darwin_arm_thread_state64_get_pc(state));
  write(frame, kRedirectFrameTarget, reinterpret_cast<u64>(target));
  __darwin_arm_thread_state64_set_sp(state, frame);
  __darwin_arm_thread_state64_set_pc_fptr(state, reinterpret_cast<void*>(&rex_redirect_trampoline));
#endif
}

#endif

}  // namespace rex::platform::detail
