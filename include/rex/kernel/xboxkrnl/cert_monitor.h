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

#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/thread.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl {

struct X_KECERTMONITORDATA {
  rex::be<uint32_t> callback_fn;
};

void KeCertMonitorCallback(::PPCContext* ppc_context, rex::system::KernelState* kernel_state);

}  // namespace rex::kernel::xboxkrnl
