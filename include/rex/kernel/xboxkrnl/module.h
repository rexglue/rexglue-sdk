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
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/platform/thread.h>

// All of the exported functions:
#include <rex/kernel/xboxkrnl/rtl.h>

namespace rex::kernel::xboxkrnl {

/**
 * Creates the xboxkrnl kernel module and binds its variable exports.
 *
 * @param kernel_state The kernel state.
 * @return The handle with a count of 1, 0 when the table cannot grow.
 */
X_HANDLE CreateXboxkrnlModule(system::KernelState* kernel_state);

}  // namespace rex::kernel::xboxkrnl
