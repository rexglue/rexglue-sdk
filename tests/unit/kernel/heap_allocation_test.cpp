/**
 * @file        tests/unit/kernel/heap_allocation_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <thread>

#include <rex/logging/log.h>
#include <rex/kernel/heap.h>

#include "test_memory.h"

namespace {

using rex::test::GetTestMemory;

rex::kernel::BaseHeap* MutableHeap(const rex::kernel::BaseHeap* heap) {
  return const_cast<rex::kernel::BaseHeap*>(heap);
}

}  // namespace

REX_TEST_CASE("Heap allocation rounds size up to page size", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  // 0x10000000 is served by the v00000000 heap with 4KB pages.
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  SECTION("Size smaller than page size rounds to page size") {
    u32 addr = 0;
    bool result = heap->Alloc(
        100, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
        rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);
    REQUIRE(addr != 0);

    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Size of 4 bytes rounds to 4096") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
        rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Size exactly page size stays unchanged") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
        rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Allocation of several pages") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096 * 4, 4096,
        rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
        rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096 * 4);

    heap->Release(addr, nullptr);
  }
}

REX_TEST_CASE("Heap allocation rounds alignment up to page size", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  SECTION("Small alignment rounds to page size") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 32, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
        rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    CHECK((addr % 4096) == 0);

    heap->Release(addr, nullptr);
  }

  SECTION("Large alignment is respected") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 65536, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
        rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

REX_TEST_CASE("Bottom-up allocation starts after reserved first 64KB", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // The first 64KB (0x10000) is reserved, so allocations start at 0x10000 or later.
  CHECK(addr >= 0x10000);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Top-down allocation returns high addresses", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr_bottom = 0;
  u32 addr_top = 0;

  heap->Alloc(
      4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr_bottom);

  heap->Alloc(4096, 4096,
              rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
              rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, true, &addr_top);

  CHECK(addr_top > addr_bottom);
  // The heap ends near 0x40000000.
  CHECK(addr_top > 0x1F000000);

  heap->Release(addr_bottom, nullptr);
  heap->Release(addr_top, nullptr);
}

REX_TEST_CASE("AllocFixed allocates at exact address", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x20000000;

  bool result =
      heap->AllocFixed(target, 4096, 4096,
                       rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                       rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  REQUIRE(result);

  rex::kernel::HeapAllocation info{};
  REQUIRE(heap->QueryRegionInfo(target, &info));
  CHECK(info.base_address == target);
  CHECK(info.state != 0);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("AllocFixed reserve only fails on a reserved region", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x21000000;

  bool first = heap->AllocFixed(target, 4096, 4096, rex::kernel::kMemoryAllocationReserve,
                                rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  REQUIRE(first);

  bool second =
      heap->AllocFixed(target, 4096, 4096, rex::kernel::kMemoryAllocationReserve,
                       rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  CHECK_FALSE(second);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("AllocFixed commit on reserved region succeeds", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x21100000;

  bool reserve =
      heap->AllocFixed(target, 4096, 4096, rex::kernel::kMemoryAllocationReserve,
                       rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  REQUIRE(reserve);

  bool commit =
      heap->AllocFixed(target, 4096, 4096, rex::kernel::kMemoryAllocationCommit,
                       rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  CHECK(commit);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("Protect changes page protection and returns old value", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  SECTION("Change to read-only") {
    u32 old_protect = 0;
    bool protect_result = heap->Protect(addr, 4096, rex::kernel::kMemoryProtectRead, &old_protect);
    REQUIRE(protect_result);

    CHECK(old_protect == (rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite));

    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.protect == rex::kernel::kMemoryProtectRead);
  }

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("QueryRegionInfo returns correct allocation info", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096 * 4, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  SECTION("Query at allocation base") {
    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));

    CHECK(info.base_address == addr);
    CHECK(info.allocation_size == 4096 * 4);
    CHECK(info.region_size == 4096 * 4);
    CHECK(info.protect == (rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite));
    CHECK(info.state != 0);
  }

  SECTION("Query in middle of allocation") {
    u32 mid = addr + 4096 * 2;
    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(mid, &info));

    CHECK(info.base_address == mid);
    CHECK(info.allocation_size == 4096 * 4);
    CHECK(info.region_size == 4096 * 2);
  }

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Release frees memory for reallocation", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x22000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x22000000;

  bool alloc1 =
      heap->AllocFixed(target, 4096, 4096,
                       rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                       rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  REQUIRE(alloc1);

  u32 released_size = 0;
  bool release = heap->Release(target, &released_size);
  REQUIRE(release);
  CHECK(released_size == 4096);

  bool alloc2 =
      heap->AllocFixed(target, 4096, 4096,
                       rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                       rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  CHECK(alloc2);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("LookupHeap returns correct heap for address", "[kernel][heap]") {
  auto& memory = GetTestMemory();

  SECTION("Address in v00000000 range") {
    auto* heap = memory.LookupHeap(0x10000000);
    REQUIRE(heap != nullptr);
  }

  SECTION("Address in v40000000 range") {
    auto* heap = memory.LookupHeap(0x50000000);
    REQUIRE(heap != nullptr);
  }

  SECTION("Address in v80000000 range (XEX)") {
    auto* heap = memory.LookupHeap(0x82000000);
    REQUIRE(heap != nullptr);
  }

  SECTION("Address in the 0x7F000000 register range returns null") {
    auto* heap = memory.LookupHeap(0x7F000000);
    CHECK(heap == nullptr);
  }
}

REX_TEST_CASE("64KB page heap rounds to 64KB boundaries", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  SECTION("Small size rounds to 64KB") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
        rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::kernel::HeapAllocation info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 65536);

    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

REX_TEST_CASE("Decommit removes commit flag but keeps reservation", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  rex::kernel::HeapAllocation info_before{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_before));
  CHECK((info_before.state & rex::kernel::kMemoryAllocationCommit) != 0);
  CHECK((info_before.state & rex::kernel::kMemoryAllocationReserve) != 0);

  bool decommit = heap->Decommit(addr, 4096);
  REQUIRE(decommit);

  rex::kernel::HeapAllocation info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::kernel::kMemoryAllocationCommit) == 0);
  CHECK((info_after.state & rex::kernel::kMemoryAllocationReserve) != 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Decommit partial region", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096 * 4, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  bool decommit = heap->Decommit(addr + 4096, 4096 * 2);
  REQUIRE(decommit);

  rex::kernel::HeapAllocation info_first{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_first));
  CHECK((info_first.state & rex::kernel::kMemoryAllocationCommit) != 0);

  rex::kernel::HeapAllocation info_mid{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096, &info_mid));
  CHECK((info_mid.state & rex::kernel::kMemoryAllocationCommit) == 0);

  rex::kernel::HeapAllocation info_last{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096 * 3, &info_last));
  CHECK((info_last.state & rex::kernel::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Decommit and recommit cycle on 64KB heap (real usage pattern)", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  // 0x50000000 is served by the v40000000 heap with 64KB pages.
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit with type 0x4000 as NtFreeVirtualMemory does.
  bool decommit = heap->Decommit(addr, 65536);
  REQUIRE(decommit);

  rex::kernel::HeapAllocation info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  CHECK(info.state == rex::kernel::kMemoryAllocationReserve);

  // Recommit with type 0x60001000 as NtAllocateVirtualMemory does.
  bool recommit =
      heap->AllocFixed(addr, 65536, 65536, rex::kernel::kMemoryAllocationCommit,
                       rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite);
  REQUIRE(recommit);

  rex::kernel::HeapAllocation info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::kernel::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Repeated decommit of same page succeeds (idempotent)", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  bool decommit1 = heap->Decommit(addr, 65536);
  REQUIRE(decommit1);

  bool decommit2 = heap->Decommit(addr, 65536);
  CHECK(decommit2);

  bool decommit3 = heap->Decommit(addr, 65536);
  CHECK(decommit3);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Decommit on 64KB heap uses 64KB granularity", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  bool decommit = heap->Decommit(addr, 4096);
  REQUIRE(decommit);

  rex::kernel::HeapAllocation info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  CHECK((info.state & rex::kernel::kMemoryAllocationCommit) == 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("TranslatePhysical masks to 29 bits", "[kernel][translation]") {
  auto& memory = GetTestMemory();

  // Physical addresses are masked with 0x1FFFFFFF, which is 29 bits.
  u8* ptr_low = memory.TranslatePhysical(0x00001000);
  u8* ptr_high = memory.TranslatePhysical(0xA0001000);

  CHECK(ptr_low == ptr_high);
}

REX_TEST_CASE("Physical heap vA0000000 (64KB pages, cached)", "[kernel][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xA0000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  CHECK(addr >= 0xA0000000);
  CHECK(addr < 0xC0000000);

  CHECK((addr % 65536) == 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Physical heap vC0000000 (16MB pages, uncached)", "[kernel][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xC0000000));
  REQUIRE(heap != nullptr);

  // The first 16MB is preallocated for GPU writeback.

  u32 addr = 0;
  bool result =
      heap->Alloc(16 * 1024 * 1024, 16 * 1024 * 1024,
                  rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                  rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  CHECK(addr >= 0xC0000000);
  CHECK(addr < 0xE0000000);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Physical heap vE0000000 (4KB pages, write-combine)", "[kernel][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xE0000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  CHECK(addr >= 0xE0000000);
  CHECK(addr < 0xFFD00000);

  CHECK((addr % 4096) == 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("LookupHeapByType selects correct heap", "[kernel][heap]") {
  auto& memory = GetTestMemory();

  SECTION("Virtual heap with 4KB pages") {
    auto* heap = memory.LookupHeapByType(false, 4096);
    REQUIRE(heap != nullptr);
  }

  SECTION("Virtual heap with 64KB pages") {
    auto* heap = memory.LookupHeapByType(false, 65536);
    REQUIRE(heap != nullptr);
  }

  SECTION("Physical heap with 4KB pages") {
    auto* heap = memory.LookupHeapByType(true, 4096);
    REQUIRE(heap != nullptr);
  }

  SECTION("Physical heap with 64KB pages") {
    auto* heap = memory.LookupHeapByType(true, 65536);
    REQUIRE(heap != nullptr);
  }

  SECTION("Physical heap with 16MB pages") {
    auto* heap = memory.LookupHeapByType(true, 16 * 1024 * 1024);
    REQUIRE(heap != nullptr);
  }
}

REX_TEST_CASE("Heap queries reject the page past the heap end", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);
  const u32 past_end = heap->GetHeapBase() + heap->GetHeapSize();

  u32 size = 0xFFFFFFFF;
  CHECK_FALSE(heap->QuerySize(past_end, &size));
  CHECK(size == 0);

  u32 protect = 0xFFFFFFFF;
  CHECK_FALSE(heap->QueryProtect(past_end, &protect));
  CHECK(protect == 0);

  rex::kernel::HeapAllocation info{};
  CHECK_FALSE(heap->QueryRegionInfo(past_end, &info));
}

REX_TEST_CASE("The title range of a virtual heap ends below its system range", "[kernel][heap]") {
  auto& memory = GetTestMemory();
  constexpr u32 kFlags =
      rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit;
  constexpr u32 kProtect = rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite;
  for (const u32 inside : {0x10000000u, 0x40000000u}) {
    auto* heap = MutableHeap(memory.LookupHeap(inside));
    REQUIRE(heap != nullptr);
    u32 title = 0;
    u32 system = 0;
    REQUIRE(heap->Alloc(0x10000, 0x10000, kFlags, kProtect, true, &title));
    REQUIRE(heap->AllocSystemHeap(0x10000, 0x10000, kFlags, kProtect, false, &system));
    CHECK(system >= title + 0x10000);
    heap->Release(title, nullptr);
    heap->Release(system, nullptr);
  }
}

REX_TEST_CASE("QueryProtect reads a whole entry while Protect changes it", "[kernel][heap]") {
  constexpr u32 kToggles = 100000;
  constexpr u32 kReadOnly = rex::kernel::kMemoryProtectRead;
  constexpr u32 kReadWrite = rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite;

  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  REQUIRE(heap->Alloc(4096, 4096,
                      rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                      kReadWrite, false, &addr));

  std::atomic<bool> done{false};
  std::atomic<u32> unexpected{0};
  std::thread reader([&] {
    while (!done.load(std::memory_order_acquire)) {
      u32 protect = 0;
      if (!heap->QueryProtect(addr, &protect) || (protect != kReadOnly && protect != kReadWrite)) {
        unexpected.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  for (u32 i = 0; i < kToggles; ++i) {
    heap->Protect(addr, 4096, (i & 1) ? kReadWrite : kReadOnly, nullptr);
  }
  done.store(true, std::memory_order_release);
  reader.join();

  CHECK(unexpected.load() == 0);
  heap->Release(addr, nullptr);
}
