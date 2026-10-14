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
#include <rex/runtime.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

std::atomic<int> xam_dialogs_shown = {0};

bool XamIsUIActive() {
  return xam_dialogs_shown > 0;
}

X_HANDLE CreateXAMModule(Runtime& runtime) {
  return CreateKernelModule(runtime, "xe:\\xam.xex");
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex
