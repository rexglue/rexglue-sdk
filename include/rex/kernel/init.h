/**
 * @file        kernel/init.h
 * @brief       Kernel initialization entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::kernel {

/**
 * Registers the Xam apps and loads the xboxkrnl, Xam and Xbdm kernel modules.
 *
 * @param runtime The runtime being set up.
 */
void InitializeKernel(Runtime& runtime);

}  // namespace rex::kernel
