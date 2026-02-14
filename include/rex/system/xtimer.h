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

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

namespace rex::system {

class XThread;

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);

  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine, uint32_t routine_arg,
                    bool resume);
  X_STATUS Cancel();

 protected:
  rex::thread::WaitHandle* GetWaitHandle() override { return timer_.get(); }

 private:
  std::unique_ptr<rex::thread::Timer> timer_;

  XThread* callback_thread_ = nullptr;
  uint32_t callback_routine_ = 0;
  uint32_t callback_routine_arg_ = 0;
};

}  // namespace rex::system
