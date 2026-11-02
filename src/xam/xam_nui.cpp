/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/logging/log.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/title_runtime.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

struct X_NUI_DEVICE_STATUS {
  be_u32 unk0;
  be_u32 unk1;
  be_u32 unk2;
  be_u32 status;
  be_u32 unk4;
  be_u32 unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

void XamNuiGetDeviceStatus_entry(GuestPtrT<X_NUI_DEVICE_STATUS> status_ptr) {
  status_ptr.Zero();
  // 0 is not connected.
  status_ptr->status = 0;
}

u32 XamShowNuiTroubleshooterUI_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                                     [[maybe_unused]] u32 unk3) {
  return 0;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamNuiGetDeviceStatus, rex::xam::XamNuiGetDeviceStatus_entry)
REX_XAM_BODY(__imp__XamShowNuiTroubleshooterUI, rex::xam::XamShowNuiTroubleshooterUI_entry)
