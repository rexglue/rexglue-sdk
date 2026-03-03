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
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

void XGetVideoMode_entry(ppc_ptr_t<X_VIDEO_MODE> video_mode) {
  // TODO(benvanik): actually check to see if these are the same.
  xboxkrnl::VdQueryVideoMode(std::move(video_mode));
}

ppc_u32_result_t XGetVideoCapabilities_entry() {
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XGetVideoMode, rex::kernel::xam::XGetVideoMode_entry)
XAM_EXPORT(__imp__XGetVideoCapabilities, rex::kernel::xam::XGetVideoCapabilities_entry)

XAM_EXPORT_STUB(__imp__XGetVideoFlags);
XAM_EXPORT_STUB(__imp__XGetVideoStandard);
XAM_EXPORT_STUB(__imp__XamLoadExtraAVCodecs2);
XAM_EXPORT_STUB(__imp__XamUnloadExtraAVCodecs2);
