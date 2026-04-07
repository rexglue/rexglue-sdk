#pragma once
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

#include <cassert>
#include <string>

#include <rex/memory.h>
#include <rex/ppc/context.h>

namespace rex::runtime {

class ThreadState {
 public:
  // rexglue constructor - takes Memory directly, no Processor needed (maybe add later)
  ThreadState(uint32_t thread_id, uint32_t stack_base, uint32_t pcr_address,
              memory::Memory* memory);
  ~ThreadState();

  memory::Memory* memory() const { return memory_; }
  ::PPCContext* context() const { return context_; }
  uint32_t thread_id() const { return thread_id_; }

  static void Bind(ThreadState* thread_state);
  static ThreadState* Get();
  static uint32_t GetThreadID();

 private:
  memory::Memory* memory_;

  uint32_t pcr_address_ = 0;
  uint32_t thread_id_ = 0;

  // NOTE: must be 64b aligned for SSE ops.
  alignas(64)::PPCContext context_storage_;
  ::PPCContext* context_ = &context_storage_;
};

// Thread-safe accessors for current thread's PPC context and kernel state.
// Require ThreadState::Bind() to have been called on the current thread.
inline PPCContext* current_ppc_context() {
  auto* ts = ThreadState::Get();
  assert(ts && "current_ppc_context() called without bound ThreadState");
  return ts->context();
}

}  // namespace rex::runtime

// Forward declaration in correct namespace
namespace rex::system {
class KernelState;
KernelState* kernel_state();
}  // namespace rex::system

namespace rex::runtime {

inline rex::system::KernelState* current_kernel_state() {
  return rex::system::kernel_state();
}

}  // namespace rex::runtime
