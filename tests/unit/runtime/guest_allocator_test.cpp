/**
 * @file        tests/unit/runtime/guest_allocator_test.cpp
 * @brief       The allocator over a range of guest memory
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string_view>
#include <cstring>

#include <rex/runtime.h>
#include <rex/runtime/guest_allocator.h>
#include <rex/runtime/xmemory.h>

#include "../kernel/kernel_fixture.h"

using rex::runtime::GuestAllocator;

namespace {

constexpr u32 kRangeSize = 64 * 1024;
constexpr u32 kAlignment = 4096;
constexpr u32 kPageSize = rex::runtime::kPageSize4KiB;
constexpr u32 kPhysicalPageSize = 64 * 1024;
constexpr u32 kPhysicalBase = 0xA0000000;
constexpr u32 kPhysicalEnd = 0xC0000000;

u32 ReserveAndRelease(rex::runtime::Memory& memory, u32 size, bool physical = false,
                      u32 page_size = rex::runtime::kPageSize4KiB) {
  rex::runtime::BaseHeap* heap = memory.LookupHeapByType(physical, page_size);
  u32 base = 0;
  REQUIRE(heap->Alloc(
      size, kAlignment,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, true, &base));
  heap->Release(base);
  return base;
}

}  // namespace

REX_TEST_CASE("A failed o1heapInit releases its range", "[runtime][allocator]") {
  rex::testing::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  const u32 expected = ReserveAndRelease(memory, 16);
  auto failed = GuestAllocator::Create(memory, 16, kAlignment, false, kPageSize, 0);
  CHECK_FALSE(failed);
  CHECK(ReserveAndRelease(memory, 16) == expected);
}

REX_TEST_CASE("A destroyed allocator releases its range", "[runtime][allocator]") {
  rex::testing::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  const u32 expected = ReserveAndRelease(memory, kRangeSize);
  {
    auto allocator = GuestAllocator::Create(memory, kRangeSize, kAlignment, false, kPageSize, 0);
    REQUIRE(allocator);
    CHECK((*allocator)->GetBase() == expected);
    CHECK((*allocator)->GetSize() == kRangeSize);
  }
  CHECK(ReserveAndRelease(memory, kRangeSize) == expected);
}

REX_TEST_CASE("Blocks lie inside the range and Free returns them", "[runtime][allocator]") {
  rex::testing::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  auto allocator = GuestAllocator::Create(memory, kRangeSize, kAlignment, false, kPageSize, 0);
  REQUIRE(allocator);
  void* block = (*allocator)->Allocate(100);
  REQUIRE(block != nullptr);
  CHECK((*allocator)->Contains(memory.HostToGuestVirtual(block)));
  CHECK_FALSE((*allocator)->Contains((*allocator)->GetBase() + kRangeSize));
  CHECK_FALSE((*allocator)->Contains((*allocator)->GetBase() - 1));
  CHECK((*allocator)->GetDiagnostics().allocated > 0);
  (*allocator)->Free(block);
  CHECK((*allocator)->GetDiagnostics().allocated == 0);
  CHECK((*allocator)->InvariantsHold());
}

REX_TEST_CASE("Reallocate keeps the contents and an exhausted range returns null",
              "[runtime][allocator]") {
  rex::testing::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  auto allocator = GuestAllocator::Create(memory, kRangeSize, kAlignment, false, kPageSize, 0);
  REQUIRE(allocator);
  auto* block = static_cast<u8*>((*allocator)->Allocate(64));
  REQUIRE(block != nullptr);
  std::memset(block, 0x5A, 64);
  auto* grown = static_cast<u8*>((*allocator)->Reallocate(block, 256));
  REQUIRE(grown != nullptr);
  CHECK(grown[0] == 0x5A);
  CHECK(grown[63] == 0x5A);
  CHECK((*allocator)->Allocate(kRangeSize) == nullptr);
  CHECK((*allocator)->GetDiagnostics().oom_count == 1);
}

REX_TEST_CASE("A physical 64K allocator falls in the physical range and releases on destroy",
              "[runtime][allocator]") {
  rex::testing::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  const u32 expected = ReserveAndRelease(memory, kRangeSize, true, kPhysicalPageSize);
  CHECK(expected >= kPhysicalBase);
  CHECK(expected < kPhysicalEnd);
  {
    auto allocator =
        GuestAllocator::Create(memory, kRangeSize, kPhysicalPageSize, true, kPhysicalPageSize, 0);
    REQUIRE(allocator);
    CHECK((*allocator)->GetBase() == expected);
    void* block = (*allocator)->Allocate(100);
    REQUIRE(block != nullptr);
    CHECK((*allocator)->Contains(memory.HostToGuestVirtual(block)));
    (*allocator)->Free(block);
  }
  CHECK(ReserveAndRelease(memory, kRangeSize, true, kPhysicalPageSize) == expected);
}

REX_TEST_CASE("Committed pages follow commit, decommit and release", "[runtime][allocator]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  rex::runtime::BaseHeap* heap = memory.LookupHeapByType(false, kPageSize);
  const u32 before = heap->GetCommittedPages();
  u32 base = 0;
  REQUIRE(heap->Alloc(kRangeSize, kAlignment, rex::runtime::kMemoryAllocationReserve,
                      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, true,
                      &base));
  CHECK(heap->GetCommittedPages() == before);
  REQUIRE(heap->AllocFixed(base, kRangeSize, kAlignment, rex::runtime::kMemoryAllocationCommit,
                           rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite));
  CHECK(heap->GetCommittedPages() == before + kRangeSize / kPageSize);
  REQUIRE(heap->AllocFixed(base, kRangeSize, kAlignment, rex::runtime::kMemoryAllocationCommit,
                           rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite));
  CHECK(heap->GetCommittedPages() == before + kRangeSize / kPageSize);
  REQUIRE(heap->Decommit(base, kRangeSize / 2));
  CHECK(heap->GetCommittedPages() == before + kRangeSize / kPageSize / 2);
  REQUIRE(heap->Release(base));
  CHECK(heap->GetCommittedPages() == before);
}

REX_TEST_CASE("A physical heap view counts the committed pages of its parent",
              "[runtime][allocator]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  rex::runtime::BaseHeap* heap = memory.LookupHeapByType(true, kPhysicalPageSize);
  rex::runtime::BaseHeap* parent = memory.GetPhysicalHeap();
  const u32 before = parent->GetCommittedPages();
  u32 base = 0;
  REQUIRE(heap->Alloc(
      kRangeSize, kAlignment,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, true, &base));
  CHECK(parent->GetCommittedPages() == before + kRangeSize / parent->GetPageSize());
  REQUIRE(heap->Release(base));
  CHECK(parent->GetCommittedPages() == before);
}

REX_TEST_CASE("Plotting the memory profile works without a profiler", "[runtime][allocator]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  auto allocator = GuestAllocator::Create(memory, kRangeSize, kAlignment, false, kPageSize, 0,
                                          GuestPool::kWorkspace);
  REQUIRE(allocator);
  void* block = (*allocator)->Allocate(64);
  REQUIRE(block);
  memory.PlotProfile();
  (*allocator)->Free(block);
  CHECK((*allocator)->GetPool() == GuestPool::kWorkspace);
}

REX_TEST_CASE("A title allocator reports its blocks to the title pool only",
              "[runtime][allocator]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  auto title = GuestAllocator::Create(memory, kRangeSize, kAlignment, false, kPageSize, 0,
                                      GuestPool::kTitle);
  auto heap =
      GuestAllocator::Create(memory, kRangeSize, kAlignment, false, kPageSize, 0, GuestPool::kHeap);
  REQUIRE(title);
  REQUIRE(heap);
  CHECK((*title)->GetPool() == GuestPool::kTitle);
  CHECK((*heap)->GetPool() == GuestPool::kHeap);
  CHECK(std::string_view(GuestAllocator::GetPoolPlotName(GuestPool::kTitle)) == "Title");
  void* block = (*title)->Allocate(64);
  REQUIRE(block);
  CHECK((*title)->GetDiagnostics().allocated > 0);
  CHECK((*heap)->GetDiagnostics().allocated == 0);
  memory.PlotProfile();
  (*title)->Free(block);
}
