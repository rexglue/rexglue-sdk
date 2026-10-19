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
#include <rex/export.h>
#include <rex/runtime/xmemory.h>
#include <rex/ppc/context.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** FP and vector state the Ke save exports hold for the routine running on this thread. */
struct DPCSaveArea {
  /** f0 to f31. */
  std::array<PPCRegister, 32> fpr{};
  /** The host FP control word, which holds the guest rounding and flush bits. */
  u32 csr = 0;
  /** v0 to v127. */
  std::array<PPCVRegister, 128> vr{};
  /** The VSCR saturation bit. */
  u8 vscr_sat = 0;
  /** The VSCR non Java mode bit. */
  u8 vscr_nj = 0;
};

/** The register file and stop flag of a guest thread. */
class REX_RUNTIME_API ThreadState {
 public:
  /**
   * Constructs the state of a guest thread.
   *
   * @param thread_id The guest thread id, UINT32_MAX for a host thread id.
   * @param stack_base The guest stack pointer.
   * @param pcr_address The guest PCR address, loaded into r13.
   * @param memory The guest memory.
   * @param runtime The runtime the thread belongs to, null in tests that run without one.
   */
  ThreadState(u32 thread_id, u32 stack_base, u32 pcr_address, Memory* memory, Runtime* runtime);
  /** Unbinds the state from the calling host thread when it is the bound one. */
  ~ThreadState();

  Memory* GetMemory() const { return memory_; }

  /** Asks the thread to stop at its next kernel boundary. */
  void RequestStop() { stop_requested_.store(true, std::memory_order_release); }

  /**
   * Reports whether the thread has a stop request.
   *
   * @return True when RequestStop ran.
   */
  bool IsStopRequested() const { return stop_requested_.load(std::memory_order_acquire); }

  /** Drops the stop request once the thread runs its exit sequence. */
  void ClearStop() { stop_requested_.store(false, std::memory_order_release); }
  /**
   * Returns the register file of this thread.
   *
   * @return The context.
   */
  ::PPCContext* GetContext() const { return context_; }
  u32 GetThreadId() const { return thread_id_; }

  /**
   * Returns the runtime the thread belongs to.
   *
   * @return The runtime, null for a state built without one.
   */
  Runtime* GetRuntime() const { return runtime_; }

  /**
   * Returns the FP and vector save area of the Ke save exports.
   *
   * @return The area.
   */
  DPCSaveArea& GetDPCSaveArea() { return dpc_save_area_; }

  /**
   * Binds a thread state to the calling host thread.
   *
   * @param state The state, null to unbind.
   */
  static void Bind(ThreadState* state);
  /**
   * Returns the thread state bound to the calling host thread.
   *
   * @return The state, null when none is bound.
   */
  static ThreadState* Get();
  /**
   * Returns the thread id of the bound thread state.
   *
   * @return The id, 0xFFFFFFFF when none is bound.
   */
  static u32 GetThreadID();

 private:
  Memory* memory_;
  Runtime* runtime_;

  u32 thread_id_ = 0;

  alignas(64)::PPCContext context_storage_{};
  ::PPCContext* context_ = &context_storage_;
  DPCSaveArea dpc_save_area_;
  std::atomic<bool> stop_requested_{false};
};

namespace detail {

/** Logs that GetCurrent() ran on a thread with no runtime and aborts, in every build. */
[[noreturn]] REX_RUNTIME_API void AbortNoRuntime();

}  // namespace detail

/**
 * Returns the runtime the calling thread belongs to.
 *
 * @return The runtime of the title or worker thread bound to the caller. Aborts on a host thread.
 */
inline Runtime& GetCurrent() {
  ThreadState* state = ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) [[unlikely]] {
    detail::AbortNoRuntime();
  }
  return *state->GetRuntime();
}

}  // namespace rex::runtime
