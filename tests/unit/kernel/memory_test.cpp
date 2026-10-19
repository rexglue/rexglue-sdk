/**
 * @file        tests/unit/kernel/memory_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kMemCommit = 0x1000;
constexpr u32 kMemReserve = 0x2000;
constexpr u32 kMemReset = 0x80000;
constexpr u32 kPageReadWrite = 0x04;
constexpr u32 kPageSize = 0x10000;
constexpr u32 kStateOffset = 16;
}  // namespace

REX_TEST_CASE("memory: a reset leaves committed pages committed", "[kernel][memory]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    c.Store32(base_ptr, 0);
    c.Store32(size_ptr, kPageSize);
    CHECKED_ELSE(c.Kernel("NtAllocateVirtualMemory",
                          {base_ptr, size_ptr, kMemCommit | kMemReserve, kPageReadWrite, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 base = c.Load32(base_ptr);
    c.Store32(size_ptr, kPageSize);
    CHECK(c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, kMemReset, kPageReadWrite, 0}) ==
          0);
    CHECK(c.Load32(base_ptr) == base);
    const u32 info = c.Alloc(28);
    REQUIRE(c.Kernel("NtQueryVirtualMemory", {base, info, 0}) == 0);
    CHECK(c.Load32(info + kStateOffset) == kMemCommit);
  });
}
