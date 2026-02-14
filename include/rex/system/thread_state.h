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

}  // namespace rex::runtime
