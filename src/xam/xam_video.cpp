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

#include "xam/xam_ui.h"
#include <rex/kernel/xboxkrnl.h>
#include <rex/logging/log.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include "xam/xam_bodies.h"
#include "xam/xam_state.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

void XGetVideoMode_entry(GuestPtrT<kernel::X_VIDEO_MODE> video_mode) {
  kernel::FillXVideoMode(CurrentXAM().GetVideoMode(), *video_mode);
}

u32 XGetVideoCapabilities_entry() {
  return 0;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XGetVideoMode, rex::xam::XGetVideoMode_entry)
REX_XAM_BODY(__imp__XGetVideoCapabilities, rex::xam::XGetVideoCapabilities_entry)
