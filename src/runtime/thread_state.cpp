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

#include <cstdlib>

#include <rex/assert.h>
#include <rex/kernel.h>
#include <rex/logging.h>
#include <rex/runtime/thread_state.h>
#include <rex/platform/fpscr.h>
#include <rex/platform/thread.h>

namespace rex::runtime {

void detail::AbortNoRuntime() {
  REX_FATAL(system, "Current() called on thread 0x{:X}, which has no runtime",
            rex::platform::current_thread_id());
}

thread_local ThreadState* thread_state_ = nullptr;

ThreadState::ThreadState(u32 thread_id, u32 stack_base, u32 pcr_address, Memory* memory,
                         Runtime* runtime)
    : memory_(memory), runtime_(runtime), pcr_address_(pcr_address), thread_id_(thread_id) {
  if (thread_id_ == UINT_MAX) {
    // System thread IDs set bit 31.
    uint32_t system_thread_handle = rex::platform::current_thread_system_id();
    thread_id_ = 0x80000000 | system_thread_handle;
  }

  // r1 is the stack pointer and r13 holds the PCR address.
  context_->r1.u64 = stack_base;
  context_->r13.u64 = pcr_address;
  rex::platform::FPSCRPlatform::InitHostExceptions(dpc_save_area_.csr);
}

ThreadState::~ThreadState() {
  if (thread_state_ == this) {
    thread_state_ = nullptr;
  }
}

void ThreadState::Bind(ThreadState* thread_state) {
  thread_state_ = thread_state;
}

ThreadState* ThreadState::Get() {
  return thread_state_;
}

uint32_t ThreadState::GetThreadID() {
  auto* thread_state = Get();
  return thread_state ? thread_state->thread_id_ : 0xFFFFFFFF;
}

}  // namespace rex::runtime
