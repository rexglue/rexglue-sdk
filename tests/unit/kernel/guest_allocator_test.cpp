/**
 * @file        tests/unit/kernel/guest_allocator_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>

#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/guest_allocator.h>

#include "../kernel/kernel_fixture.h"
#include "kernel/title_state.h"

using rex::kernel::GuestAllocator;
using rex::kernel::GuestPool;

namespace {

constexpr u32 kRangeSize = 64 * 1024;
constexpr u32 kAlignment = 4096;
constexpr u32 kPageSize = rex::kernel::kPageSize4KiB;
constexpr u32 kPhysicalPageSize = 64 * 1024;
constexpr u32 kPhysicalBase = 0xA0000000;
constexpr u32 kPhysicalEnd = 0xC0000000;

u32 ReserveAndRelease(rex::kernel::GuestAddressSpace& memory, u32 size, bool physical = false,
                      u32 page_size = rex::kernel::kPageSize4KiB) {
  rex::kernel::BaseHeap* heap = memory.LookupHeapByType(physical, page_size);
  u32 base = 0;
  REQUIRE(heap->Alloc(size, kAlignment,
                      rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, true,
                      &base));
  heap->Release(base);
  return base;
}

}  // namespace

REX_TEST_CASE("A failed o1heapInit releases its range", "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  rex::kernel::GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  const u32 expected = ReserveAndRelease(memory, 16);
  auto failed = GuestAllocator::Create(fixture.GetRuntime().GetGuestMemory(), 16, kAlignment, false,
                                       kPageSize, 0, GuestPool::kHeap);
  CHECK_FALSE(failed);
  CHECK(ReserveAndRelease(memory, 16) == expected);
}

REX_TEST_CASE("A destroyed allocator releases its range", "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  rex::kernel::GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  const u32 expected = ReserveAndRelease(memory, kRangeSize);
  {
    auto allocator = GuestAllocator::Create(fixture.GetRuntime().GetGuestMemory(), kRangeSize,
                                            kAlignment, false, kPageSize, 0, GuestPool::kHeap);
    REQUIRE(allocator);
    CHECK((*allocator)->GetBase() == expected);
    CHECK((*allocator)->GetSize() == kRangeSize);
  }
  CHECK(ReserveAndRelease(memory, kRangeSize) == expected);
}

REX_TEST_CASE("Blocks reside inside the range and Free returns them", "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  rex::kernel::GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  auto allocator = GuestAllocator::Create(fixture.GetRuntime().GetGuestMemory(), kRangeSize,
                                          kAlignment, false, kPageSize, 0, GuestPool::kHeap);
  REQUIRE(allocator);
  void* block = (*allocator)->Allocate(100);
  REQUIRE(block != nullptr);
  CHECK((*allocator)->Contains(memory.HostToGuestVirtual(block)));
  CHECK_FALSE((*allocator)->Contains((*allocator)->GetBase() + kRangeSize));
  CHECK_FALSE((*allocator)->Contains((*allocator)->GetBase() - 1));
  CHECK((*allocator)->GetStats().allocated > 0);
  (*allocator)->Free(block);
  CHECK((*allocator)->GetStats().allocated == 0);
  CHECK((*allocator)->IsConsistent());
}

REX_TEST_CASE("Reallocate keeps the contents and an exhausted range returns null",
              "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  auto allocator = GuestAllocator::Create(fixture.GetRuntime().GetGuestMemory(), kRangeSize,
                                          kAlignment, false, kPageSize, 0, GuestPool::kHeap);
  REQUIRE(allocator);
  auto* block = static_cast<u8*>((*allocator)->Allocate(64));
  REQUIRE(block != nullptr);
  std::memset(block, 0x5A, 64);
  auto* grown = static_cast<u8*>((*allocator)->Reallocate(block, 256));
  REQUIRE(grown != nullptr);
  CHECK(grown[0] == 0x5A);
  CHECK(grown[63] == 0x5A);
  CHECK((*allocator)->Allocate(kRangeSize) == nullptr);
  CHECK((*allocator)->GetStats().oom_count == 1);
}

REX_TEST_CASE("A physical 64K allocator falls in the physical range and releases on destroy",
              "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  rex::kernel::GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  const u32 expected = ReserveAndRelease(memory, kRangeSize, true, kPhysicalPageSize);
  CHECK(expected >= kPhysicalBase);
  CHECK(expected < kPhysicalEnd);
  {
    auto allocator =
        GuestAllocator::Create(fixture.GetRuntime().GetGuestMemory(), kRangeSize, kPhysicalPageSize,
                               true, kPhysicalPageSize, 0, GuestPool::kHeap);
    REQUIRE(allocator);
    CHECK((*allocator)->GetBase() == expected);
    void* block = (*allocator)->Allocate(100);
    REQUIRE(block != nullptr);
    const u32 guest_block = memory.HostToGuestVirtual(block);
    CHECK((*allocator)->Contains(guest_block));
    CHECK(guest_block >= kPhysicalBase);
    CHECK(guest_block < kPhysicalEnd);
    (*allocator)->Free(block);
  }
  CHECK(ReserveAndRelease(memory, kRangeSize, true, kPhysicalPageSize) == expected);
}

REX_TEST_CASE("Committed pages follow commit, decommit and release", "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  rex::kernel::GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  rex::kernel::BaseHeap* heap = memory.LookupHeapByType(false, kPageSize);
  const u32 before = heap->GetCommittedPages();
  u32 base = 0;
  REQUIRE(heap->Alloc(kRangeSize, kAlignment, rex::kernel::kMemoryAllocationReserve,
                      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, true,
                      &base));
  CHECK(heap->GetCommittedPages() == before);
  REQUIRE(heap->AllocFixed(base, kRangeSize, kAlignment, rex::kernel::kMemoryAllocationCommit,
                           rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite));
  CHECK(heap->GetCommittedPages() == before + kRangeSize / kPageSize);
  REQUIRE(heap->AllocFixed(base, kRangeSize, kAlignment, rex::kernel::kMemoryAllocationCommit,
                           rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite));
  CHECK(heap->GetCommittedPages() == before + kRangeSize / kPageSize);
  REQUIRE(heap->Decommit(base, kRangeSize / 2));
  CHECK(heap->GetCommittedPages() == before + kRangeSize / kPageSize / 2);
  REQUIRE(heap->Release(base));
  CHECK(heap->GetCommittedPages() == before);
}

REX_TEST_CASE("A physical heap view counts the committed pages of its parent",
              "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  rex::kernel::GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  rex::kernel::BaseHeap* heap = memory.LookupHeapByType(true, kPhysicalPageSize);
  rex::kernel::BaseHeap* parent = memory.GetPhysicalHeap();
  const u32 before = parent->GetCommittedPages();
  u32 base = 0;
  REQUIRE(heap->Alloc(kRangeSize, kAlignment,
                      rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, true,
                      &base));
  CHECK(parent->GetCommittedPages() == before + kRangeSize / parent->GetPageSize());
  REQUIRE(heap->Release(base));
  CHECK(parent->GetCommittedPages() == before);
}

REX_TEST_CASE("Plotting the memory profile works without a profiler", "[kernel][allocator]") {
  rex::test::KernelFixture fixture;
  rex::kernel::GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  auto allocator = GuestAllocator::Create(fixture.GetRuntime().GetGuestMemory(), kRangeSize,
                                          kAlignment, false, kPageSize, 0, GuestPool::kWorkspace);
  REQUIRE(allocator);
  void* block = (*allocator)->Allocate(64);
  REQUIRE(block);
  memory.PlotProfile();
  (*allocator)->Free(block);
}
