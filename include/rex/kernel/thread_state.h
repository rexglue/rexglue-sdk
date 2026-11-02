/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <string>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/kernel/heap.h>
#include <rex/ppc/context.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

/** FP and vector state the Ke save exports hold for the routine running on this thread. */
struct DPCSaveArea {
  std::array<PPCRegister, 32> fpr{};
  /** The host FP control word, which holds the guest rounding and flush bits. */
  u32 csr = 0;
  std::array<PPCVRegister, 128> vr{};
  u8 vscr_sat = 0;
  /** The VSCR non Java mode bit. */
  u8 vscr_nj = 0;
};

class REX_KERNEL_API ThreadState {
 public:
  /**
   * Constructs the state of a guest thread.
   *
   * @param thread_id The guest thread ID, UINT32_MAX for a host thread ID.
   * @param stack_base The guest stack pointer.
   * @param pcr_address The guest PCR address, loaded into r13.
   * @param runtime The runtime the thread belongs to, null in tests that run without one.
   */
  ThreadState(u32 thread_id, u32 stack_base, u32 pcr_address, Memory* memory,
              TitleRuntime* runtime);
  /** Unbinds the state from the calling host thread when it is the bound one. */
  ~ThreadState();

  Memory* GetMemory() const { return memory_; }

  /** Asks the thread to stop at its next kernel boundary. */
  void RequestStop() { stop_requested_.store(true, std::memory_order_release); }

  bool IsStopRequested() const { return stop_requested_.load(std::memory_order_acquire); }

  /** Clears the stop request once the thread runs its exit sequence. */
  void ClearStop() { stop_requested_.store(false, std::memory_order_release); }
  ::PPCContext* GetContext() const { return context_; }

  u32 GetThreadID() const { return thread_id_; }

  /** Returns the runtime the thread belongs to, null for a state built without one. */
  TitleRuntime* GetRuntime() const { return runtime_; }

  DPCSaveArea& GetDPCSaveArea() { return dpc_save_area_; }

  /**
   * Binds a thread state to the calling host thread.
   *
   * @param state The state, null to unbind.
   */
  static void Bind(ThreadState* state);
  /** Returns the thread state bound to the calling host thread, null when none is bound. */
  static ThreadState* Get();
  /**
   * @return The ID, 0xFFFFFFFF when none is bound.
   */
  static u32 GetBoundThreadID();

 private:
  Memory* memory_;
  TitleRuntime* runtime_;

  u32 thread_id_ = 0;

  alignas(64)::PPCContext context_storage_{};
  ::PPCContext* context_ = &context_storage_;
  DPCSaveArea dpc_save_area_;
  std::atomic<bool> stop_requested_{false};
};

namespace detail {

/** Logs that GetCurrent() ran on a thread with no runtime and aborts, in every build. */
[[noreturn]] REX_KERNEL_API void AbortNoRuntime();

}  // namespace detail

/** Returns the runtime the calling thread belongs to. Aborts on a host thread. */
inline TitleRuntime& GetCurrent() {
  ThreadState* state = ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) [[unlikely]] {
    detail::AbortNoRuntime();
  }
  return *state->GetRuntime();
}

}  // namespace rex::kernel
