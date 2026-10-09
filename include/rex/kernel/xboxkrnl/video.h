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

#include <rex/macros.h>
#include <rex/runtime/xtypes.h>
#include <rex/runtime/xvideo.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {
struct ModuleObject;
}  // namespace rex::runtime

namespace rex::kernel::xboxkrnl {

/**
 * Fills a video mode structure from the runtime display settings.
 *
 * @param video_mode Receives the video mode.
 */
REX_RUNTIME_API void VdQueryVideoMode(runtime::X_VIDEO_MODE* video_mode);

/**
 * Registers the video exports of xboxkrnl.exe on its module.
 *
 * @param module The xboxkrnl.exe module.
 * @param runtime The runtime the module belongs to.
 */
REX_RUNTIME_API void RegisterVideoExports(runtime::ModuleObject& module, Runtime& runtime);

}  // namespace rex::kernel::xboxkrnl
