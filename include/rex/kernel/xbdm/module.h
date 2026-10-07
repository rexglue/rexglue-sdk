#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Joseph Lee, 2026 - Adapted for ReXGlue runtime
 */

#include <string>

#include <rex/macros.h>
#include <rex/ppc/function.h>
#include <rex/runtime/kernel_module.h>
#include <rex/runtime.h>

namespace rex::kernel::xbdm {

/**
 * Creates the xbdm.xex kernel module and registers its exports.
 *
 * @param runtime The runtime the module belongs to.
 * @return A handle of the module with a count of 1.
 */
REX_RUNTIME_API X_HANDLE CreateXbdmModule(Runtime& runtime);

}  // namespace rex::kernel::xbdm
