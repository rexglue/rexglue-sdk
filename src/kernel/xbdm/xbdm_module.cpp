/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Joseph Lee, 2026 - Adapted for ReXGlue runtime
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xbdm/module.h>
#include <rex/runtime.h>

namespace rex {
namespace kernel {
namespace xbdm {
using namespace rex::runtime;

X_HANDLE CreateXBDMModule(Runtime& runtime) {
  return ModuleObject::CreateKernelModule(runtime, "xe:\\xbdm.xex");
}

}  // namespace xbdm
}  // namespace kernel
}  // namespace rex
