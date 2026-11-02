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
#include <rex/kernel/title_runtime.h>

namespace rex {
namespace xam {

/** Reports whether an XAM dialog is on screen. */
bool XAMIsUIActive();

}  // namespace xam
}  // namespace rex
