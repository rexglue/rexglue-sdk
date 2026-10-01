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

namespace rex::system {
struct ModuleObject;
class KernelState;
}  // namespace rex::system

namespace rex::kernel::xboxkrnl {

void VdQueryVideoMode(system::X_VIDEO_MODE* video_mode);

/**
 * Registers the video variable exports (VdGlobalDevice, VdHSIOCalibrationLock, etc.). Must be
 * called during kernel initialization before XEX modules are loaded.
 *
 * @param module The xboxkrnl module.
 * @param kernel_state The kernel state.
 */
void RegisterVideoExports(system::ModuleObject& module, system::KernelState* kernel_state);

}  // namespace rex::kernel::xboxkrnl
