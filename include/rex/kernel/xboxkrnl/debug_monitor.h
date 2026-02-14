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

struct X_KEDEBUGMONITORDATA {
  rex::be<uint32_t> unk_00;       // 0x00
  rex::be<uint32_t> unk_04;       // 0x04
  rex::be<uint32_t> unk_08;       // 0x08
  rex::be<uint32_t> unk_0C;       // 0x0C
  rex::be<uint32_t> unk_10;       // 0x10
  rex::be<uint32_t> unk_14;       // 0x14
  rex::be<uint32_t> callback_fn;  // 0x18 function
  rex::be<uint32_t> unk_1C;       // 0x1C
  rex::be<uint32_t> unk_20;       // 0x20 Vd graphics data?
};

void KeDebugMonitorCallback(::PPCContext* ppc_context, rex::system::KernelState* kernel_state);

}  // namespace rex::kernel::xboxkrnl
