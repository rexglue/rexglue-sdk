/**
 * @file        tests/unit/runtime/memory_view_test.cpp
 * @brief       Address translation at every heap edge
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>

#include <rex/runtime.h>
#include <rex/runtime/memory_view.h>
#include <rex/runtime/xmemory.h>

#include "../kernel/kernel_fixture.h"

namespace {

constexpr std::array<u32, 17> kHeapEdges = {
    0x00000000, 0x3FFFFFFF, 0x40000000, 0x7EFFFFFF, 0x7F000000, 0x7FFFFFFF,
    0x80000000, 0x8FFFFFFF, 0x90000000, 0x9FFFFFFF, 0xA0000000, 0xBFFFFFFF,
    0xC0000000, 0xDFFFFFFF, 0xE0000000, 0xE0001000, 0xFFCFFFFF,
};

u32 HardwarePhysical(u32 address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000 ? 0x1000 : 0);
}

void CheckAliases(rex::runtime::Memory& memory, u32 address) {
  CAPTURE(address);
  CHECK(memory.GetPhysicalAddress(address) == HardwarePhysical(address));
  u8* virtual_byte = memory.TranslateVirtual(address);
  u8* physical_byte = memory.TranslatePhysical(HardwarePhysical(address));
  *virtual_byte = 0x5A;
  CHECK(*physical_byte == 0x5A);
  *physical_byte = 0xA5;
  CHECK(*virtual_byte == 0xA5);
  CHECK(memory.HostToGuestVirtual(virtual_byte) == address);
}

u8* HeapLookup(const rex::runtime::Memory& memory, u32 address) {
  const rex::runtime::BaseHeap* heap = memory.LookupHeap(address);
  return memory.GetVirtualMembase() + address + (heap ? heap->GetHostAddressOffset() : 0);
}

}  // namespace

REX_TEST_CASE("TranslateVirtual equals the heap lookup at every heap edge", "[runtime]") {
  rex::test::KernelFixture fixture;
  const rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  for (const u32 address : kHeapEdges) {
    CHECK(memory.TranslateVirtual(address) == HeapLookup(memory, address));
  }
}

REX_TEST_CASE("HostToGuestVirtual inverts TranslateVirtual at every heap edge", "[runtime]") {
  rex::test::KernelFixture fixture;
  const rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  for (const u32 address : kHeapEdges) {
    CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(address)) == address);
  }
}

REX_TEST_CASE("MemoryView returns what Memory returns", "[runtime]") {
  rex::test::KernelFixture fixture;
  const rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  const rex::runtime::MemoryView view(memory);
  CHECK(view.GetVirtualMembase() == memory.GetVirtualMembase());
  for (const u32 address : kHeapEdges) {
    u8* host = memory.TranslateVirtual(address);
    CHECK(view.TranslateVirtual(address) == host);
    CHECK(view.HostToGuestVirtual(host) == memory.HostToGuestVirtual(host));
  }
}

REX_TEST_CASE("TranslateVirtual above the 0xE0 heap matches recompiled code", "[runtime]") {
  rex::test::KernelFixture fixture;
  const rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  for (const u32 address : {0xFFD00000u, 0xFFFFFFFFu}) {
    CHECK(memory.TranslateVirtual(address) ==
          rex::runtime::GuestPtr(memory.GetVirtualMembase(), address));
  }
}

REX_TEST_CASE("HostToGuestVirtual inverts TranslateVirtual above the 0xE0 heap", "[runtime]") {
  rex::test::KernelFixture fixture;
  const rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  const rex::runtime::MemoryView view(memory);
  for (const u32 address : {0xE0000FFFu, 0xFFCFF000u, 0xFFD00000u, 0xFFD00FFFu, 0xFFD01000u,
                            0xFFFFEFFFu, 0xFFFFF000u, 0xFFFFFFFFu}) {
    CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(address)) == address);
    CHECK(view.HostToGuestVirtual(view.TranslateVirtual(address)) == address);
  }
}

REX_TEST_CASE("Every physical heap reaches the physical byte the hardware maps", "[runtime]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  for (const u32 page_size : {4096u, 64u * 1024u, 16u * 1024u * 1024u}) {
    for (const bool top_down : {false, true}) {
      CAPTURE(page_size, top_down);
      rex::runtime::BaseHeap* heap = memory.LookupHeapByType(true, page_size);
      REQUIRE(heap != nullptr);
      u32 address = 0;
      REQUIRE(heap->Alloc(
          page_size * 2, page_size,
          rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
          rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, top_down,
          &address));
      for (const u32 offset : {0u, 0xFFFu, 0x1000u, page_size * 2 - 1}) {
        CheckAliases(memory, address + offset);
      }
      CHECK(heap->Release(address));
    }
  }
}

REX_TEST_CASE("The top guest page wraps to physical 0 as the hardware does", "[runtime]") {
  rex::test::KernelFixture fixture;
  const rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  CHECK(memory.TranslateVirtual(0xFFFFF000) == memory.TranslatePhysical(0));
  CHECK(memory.TranslateVirtual(0xFFFFFFFF) == memory.TranslatePhysical(0xFFF));
  CHECK(memory.TranslateVirtual(0xE0000000) == memory.GetVirtualMembase() + 0xE0001000u);
}
