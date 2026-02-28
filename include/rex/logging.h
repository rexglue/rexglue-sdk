/**
 * @file        logging.h
 * @brief       Logging subsystem umbrella header
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

// Category types, constants, configuration, deprecated enum compat
#include <rex/logging/types.h>

// Function declarations, CVAR declarations
#include <rex/logging/api.h>

// Logging macros (parameterized, legacy aliases, FN, TID)
#include <rex/logging/macros.h>

// Fatal error and assertion macros
#include <rex/logging/assert.h>

// Formatting helpers (ptr, hex, boolean)
#include <rex/logging/format.h>
