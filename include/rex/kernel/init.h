/**
 * @file        kernel/init.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/export.h>
#include <rex/result.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::kernel {

/**
 * Registers the XAM apps and loads the xboxkrnl, XAM and XBDM kernel modules.
 *
 * @param runtime The runtime being set up.
 * @return Ok, or the error of the first kernel module that did not load.
 */
[[nodiscard]] REX_RUNTIME_API Result<void> InitializeKernel(Runtime& runtime);

}  // namespace rex::kernel
