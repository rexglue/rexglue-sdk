/**
 * @file        kernel/kernel_control.h
 * @brief       Settings the app passes to the kernel module
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/module.h>

namespace rex::kernel {

/** Interface the app uses to configure the kernel module before it starts. */
REX_DECLARE_PRIVATE_INTERFACE(IKernelControl, 1)
/**
 * Sets the private factory the kernel gets its internal UI, graphics and settings interfaces
 * from. The app calls it between Connect and Init.
 *
 */
  virtual void SetControls(IFactory& controls) = 0;
};

}  // namespace rex::kernel
