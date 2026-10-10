/**
 * @file        tests/unit/runtime/xsemaphore_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "kernel/kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

REX_TEST_CASE("A title initialized semaphore with a limit above the int range still works",
              "[runtime][semaphore]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 semaphore = c.Alloc(0x14);
    c.Kernel("KeInitializeSemaphore", {semaphore, 0, 0x80000000});
    CHECK(c.Kernel("KeReleaseSemaphore", {semaphore, 0, 1, 0}) == 0);
    CHECK(c.Kernel("KeReleaseSemaphore", {semaphore, 0, 1, 0}) == 1);
  });
}
