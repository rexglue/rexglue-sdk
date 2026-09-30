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

#include <memory>

#include <rex/ppc/function.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/platform/thread.h>

// All of the exported functions:
#include <rex/kernel/xboxkrnl/rtl.h>

namespace rex::kernel::xboxkrnl {

class XboxkrnlModule : public system::KernelModule {
 public:
  XboxkrnlModule(Runtime* emulator, system::KernelState* kernel_state);
  virtual ~XboxkrnlModule();

 private:
  std::unique_ptr<rex::platform::HighResolutionTimer> timestamp_timer_;
};

}  // namespace rex::kernel::xboxkrnl
