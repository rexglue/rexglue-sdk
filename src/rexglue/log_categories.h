/**
 * @file        rexglue/log_categories.h
 * @brief       The log categories the rexglue tool registers at log start
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>

#include <rex/logging.h>

namespace rexglue {

/** Categories the tool owns. A category a connected module registers is never listed here. */
inline constexpr std::array<const rex::log::Category*, 2> kLogCategories = {&rex::log::codegen,
                                                                            &rex::log::rexglue};

}  // namespace rexglue
