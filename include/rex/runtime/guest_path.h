/**
 * @file        rex/runtime/guest_path.h
 * @brief       Guest path normalization for Xbox 360 VFS paths
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/macros.h>

namespace rex::runtime {

/**
 * Normalize a guest path to the canonical form used as a key for runtime
 * lookups: device prefix stripped, separators canonicalized to forward
 * slashes, redundant `..` segments resolved, leading slashes removed,
 * and ASCII lowercased. Codegen-side canonicalization
 * (rex::codegen::CanonicalizeModuleGuestPath) layers a project-scoped prefix
 * strip on top of this; both must agree on this base form for runtime
 * lookups to find the registered module.
 */
REX_RUNTIME_API std::string NormalizeGuestPath(std::string_view path);

}  // namespace rex::runtime
