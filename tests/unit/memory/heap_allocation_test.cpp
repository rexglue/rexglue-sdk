/**
 * @file        heap_allocation_test.cpp
 * @brief       Unit tests for memory heap allocation behavior
 *
 * These tests validate the BaseHeap allocation, protection, and query
 * operations based on observed runtime behavior.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace {

// Shared memory instance - expensive to create, reuse across tests
rex::memory::Memory& GetTestMemory() {
  static rex::memory::Memory memory;
  static bool initialized = false;
  if (!initialized) {
    rex::InitLogging();
    bool result = memory.Initialize();
    REQUIRE(result);
    initialized = true;
  }
  return memory;
}

// Helper to cast away const for heap operations
rex::memory::BaseHeap* MutableHeap(const rex::memory::BaseHeap* heap) {
  return const_cast<rex::memory::BaseHeap*>(heap);
}

}  // namespace

// =============================================================================
// Size and Alignment Rounding Tests
// =============================================================================

TEST_CASE("Heap allocation rounds size up to page size", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));  // v00000000, 4KB pages
  REQUIRE(heap != nullptr);

  SECTION("Size smaller than page size rounds to page size") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        100,   // Requested size
        4096,  // Alignment
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);
    REQUIRE(addr != 0);

    // Query to verify actual allocated size
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);  // Rounded up from 100

    heap->Release(addr, nullptr);
  }

  SECTION("Size of 4 bytes rounds to 4096") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Size exactly page size stays unchanged") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096);

    heap->Release(addr, nullptr);
  }

  SECTION("Multi-page allocation") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096 * 4, 4096,  // 4 pages
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.allocation_size == 4096 * 4);

    heap->Release(addr, nullptr);
  }
}

TEST_CASE("Heap allocation rounds alignment up to page size", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  SECTION("Small alignment rounds to page size") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 32,  // 32-byte alignment requested
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    // Address should be page-aligned (4096), not just 32-byte aligned
    CHECK((addr % 4096) == 0);

    heap->Release(addr, nullptr);
  }

  SECTION("Large alignment is respected") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 65536,  // 64KB alignment
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

// =============================================================================
// Allocation Direction Tests
// =============================================================================

TEST_CASE("Bottom-up allocation starts after reserved first 64KB", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
      false,  // bottom-up
      &addr);
  REQUIRE(result);

  // First 64KB (0x10000) is reserved, allocations start at 0x10000 or later
  CHECK(addr >= 0x10000);

  heap->Release(addr, nullptr);
}

TEST_CASE("Top-down allocation returns high addresses", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr_bottom = 0;
  uint32_t addr_top = 0;

  // Allocate bottom-up first
  heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr_bottom);

  // Allocate top-down
  heap->Alloc(4096, 4096,
              rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
              true,  // top-down
              &addr_top);

  // Top-down should be significantly higher
  CHECK(addr_top > addr_bottom);
  // Top-down should be near top of heap (heap ends at ~0x40000000)
  CHECK(addr_top > 0x1F000000);

  heap->Release(addr_bottom, nullptr);
  heap->Release(addr_top, nullptr);
}

// =============================================================================
// AllocFixed Tests
// =============================================================================

TEST_CASE("AllocFixed allocates at exact address", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  // Choose an address that should be free
  uint32_t target = 0x20000000;

  bool result =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(result);

  // Verify allocation is at exact address
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(target, &info));
  CHECK(info.base_address == target);
  CHECK(info.state != 0);  // Should be allocated

  heap->Release(target, nullptr);
}

TEST_CASE("AllocFixed reserve-only fails on already-reserved region", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t target = 0x21000000;

  // First allocation: reserve only
  bool first = heap->AllocFixed(target, 4096, 4096,
                                rex::memory::kMemoryAllocationReserve,  // Reserve only
                                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(first);

  // Second reserve at same address should FAIL
  bool second =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve,  // Reserve only - should fail
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  CHECK_FALSE(second);

  heap->Release(target, nullptr);
}

TEST_CASE("AllocFixed commit on reserved region succeeds", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t target = 0x21100000;

  // First: reserve only
  bool reserve =
      heap->AllocFixed(target, 4096, 4096, rex::memory::kMemoryAllocationReserve,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(reserve);

  // Second: commit on already-reserved region should SUCCEED
  bool commit =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationCommit,  // Commit only
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  CHECK(commit);

  heap->Release(target, nullptr);
}

// =============================================================================
// Protection Tests
// =============================================================================

TEST_CASE("Protect changes page protection and returns old value", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,  // Initial: RW
      false, &addr);
  REQUIRE(result);

  SECTION("Change to read-only") {
    uint32_t old_protect = 0;
    bool protect_result = heap->Protect(addr, 4096,
                                        rex::memory::kMemoryProtectRead,  // New: Read only
                                        &old_protect);
    REQUIRE(protect_result);

    // Old protection should have been RW (3)
    CHECK(old_protect == (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));

    // Query to verify new protection
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    CHECK(info.protect == rex::memory::kMemoryProtectRead);
  }

  heap->Release(addr, nullptr);
}

// =============================================================================
// QueryRegionInfo Tests
// =============================================================================

TEST_CASE("QueryRegionInfo returns correct allocation info", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result =
      heap->Alloc(4096 * 4, 4096,  // 4 pages
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  SECTION("Query at allocation base") {
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));

    CHECK(info.base_address == addr);
    CHECK(info.allocation_size == 4096 * 4);
    CHECK(info.region_size == 4096 * 4);
    CHECK(info.protect == (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
    CHECK(info.state != 0);  // Allocated
  }

  SECTION("Query in middle of allocation") {
    uint32_t mid = addr + 4096 * 2;  // Third page
    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(mid, &info));

    // base_address is the queried address
    CHECK(info.base_address == mid);
    // allocation_size should still be full allocation
    CHECK(info.allocation_size == 4096 * 4);
    // region_size is remaining size from query point
    CHECK(info.region_size == 4096 * 2);  // 2 pages left
  }

  heap->Release(addr, nullptr);
}

// =============================================================================
// Release Tests
// =============================================================================

TEST_CASE("Release frees memory for reallocation", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x22000000));  // Use a different region
  REQUIRE(heap != nullptr);

  uint32_t target = 0x22000000;

  // Allocate
  bool alloc1 =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(alloc1);

  // Release
  uint32_t released_size = 0;
  bool release = heap->Release(target, &released_size);
  REQUIRE(release);
  CHECK(released_size == 4096);

  // Should be able to allocate at same address again
  bool alloc2 =
      heap->AllocFixed(target, 4096, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  CHECK(alloc2);

  heap->Release(target, nullptr);
}

// =============================================================================
// Heap Selection Tests (LookupHeap)
// =============================================================================

TEST_CASE("LookupHeap returns correct heap for address", "[memory][heap]") {
  auto& memory = GetTestMemory();

  SECTION("Address in v00000000 range") {
    auto* heap = memory.LookupHeap(0x10000000);
    REQUIRE(heap != nullptr);
    // This is the 4KB page heap
  }

  SECTION("Address in v40000000 range") {
    auto* heap = memory.LookupHeap(0x50000000);
    REQUIRE(heap != nullptr);
    // This is the 64KB page heap
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

// =============================================================================
// 64KB Page Heap Tests (v40000000)
// =============================================================================

TEST_CASE("64KB page heap rounds to 64KB boundaries", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));  // v40000000 heap
  REQUIRE(heap != nullptr);

  SECTION("Small size rounds to 64KB") {
    uint32_t addr = 0;
    bool result = heap->Alloc(
        4096, 4096,  // Request only 4KB
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
    REQUIRE(result);

    rex::memory::HeapAllocationInfo info{};
    REQUIRE(heap->QueryRegionInfo(addr, &info));
    // Size should be rounded to 64KB page size
    CHECK(info.allocation_size == 65536);

    // Address should be 64KB aligned
    CHECK((addr % 65536) == 0);

    heap->Release(addr, nullptr);
  }
}

// =============================================================================
// Decommit Tests
// =============================================================================

TEST_CASE("Decommit removes commit flag but keeps reservation", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Verify initial state is committed
  rex::memory::HeapAllocationInfo info_before{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_before));
  CHECK((info_before.state & rex::memory::kMemoryAllocationCommit) != 0);
  CHECK((info_before.state & rex::memory::kMemoryAllocationReserve) != 0);

  // Decommit
  bool decommit = heap->Decommit(addr, 4096);
  REQUIRE(decommit);

  // Verify commit flag is removed but reserve remains
  rex::memory::HeapAllocationInfo info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::memory::kMemoryAllocationCommit) == 0);
  CHECK((info_after.state & rex::memory::kMemoryAllocationReserve) != 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Decommit partial region", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result =
      heap->Alloc(4096 * 4, 4096,  // 4 pages
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit only middle 2 pages
  bool decommit = heap->Decommit(addr + 4096, 4096 * 2);
  REQUIRE(decommit);

  // First page should still be committed
  rex::memory::HeapAllocationInfo info_first{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_first));
  CHECK((info_first.state & rex::memory::kMemoryAllocationCommit) != 0);

  // Middle page should be decommitted
  rex::memory::HeapAllocationInfo info_mid{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096, &info_mid));
  CHECK((info_mid.state & rex::memory::kMemoryAllocationCommit) == 0);

  // Last page should still be committed
  rex::memory::HeapAllocationInfo info_last{};
  REQUIRE(heap->QueryRegionInfo(addr + 4096 * 3, &info_last));
  CHECK((info_last.state & rex::memory::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Decommit-recommit cycle on 64KB heap (real usage pattern)", "[memory][heap]") {
  // This pattern observed in real app: NtFreeVirtualMemory(MEM_DECOMMIT)
  // followed by NtAllocateVirtualMemory at same address
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));  // v40000000 heap, 64KB pages
  REQUIRE(heap != nullptr);

  // Allocate on 64KB heap
  uint32_t addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit (like NtFreeVirtualMemory with type=0x4000)
  bool decommit = heap->Decommit(addr, 65536);
  REQUIRE(decommit);

  // After decommit, state should be Reserve only (state=1)
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  CHECK(info.state == rex::memory::kMemoryAllocationReserve);  // Exactly 1

  // Recommit at same address (like NtAllocateVirtualMemory with type=0x60001000)
  bool recommit =
      heap->AllocFixed(addr, 65536, 65536,
                       rex::memory::kMemoryAllocationCommit,  // Commit only
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  REQUIRE(recommit);

  // State should now include commit again
  rex::memory::HeapAllocationInfo info_after{};
  REQUIRE(heap->QueryRegionInfo(addr, &info_after));
  CHECK((info_after.state & rex::memory::kMemoryAllocationCommit) != 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Repeated decommit of same page succeeds (idempotent)", "[memory][heap]") {
  // Real app decommits same address multiple times in sequence
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));  // v40000000 heap
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // First decommit
  bool decommit1 = heap->Decommit(addr, 65536);
  REQUIRE(decommit1);

  // Second decommit of same already-decommitted page should still succeed
  bool decommit2 = heap->Decommit(addr, 65536);
  CHECK(decommit2);

  // Third decommit
  bool decommit3 = heap->Decommit(addr, 65536);
  CHECK(decommit3);

  heap->Release(addr, nullptr);
}

TEST_CASE("Decommit on 64KB heap uses 64KB granularity", "[memory][heap]") {
  // On v40000000 heap, even small decommit requests affect whole 64KB page
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x50000000));
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  bool result = heap->Alloc(
      65536, 65536, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Decommit with small size - should still affect the page
  bool decommit = heap->Decommit(addr, 4096);  // Request only 4KB
  REQUIRE(decommit);

  // The 64KB page should still be affected (implementation detail:
  // size gets rounded to page_size internally via get_page_count)
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(addr, &info));
  // State should show decommitted
  CHECK((info.state & rex::memory::kMemoryAllocationCommit) == 0);

  heap->Release(addr, nullptr);
}

// =============================================================================
// Address Translation Tests
// =============================================================================

TEST_CASE("TranslateVirtual returns valid host pointer", "[memory][translation]") {
  auto& memory = GetTestMemory();

  // Allocate some memory first
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t guest_addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &guest_addr);
  REQUIRE(result);

  // Translate to host
  uint8_t* host_ptr = memory.TranslateVirtual(guest_addr);
  REQUIRE(host_ptr != nullptr);

  // Should be able to read/write
  host_ptr[0] = 0xAB;
  CHECK(host_ptr[0] == 0xAB);

  heap->Release(guest_addr, nullptr);
}

TEST_CASE("HostToGuestVirtual roundtrip", "[memory][translation]") {
  auto& memory = GetTestMemory();

  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t original_guest = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &original_guest);
  REQUIRE(result);

  // Guest -> Host -> Guest roundtrip
  uint8_t* host_ptr = memory.TranslateVirtual(original_guest);
  uint32_t back_to_guest = memory.HostToGuestVirtual(host_ptr);

  CHECK(back_to_guest == original_guest);

  heap->Release(original_guest, nullptr);
}

TEST_CASE("TranslatePhysical masks to 29 bits", "[memory][translation]") {
  auto& memory = GetTestMemory();

  // Physical addresses are masked with 0x1FFFFFFF (29 bits)
  // So 0xA0000000 and 0x00000000 should map to same physical offset
  uint8_t* ptr_low = memory.TranslatePhysical(0x00001000);
  uint8_t* ptr_high = memory.TranslatePhysical(0xA0001000);

  // Both should resolve to same offset in physical memory
  // (0xA0001000 & 0x1FFFFFFF) == 0x00001000
  CHECK(ptr_low == ptr_high);
}

// =============================================================================
// Physical Heap Tests
// =============================================================================

TEST_CASE("Physical heap vA0000000 (64KB pages, cached)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xA0000000));
  REQUIRE(heap != nullptr);

  // vA0000000 heap: base=0xA0000000, 64KB pages
  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Address should be in 0xA0000000-0xBFFFFFFF range
  CHECK(addr >= 0xA0000000);
  CHECK(addr < 0xC0000000);

  // Should be 64KB aligned
  CHECK((addr % 65536) == 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("Physical heap vC0000000 (16MB pages, uncached)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xC0000000));
  REQUIRE(heap != nullptr);

  // vC0000000 heap: base=0xC0000000, 16MB pages
  // Note: First 16MB is pre-allocated for GPU writeback

  uint32_t addr = 0;
  bool result =
      heap->Alloc(16 * 1024 * 1024, 16 * 1024 * 1024,  // 16MB
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Address should be in 0xC0000000-0xDFFFFFFF range
  CHECK(addr >= 0xC0000000);
  CHECK(addr < 0xE0000000);

  heap->Release(addr, nullptr);
}

TEST_CASE("Physical heap vE0000000 (4KB pages, write-combine)", "[memory][physical]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0xE0000000));
  REQUIRE(heap != nullptr);

  // vE0000000 heap: base=0xE0000000, 4KB pages
  uint32_t addr = 0;
  bool result = heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr);
  REQUIRE(result);

  // Address should be in 0xE0000000-0xFFCFFFFF range
  CHECK(addr >= 0xE0000000);
  CHECK(addr < 0xFFD00000);

  // Should be 4KB aligned
  CHECK((addr % 4096) == 0);

  heap->Release(addr, nullptr);
}

TEST_CASE("LookupHeapByType selects correct heap", "[memory][heap]") {
  auto& memory = GetTestMemory();

  SECTION("Virtual heap with 4KB pages") {
    auto* heap = memory.LookupHeapByType(false, 4096);
    REQUIRE(heap != nullptr);
    // Should be v00000000 heap
  }

  SECTION("Virtual heap with 64KB pages") {
    auto* heap = memory.LookupHeapByType(false, 65536);
    REQUIRE(heap != nullptr);
    // Should be v40000000 heap
  }

  SECTION("Physical heap with 4KB pages") {
    auto* heap = memory.LookupHeapByType(true, 4096);
    REQUIRE(heap != nullptr);
    // Should be vE0000000 heap
  }

  SECTION("Physical heap with 64KB pages") {
    auto* heap = memory.LookupHeapByType(true, 65536);
    REQUIRE(heap != nullptr);
    // Should be vA0000000 heap
  }

  SECTION("Physical heap with 16MB pages") {
    auto* heap = memory.LookupHeapByType(true, 16 * 1024 * 1024);
    REQUIRE(heap != nullptr);
    // Should be vC0000000 heap
  }
}
