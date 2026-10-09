/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>

#include <rex/macros.h>
#include <rex/ppc/function.h>
#include <rex/runtime/kernel_module.h>
#include <rex/runtime.h>

namespace rex::kernel::xam {

/**
 * Creates the xam.xex kernel module and registers its exports.
 *
 * @param runtime The runtime the module belongs to.
 * @return A handle of the module with a count of 1.
 */
REX_RUNTIME_API X_HANDLE CreateXamModule(Runtime& runtime);

}  // namespace rex::kernel::xam
