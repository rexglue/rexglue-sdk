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

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {
constexpr u32 kMemCommit = 0x1000;
constexpr u32 kMemReserve = 0x2000;
constexpr u32 kMemDecommit = 0x4000;
constexpr u32 kMemRelease = 0x8000;
constexpr u32 kMemReset = 0x80000;
constexpr u32 kMemTopDown = 0x100000;
constexpr u32 kMemNoZero = 0x800000;
constexpr u32 kPageNoAccess = 0x01;
constexpr u32 kPageReadOnly = 0x02;
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

REX_TEST_CASE("memory: an address outside every heap fails instead of reaching a null heap",
              "[kernel][memory]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kUnmapped = 0x7F100000;
    constexpr u32 kUnmappedHigh = 0xFFE00000;
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    for (const u32 address : {kUnmapped, kUnmappedHigh}) {
      c.Store32(base_ptr, address);
      c.Store32(size_ptr, kPageSize);
      CHECK(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, kPageReadWrite, old_ptr, 0}) ==
            rex::X_STATUS_INVALID_PARAMETER);
      c.Store32(base_ptr, address);
      c.Store32(size_ptr, 0);
      CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, kMemRelease, 0}) ==
            rex::X_STATUS_MEMORY_NOT_ALLOCATED);
      c.Kernel("MmFreePhysicalMemory", {0, address});
      CHECK(c.Kernel("MmQueryAddressProtect", {address}) == 0);
      CHECK(c.Kernel("MmQueryAllocationSize", {address}) == 0);
    }
  });
}

REX_TEST_CASE("memory: regions 0 to 2 use the guest heap and larger values are rejected",
              "[kernel][memory]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    for (const u32 region : {0u, 1u, 2u}) {
      c.Store32(base_ptr, 0);
      c.Store32(size_ptr, kPageSize);
      CHECKED_ELSE(
          c.Kernel("NtAllocateVirtualMemory",
                   {base_ptr, size_ptr, kMemCommit | kMemReserve, kPageReadWrite, region}) == 0) {
        FAIL_CHECK("the check above failed");
        return;
      }
      CHECK(c.Kernel("NtProtectVirtualMemory",
                     {base_ptr, size_ptr, kPageReadWrite, old_ptr, region}) == 0);
      c.Store32(size_ptr, 0);
      CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, kMemRelease, region}) == 0);
    }
    for (const u32 region : {3u, 0x100u, 0xFFFFFFFFu}) {
      c.Store32(base_ptr, 0);
      c.Store32(size_ptr, kPageSize);
      CHECK(c.Kernel("NtAllocateVirtualMemory",
                     {base_ptr, size_ptr, kMemCommit | kMemReserve, kPageReadWrite, region}) ==
            rex::X_STATUS_INVALID_PARAMETER);
      CHECK(c.Load32(base_ptr) == 0);
      CHECK(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, kPageReadWrite, old_ptr,
                                                region}) == rex::X_STATUS_INVALID_PARAMETER);
      CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, kMemRelease, region}) ==
            rex::X_STATUS_INVALID_PARAMETER);
    }
  });
}

REX_TEST_CASE("memory: an allocation type outside the accepted set is rejected",
              "[kernel][memory]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    for (const u32 type :
         {0u, kMemDecommit, kMemRelease, kMemCommit | kMemDecommit, kMemReserve | 0x80000000u,
          kMemReset | kMemCommit, kMemReset | kMemTopDown, kMemTopDown, kMemNoZero}) {
      INFO(type);
      c.Store32(base_ptr, 0);
      c.Store32(size_ptr, kPageSize);
      CHECK(c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, type, kPageReadWrite, 0}) ==
            rex::X_STATUS_INVALID_PARAMETER);
      CHECK(c.Load32(base_ptr) == 0);
    }
  });
}

REX_TEST_CASE("memory: top down and no zero allocations are accepted", "[kernel][memory]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 low_ptr = c.Alloc(4);
    const u32 high_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    c.Store32(low_ptr, 0);
    c.Store32(size_ptr, kPageSize);
    CHECK(c.Kernel("NtAllocateVirtualMemory",
                   {low_ptr, size_ptr, kMemCommit | kMemReserve, kPageReadWrite, 0}) == 0);
    c.Store32(high_ptr, 0);
    c.Store32(size_ptr, kPageSize);
    CHECK(c.Kernel("NtAllocateVirtualMemory",
                   {high_ptr, size_ptr, kMemCommit | kMemReserve | kMemTopDown | kMemNoZero,
                    kPageReadWrite, 0}) == 0);
    CHECK(c.Load32(high_ptr) > c.Load32(low_ptr));
  });
}

REX_TEST_CASE("memory: a free type other than decommit or release is rejected",
              "[kernel][memory]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    c.Store32(base_ptr, 0);
    c.Store32(size_ptr, kPageSize);
    REQUIRE(c.Kernel("NtAllocateVirtualMemory",
                     {base_ptr, size_ptr, kMemCommit | kMemReserve, kPageReadWrite, 0}) == 0);
    const u32 base = c.Load32(base_ptr);
    for (const u32 type : {0u, kMemCommit, kMemDecommit | kMemRelease, kMemRelease | kMemReset}) {
      INFO(type);
      c.Store32(base_ptr, base);
      c.Store32(size_ptr, 0);
      CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, type, 0}) ==
            rex::X_STATUS_INVALID_PARAMETER);
    }
    c.Store32(base_ptr, base);
    c.Store32(size_ptr, kPageSize);
    CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, kMemDecommit, 0}) == 0);
    c.Store32(base_ptr, base);
    c.Store32(size_ptr, 0);
    CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, kMemRelease, 0}) == 0);
  });
}

REX_TEST_CASE("memory: allocation and protection accept only the kernel protection set",
              "[kernel][memory]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    for (const u32 protect :
         {kPageNoAccess, kPageReadOnly, kPageReadWrite, 0x20u, 0x40u, 0x104u, 0x1002u, 0x2004u}) {
      INFO(protect);
      c.Store32(base_ptr, 0);
      c.Store32(size_ptr, kPageSize);
      CHECKED_ELSE(c.Kernel("NtAllocateVirtualMemory",
                            {base_ptr, size_ptr, kMemCommit | kMemReserve, protect, 0}) == 0) {
        FAIL_CHECK("the check above failed");
        continue;
      }
      CHECK(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, protect, old_ptr, 0}) == 0);
      c.Store32(size_ptr, 0);
      CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, kMemRelease, 0}) == 0);
    }
    c.Store32(base_ptr, 0);
    c.Store32(size_ptr, kPageSize);
    REQUIRE(c.Kernel("NtAllocateVirtualMemory",
                     {base_ptr, size_ptr, kMemCommit | kMemReserve, kPageReadWrite, 0}) == 0);
    const u32 base = c.Load32(base_ptr);
    for (const u32 protect :
         {0u, 0x08u, 0x10u, 0x80u, 0x200u, 0x400u, 0x206u, 0x404u, 0x800u, 0x4000u, 0x03u, 0x24u,
          0x60u, 0x21u, 0x41u, 0x1001u, 0x2001u, 0x101u, 0xFFFF0004u}) {
      INFO(protect);
      c.Store32(base_ptr, 0);
      c.Store32(size_ptr, kPageSize);
      CHECK(c.Kernel("NtAllocateVirtualMemory",
                     {base_ptr, size_ptr, kMemCommit | kMemReserve, protect, 0}) ==
            rex::X_STATUS_INVALID_PAGE_PROTECTION);
      c.Store32(base_ptr, base);
      c.Store32(size_ptr, kPageSize);
      CHECK(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, protect, old_ptr, 0}) ==
            rex::X_STATUS_INVALID_PAGE_PROTECTION);
    }
  });
}
