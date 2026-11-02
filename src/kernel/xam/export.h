/**
 * @file        kernel/xam/export.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>

#include <rex/kernel/export.h>
#include <rex/types.h>

namespace rex::kernel {

/** Returns the XAM export table, sorted by unique ordinal. */
std::span<const ExportEntry> GetXAMExports();

/** Reports whether the XAM export of an ordinal never returns to its caller. */
bool IsNoReturnXAMExport(u16 ordinal);

}  // namespace rex::kernel
