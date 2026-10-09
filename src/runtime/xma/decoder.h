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
#include <rex/macros.h>

namespace rex::runtime {

class FunctionDispatcher;

struct XMA_CONTEXT_DATA;

/**
 * Decodes XMA audio for the APU.
 * Titles reach it through the XMA exports or by writing the contexts and registers,
 * after mapping them with MmMapIoSpace.
 */
class REX_RUNTIME_API XMADecoder {
 public:
  explicit XMADecoder(FunctionDispatcher* function_dispatcher);
  ~XMADecoder();

  Memory* memory() const { return memory_; }
  FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }

  /**
   * Allocates the context array, attaches the APU MMIO block and starts the worker.
   *
   * Contexts live in physical memory and the ContextArrayAddress register holds the physical
   * address of the array.
   *
   * @return X_STATUS_SUCCESS.
   */
  X_STATUS Setup();
  void Shutdown();

  uint32_t context_array_ptr() const { return register_file_[XMARegister::ContextArrayAddress]; }

  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_ptr);
  bool BlockOnContext(uint32_t guest_ptr, bool poll);

  /**
   * Reads an APU register.
   *
   * Register 0x0606 returns a rotating context index so a title never sees a stuck context.
   *
   * @param addr Register address, the low 16 bits select the register.
   * @return The register value, byte swapped.
   */
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
  rex::platform::Fence pause_fence_;  /**< Signaled when the worker paused */
  rex::platform::Fence resume_fence_; /**< Signaled when resume is requested */

  XMARegisterFile register_file_;

  static const uint32_t kContextCount = 320;
  XMAContext contexts_[kContextCount];
  BitMap context_bitmap_;

  uint32_t context_data_first_ptr_ = 0;
  uint32_t context_data_last_ptr_ = 0;
};

}  // namespace rex::runtime
