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

#pragma once

#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>

namespace rex::runtime {
class ExportResolver;
}

namespace rex::system {
class KernelState;
}

namespace rex::kernel::xboxkrnl {

void VdQueryVideoMode(system::X_VIDEO_MODE* video_mode);

// Register video variable exports (VdGlobalDevice, VdHSIOCalibrationLock, etc.)
// Must be called during kernel initialization before XEX modules are loaded.
void RegisterVideoExports(rex::runtime::ExportResolver* export_resolver,
                          system::KernelState* kernel_state);

}  // namespace rex::kernel::xboxkrnl
