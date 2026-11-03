/**
 * @file        rexglue/log_categories.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>

#include <rex/logging/log.h>

#include "codegen/codegen_categories.h"

REXLOG_CATEGORY(rexglue, "The rexglue command line tool")

namespace rexglue {

/** Categories the tool owns, not those of connected modules. */
inline constexpr std::array<const rex::log::Category*, 2> kLogCategories = {&rex::log::codegen,
                                                                            &rex::log::rexglue};

}  // namespace rexglue
