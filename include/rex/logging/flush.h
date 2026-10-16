/**
 * @file        logging/flush.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/export.h>

namespace rex::log {

/** Writes buffered output of every sink. For crash handlers that hold no API handle. */
REX_API void Flush();

}  // namespace rex::log
