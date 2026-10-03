/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

#include "runtime/xma/context.h"
#include "runtime/xma/register_file.h"
#include <rex/bit.h>
#include <rex/kernel.h>

namespace rex::runtime {

class FunctionDispatcher;

struct XMA_CONTEXT_DATA;

class XMADecoder {
 public:
  explicit XMADecoder(FunctionDispatcher* function_dispatcher);
  ~XMADecoder();

  Memory* memory() const { return memory_; }
  FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }

  X_STATUS Setup();
  void Shutdown();

  uint32_t context_array_ptr() const { return register_file_[XMARegister::ContextArrayAddress]; }

  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_ptr);
  bool BlockOnContext(uint32_t guest_ptr, bool poll);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  int GetContextId(uint32_t guest_ptr) const;

 private:
  void WorkerThreadMain(const std::stop_token& stop);

  static u32 MMIOReadRegisterThunk(void* context, u32 addr) {
    return static_cast<XMADecoder*>(context)->ReadRegister(addr);
  }
  static void MMIOWriteRegisterThunk(void* context, u32 addr, u32 value) {
    static_cast<XMADecoder*>(context)->WriteRegister(addr, value);
  }

 protected:
  Memory* memory_ = nullptr;
  FunctionDispatcher* function_dispatcher_ = nullptr;

  std::unique_ptr<rex::platform::Event> work_event_ = nullptr;
  std::unique_ptr<rex::platform::Event> worker_exited_ = nullptr;
  std::jthread worker_;

  std::atomic<bool> paused_ = false;
  rex::platform::Fence pause_fence_;   // Signaled when worker paused.
  rex::platform::Fence resume_fence_;  // Signaled when resume requested.

  XMARegisterFile register_file_;

  static const uint32_t kContextCount = 320;
  XMAContext contexts_[kContextCount];
  BitMap context_bitmap_;

  uint32_t context_data_first_ptr_ = 0;
  uint32_t context_data_last_ptr_ = 0;
};

}  // namespace rex::runtime
