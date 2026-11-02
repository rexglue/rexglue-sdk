/**
 * @file        kernel/kernel_init.h
 * @brief       Kernel bootstrap and the creation of the xboxkrnl, XAM and XBDM kernel modules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/kernel/xboxkrnl/xboxkrnl_module.h,
 *              xenia/kernel/xam/xam_module.h and xenia/kernel/xbdm/xbdm_module.h
 *              (Ben Vanik, 2013).
 *
 * @modified    Joseph Lee, 2026 - Adapted for ReXGlue runtime
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/ppc/call.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/thread.h>

#include <rex/kernel/xboxkrnl.h>

namespace rex::kernel {

/**
 * Registers the XAM apps and loads the xboxkrnl, XAM and XBDM kernel modules.
 *
 * @return Ok, or the error of the first kernel module that did not load.
 */
[[nodiscard]] Result<void> InitializeKernel(TitleRuntime& runtime);

}  // namespace rex::kernel

namespace rex::xam {}  // namespace rex::xam

namespace rex::kernel::xbdm {

/**
 * Creates the xbdm.xex kernel module and registers its exports.
 *
 * @return A handle of the module with a count of 1.
 */
X_HANDLE CreateXBDMModule(TitleRuntime& runtime);

}  // namespace rex::kernel::xbdm

namespace rex::kernel::xboxkrnl {

/**
 * Creates the xboxkrnl.exe kernel module and registers its exports.
 *
 * @return A handle of the module with a count of 1.
 */
X_HANDLE CreateXboxkrnlModule(TitleRuntime& runtime);

}  // namespace rex::kernel::xboxkrnl
