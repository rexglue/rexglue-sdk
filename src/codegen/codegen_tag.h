/**
 * @file        codegen/codegen_tag.h
 * @brief       The host allocation tag of the recompiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/memory/host.h>

namespace rex::codegen {

/** The tag of the recompiler. It is named "Codegen" by the codegen library at load. */
inline constexpr memory::Tag kCodegen{8};

}  // namespace rex::codegen
