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
#include <rex/platform.h>
#include <rex/types.h>
#include <rex/bit.h>
#include <rex/kernel.h>
#include <rex/export.h>

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

  Memory* GetMemory() const { return memory_; }
  FunctionDispatcher* GetFunctionDispatcher() const { return function_dispatcher_; }

  /**
   * Allocates the context array, attaches the APU MMIO block and starts the worker.
   *
   * Contexts reside in physical memory and the ContextArrayAddress register holds the physical
   * address of the array.
   *
   * @return X_STATUS_SUCCESS.
   */
  X_STATUS Setup();
  void Shutdown();

  u32 GetContextArrayPtr() const { return register_file_[XMARegister::kContextArrayAddress]; }

  u32 AllocateContext();

  /**
   * Releases an allocated context. Does nothing for a pointer that is not an allocated context.
   *
   * @param context_ptr The context address.
   */
  void ReleaseContext(u32 context_ptr);

  /**
   * Waits for a context to finish its current packet.
   *
   * @param context_ptr The context address.
   * @param poll True to check without waiting.
   * @return True when the context is idle, also for a pointer that is not a context.
   */
  bool BlockOnContext(u32 context_ptr, bool poll);

  /**
   * Sets the bit of a context in an indexed register such as the Kick or Lock registers.
   *
   * @param base_register The first register of the indexed range.
   * @param context_physical_address The physical address of the context.
   * @return False and no register written when the address is not inside the context array.
   */
  bool SignalContext(u32 base_register, u32 context_physical_address);

  /**
   * Reads an APU register.
   *
   * Register 0x0606 returns a rotating context index so a title never sees a stuck context.
   *
   * @param addr Register address, the low 16 bits select the register.
   * @return The register value, byte swapped.
   */
  u32 ReadRegister(u32 addr);
  void WriteRegister(u32 addr, u32 value);

  bool IsPaused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  int GetContextId(u32 context_ptr) const;

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

  static const u32 kContextCount = 320;
  XMAContext contexts_[kContextCount];
  BitMap context_bitmap_;

  u32 context_data_first_ptr_ = 0;
  u32 context_data_last_ptr_ = 0;
};

}  // namespace rex::runtime
