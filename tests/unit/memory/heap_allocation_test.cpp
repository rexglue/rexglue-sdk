/**
 * @file        tests/unit/memory/heap_allocation_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime/xmemory.h>

#include "test_memory.h"

namespace {

using rex::testing::GetTestMemory;

/**
 * Casts away const so a test can mutate a heap.
 *
 * @param heap The heap.
 * @return The same heap without const.
 */
rex::runtime::BaseHeap* MutableHeap(const rex::runtime::BaseHeap* heap) {
  return const_cast<rex::runtime::BaseHeap*>(heap);
}

}  // namespace

REX_TEST_CASE("Heap allocation rounds size up to page size", "[memory][heap]") {
  auto& memory = GetTestMemory();
  // 0x10000000 is served by the v00000000 heap with 4KB pages.
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  SECTION("Size smaller than page size rounds to page size") {
    u32 addr = 0;
    bool result = heap->Alloc(
        100, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
        rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);
    REQUIRE(addr != 0);

    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Size of 4 bytes rounds to 4096") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
        rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Size exactly page size stays unchanged") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
        rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Multi-page allocation") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096 * 4, 4096,
        rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
        rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096 * 4);

    heap->Release(addr, nullptr);
  }
}

REX_TEST_CASE("Heap allocation rounds alignment up to page size", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  SECTION("Small alignment rounds to page size") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 32, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
        rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    CHECK((addr % 4096) == 0);

    heap->Release(addr, nullptr);
  }

  SECTION("Large alignment is respected") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 65536, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
        rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

REX_TEST_CASE("Bottom-up allocation starts after reserved first 64KB", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // First 64KB (0x10000) is reserved, allocations start at 0x10000 or later
  CHECK(addr >= 0x10000);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Top-down allocation returns high addresses", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr_bottom = 0;
  u32 addr_top = 0;

  heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr_bottom);

  heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, true, &addr_top);

  CHECK(addr_top > addr_bottom);
  // The heap ends near 0x40000000.
  CHECK(addr_top > 0x1F000000);

  heap->Release(addr_bottom, nullptr);
  heap->Release(addr_top, nullptr);
}

REX_TEST_CASE("AllocFixed allocates at exact address", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x20000000;

  bool result = heap->AllocFixed(
      target, 4096, 4096,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  REQUIRE(result);

  rex::runtime::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(target, &info));
  CHECK(info.base_address == target);
  CHECK(info.state != 0);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("AllocFixed reserve-only fails on already-reserved region", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x21000000;

  bool first =
      heap->AllocFixed(target, 4096, 4096, rex::runtime::kMemoryAllocationReserve,
                       rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  REQUIRE(first);

  bool second =
      heap->AllocFixed(target, 4096, 4096, rex::runtime::kMemoryAllocationReserve,
                       rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  CHECK_FALSE(second);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("AllocFixed commit on reserved region succeeds", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x21100000;

  bool reserve =
      heap->AllocFixed(target, 4096, 4096, rex::runtime::kMemoryAllocationReserve,
                       rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  REQUIRE(reserve);

  bool commit =
      heap->AllocFixed(target, 4096, 4096, rex::runtime::kMemoryAllocationCommit,
                       rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  CHECK(commit);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("Protect changes page protection and returns old value", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  SECTION("Change to read-only") {
    u32 old_protect = 0;
    bool protect_result = heap->Protect(addr, 4096, rex::runtime::kMemoryProtectRead, &old_protect);
    REQUIRE(protect_result);

    CHECK(old_protect == (rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite));

    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.protect == rex::runtime::kMemoryProtectRead);
  }

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("QueryRegionInfo returns correct allocation info", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096 * 4, 4096,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  SECTION("Query at allocation base") {
    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));

    CHECK(info.base_address == addr);
    CHECK(info.allocation_size == 4096 * 4);
    CHECK(info.region_size == 4096 * 4);
    CHECK(info.protect == (rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite));
    CHECK(info.state != 0);
  }

  SECTION("Query in middle of allocation") {
    u32 mid = addr + 4096 * 2;
    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(mid, &info));

    CHECK(info.base_address == mid);
    CHECK(info.allocation_size == 4096 * 4);
    CHECK(info.region_size == 4096 * 2);
  }

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Release frees memory for reallocation", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x22000000));
  REQUIRE(heap != nullptr);

  u32 target = 0x22000000;

  bool alloc1 = heap->AllocFixed(
      target, 4096, 4096,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  REQUIRE(alloc1);

  u32 released_size = 0;
  bool release = heap->Release(target, &released_size);
  REQUIRE(release);
  CHECK(released_size == 4096);

  bool alloc2 = heap->AllocFixed(
      target, 4096, 4096,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  CHECK(alloc2);

  heap->Release(target, nullptr);
}

REX_TEST_CASE("LookupHeap returns correct heap for address", "[memory][heap]") {
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

  SECTION("Address in stack range returns nullptr") {
    auto* heap = memory.LookupHeap(0x7F000000);
    CHECK(heap == nullptr);
  }
}

REX_TEST_CASE("64KB page heap rounds to 64KB boundaries", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  SECTION("Small size rounds to 64KB") {
    u32 addr = 0;
    bool result = heap->Alloc(
        4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
        rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    // Sizes round up to the 64KB page size.
    CHECK(info.allocation_size == 65536);

    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

REX_TEST_CASE("Decommit removes commit flag but keeps reservation", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  rex::runtime::HeapAllocationInfo info_before{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_before));
  CHECK((info_before.state & rex::runtime::kMemoryAllocationCommit) != 0);
  CHECK((info_before.state & rex::runtime::kMemoryAllocationReserve) != 0);

  bool decommit = heap->Decommit(addr, 4096);
  REQUIRE(decommit);

  rex::runtime::HeapAllocationInfo info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::runtime::kMemoryAllocationCommit) == 0);
  CHECK((info_after.state & rex::runtime::kMemoryAllocationReserve) != 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Decommit partial region", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      4096 * 4, 4096,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  bool decommit = heap->Decommit(addr + 4096, 4096 * 2);
  REQUIRE(decommit);

  rex::runtime::HeapAllocationInfo info_first{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_first));
  CHECK((info_first.state & rex::runtime::kMemoryAllocationCommit) != 0);

  rex::runtime::HeapAllocationInfo info_mid{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096, &info_mid));
  CHECK((info_mid.state & rex::runtime::kMemoryAllocationCommit) == 0);

  rex::runtime::HeapAllocationInfo info_last{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096 * 3, &info_last));
  CHECK((info_last.state & rex::runtime::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Decommit-recommit cycle on 64KB heap (real usage pattern)", "[memory][heap]") {
  auto& memory = GetTestMemory();
  // 0x50000000 is served by the v40000000 heap with 64KB pages.
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit with type 0x4000 as NtFreeVirtualMemory does.
  bool decommit = heap->Decommit(addr, 65536);
  REQUIRE(decommit);

  rex::runtime::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  CHECK(info.state == rex::runtime::kMemoryAllocationReserve);

  // Recommit with type 0x60001000 as NtAllocateVirtualMemory does.
  bool recommit =
      heap->AllocFixed(addr, 65536, 65536, rex::runtime::kMemoryAllocationCommit,
                       rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite);
  REQUIRE(recommit);

  rex::runtime::HeapAllocationInfo info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::runtime::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Repeated decommit of same page succeeds (idempotent)", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  bool decommit1 = heap->Decommit(addr, 65536);
  REQUIRE(decommit1);

  bool decommit2 = heap->Decommit(addr, 65536);
  CHECK(decommit2);

  bool decommit3 = heap->Decommit(addr, 65536);
  CHECK(decommit3);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Decommit on 64KB heap uses 64KB granularity", "[memory][heap]") {
  // On the v40000000 heap a small decommit covers the whole 64KB page.
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  u32 addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  bool decommit = heap->Decommit(addr, 4096);
  REQUIRE(decommit);

  // Decommit size is rounded to the page size through get_page_count.
  rex::runtime::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  CHECK((info.state & rex::runtime::kMemoryAllocationCommit) == 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("TranslateVirtual returns valid host pointer", "[memory][translation]") {
  auto& memory = GetTestMemory();

  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 guest_addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &guest_addr);
  REQUIRE(result);

  u8* host_ptr = memory.TranslateVirtual(guest_addr);
  REQUIRE(host_ptr != nullptr);

  host_ptr[0] = 0xAB;
  CHECK(host_ptr[0] == 0xAB);

  heap->Release(guest_addr, nullptr);
}

REX_TEST_CASE("HostToGuestVirtual roundtrip", "[memory][translation]") {
  auto& memory = GetTestMemory();

  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  u32 original_guest = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &original_guest);
  REQUIRE(result);

  u8* host_ptr = memory.TranslateVirtual(original_guest);
  u32 back_to_guest = memory.HostToGuestVirtual(host_ptr);

  CHECK(back_to_guest == original_guest);

  heap->Release(original_guest, nullptr);
}

REX_TEST_CASE("TranslatePhysical masks to 29 bits", "[memory][translation]") {
  auto& memory = GetTestMemory();

  // Physical addresses are masked with 0x1FFFFFFF (29 bits)
  u8* ptr_low = memory.TranslatePhysical(0x00001000);
  u8* ptr_high = memory.TranslatePhysical(0xA0001000);

  // (0xA0001000 & 0x1FFFFFFF) == 0x00001000
  CHECK(ptr_low == ptr_high);
}

REX_TEST_CASE("Physical heap vA0000000 (64KB pages, cached)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xA0000000));
  REQUIRE(heap != nullptr);

  // vA0000000 heap has base 0xA0000000 and 64KB pages
  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  CHECK(addr >= 0xA0000000);
  CHECK(addr < 0xC0000000);

  CHECK((addr % 65536) == 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Physical heap vC0000000 (16MB pages, uncached)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xC0000000));
  REQUIRE(heap != nullptr);

  // vC0000000 has 16MB pages, and its first 16MB is preallocated for GPU writeback.

  u32 addr = 0;
  bool result = heap->Alloc(
      16 * 1024 * 1024, 16 * 1024 * 1024,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  CHECK(addr >= 0xC0000000);
  CHECK(addr < 0xE0000000);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("Physical heap vE0000000 (4KB pages, write-combine)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xE0000000));
  REQUIRE(heap != nullptr);

  // vE0000000 heap has base 0xE0000000 and 4KB pages
  u32 addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  CHECK(addr >= 0xE0000000);
  CHECK(addr < 0xFFD00000);

  CHECK((addr % 4096) == 0);

  heap->Release(addr, nullptr);
}

REX_TEST_CASE("LookupHeapByType selects correct heap", "[memory][heap]") {
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
