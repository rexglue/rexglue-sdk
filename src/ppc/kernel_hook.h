/**
 * @file        ppc/kernel_hook.h
 * @brief       Kernel calls the PPC runtime makes from its own sources
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::ppc::detail {

/**
 * Writes the text of a debug print service trap through the kernel hooks. Does nothing when no
 * kernel hooks are set.
 *
 * @param address The guest address of the text.
 * @param length The length of the text in bytes.
 */
void WriteDebugText(u32 address, u32 length);

}  // namespace rex::ppc::detail
