/**
 * @file        build_stamp.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/export.h>

namespace rex {

/**
 * Returns the build stamp. The time in it is the configure time of the build tree.
 *
 * @code
 * "build: rexglue-v0.7.5.38-dev.gb27f3f74-win-amd64-Release@20260420_1430"
 * @endcode
 *
 * @return The stamp, a string that lasts for the life of the process.
 */
REX_API const char* GetBuildStamp();

}  // namespace rex