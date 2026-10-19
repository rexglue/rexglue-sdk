/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/export.h>
#include <rex/runtime.h>

namespace rex {
namespace kernel {
namespace xam {

/**
 * Reports whether an XAM dialog is on screen.
 *
 * @return True when at least 1 XAM dialog is shown.
 */
REX_RUNTIME_API bool XAMIsUIActive();

}  // namespace xam
}  // namespace kernel
}  // namespace rex
