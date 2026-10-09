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

#include <rex/macros.h>
#include <rex/runtime.h>

namespace rex {
namespace kernel {
namespace xam {

/**
 * Reports whether a Xam dialog is on screen.
 *
 * @return True when at least 1 Xam dialog is shown.
 */
REX_RUNTIME_API bool XamIsUIActive();

}  // namespace xam
}  // namespace kernel
}  // namespace rex
