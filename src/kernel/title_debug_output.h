/**
 * @file        kernel/title_debug_output.h
 * @brief       Writes the printf style debug calls of the kernel exports to the debug output
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/ppc/context.h>
#include <rex/types.h>

namespace rex::kernel {

/**
 * Formats a variadic call and writes it to the debug output of the runtime of the calling thread.
 * Does nothing when the output is disabled or the call does not format.
 *
 * @param ctx The register file at the entry of the export.
 * @param format_index The argument index of the format pointer.
 */
void WritePrintf(const PPCContext& ctx, u32 format_index);

}  // namespace rex::kernel
