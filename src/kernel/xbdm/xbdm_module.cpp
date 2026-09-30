/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Joseph Lee, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xbdm/module.h>
#include <rex/kernel/xbdm/private.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xbdm {
using namespace rex::system;

XbdmModule::XbdmModule(Runtime* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xbdm.xex") {}

XbdmModule::~XbdmModule() {}

}  // namespace xbdm
}  // namespace kernel
}  // namespace rex
