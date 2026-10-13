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
 * Returns the guest Memory shared by the test case, creating it on first use. It is destroyed when
 * the test case ends, so a later test case can create its own.
 *
 * @return The shared Memory.
 */
rex::runtime::Memory& GetTestMemory();

/**
 * Destroys the shared Memory so another owner can create its own within the same test case.
 */
void ReleaseTestMemory();

}  // namespace rex::testing
