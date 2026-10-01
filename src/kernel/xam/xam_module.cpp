/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>

#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

std::atomic<int> xam_dialogs_shown_ = {0};

bool XamIsUIActive() {
  return xam_dialogs_shown_ > 0;
}

X_HANDLE CreateXamModule(KernelState* kernel_state) {
  return CreateKernelModule(kernel_state, "xe:\\xam.xex");
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex
