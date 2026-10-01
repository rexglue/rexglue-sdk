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

#include <rex/ppc/function.h>
#include <rex/runtime/kernel_module.h>
#include <rex/runtime.h>
#include <rex/platform/thread.h>

// All of the exported functions:
#include <rex/kernel/xboxkrnl/rtl.h>

namespace rex::kernel::xboxkrnl {

/**
 * Creates the xboxkrnl.exe kernel module and registers its exports.
 *
 * @param runtime The runtime the module belongs to.
 * @return A handle of the module with a count of 1.
 */
X_HANDLE CreateXboxkrnlModule(Runtime& runtime);

}  // namespace rex::kernel::xboxkrnl
