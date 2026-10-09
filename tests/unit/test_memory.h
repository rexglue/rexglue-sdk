/**
 * @file        tests/unit/test_memory.h
 * @brief       Single shared guest Memory instance for the unit test binary
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/runtime/xmemory.h>

namespace rex::testing {

/**
 * Returns the process wide guest Memory, creating it on first use.
 *
 * @return The shared Memory.
 */
rex::runtime::Memory& GetTestMemory();

/**
 * Destroys the shared Memory so another owner can create its own.
 */
void ReleaseTestMemory();

}  // namespace rex::testing
