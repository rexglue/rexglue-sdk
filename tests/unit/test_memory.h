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
 * Returns the guest address space shared by the test case, creating it on first use.
 * It is destroyed when the test case ends.
 */
rex::kernel::GuestAddressSpace& GetTestMemory();

/** Destroys the shared guest address space so another owner can create its own. */
void ReleaseTestMemory();

}  // namespace rex::test
