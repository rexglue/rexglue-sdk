/**
 * @file        tests/unit/test_memory.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/kernel/heap.h>

namespace rex::test {

/**
 * Returns the guest Memory shared by the test case, creating it on first use.
 * It is destroyed when the test case ends.
 */
rex::kernel::Memory& GetTestMemory();

/** Destroys the shared Memory so another owner can create its own. */
void ReleaseTestMemory();

}  // namespace rex::test
