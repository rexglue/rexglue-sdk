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

#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xstatus.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

void XGetVideoMode_entry(GuestPtrT<X_VIDEO_MODE> video_mode) {
  xboxkrnl::VdQueryVideoMode(video_mode);
}

u32 XGetVideoCapabilities_entry() {
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XGetVideoMode, rex::kernel::xam::XGetVideoMode_entry)
REX_EXPORT(__imp__XGetVideoCapabilities, rex::kernel::xam::XGetVideoCapabilities_entry)

REX_EXPORT_STUB(__imp__XGetVideoFlags);
REX_EXPORT_STUB(__imp__XGetVideoStandard);
REX_EXPORT_STUB(__imp__XamLoadExtraAVCodecs2);
REX_EXPORT_STUB(__imp__XamUnloadExtraAVCodecs2);
