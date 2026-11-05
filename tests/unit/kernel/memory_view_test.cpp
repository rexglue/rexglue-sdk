/**
 * @file        tests/unit/kernel/memory_view_test.cpp
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
#include <cstddef>
#include <cstring>

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/heap.h>
#include <rex/memory/mapped_ptr.h>

#include "../kernel/kernel_fixture.h"
#include "kernel/title_state.h"

namespace {

constexpr std::array<u32, 17> kHeapEdges = {
    0x00000000, 0x3FFFFFFF, 0x40000000, 0x7EFFFFFF, 0x7F000000, 0x7FFFFFFF,
    0x80000000, 0x8FFFFFFF, 0x90000000, 0x9FFFFFFF, 0xA0000000, 0xBFFFFFFF,
    0xC0000000, 0xDFFFFFFF, 0xE0000000, 0xE0001000, 0xFFCFFFFF,
};

u32 HardwarePhysical(u32 address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000 ? 0x1000 : 0);
}

void CheckAliases(rex::kernel::Memory& memory, u32 address) {
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

u8* HeapLookup(const rex::kernel::Memory& memory, u32 address) {
  const rex::kernel::BaseHeap* heap = memory.LookupHeap(address);
  return memory.GetVirtualMembase() + address + (heap ? heap->GetHostAddressOffset() : 0);
}

}  // namespace

REX_TEST_CASE("TranslateVirtual equals the heap lookup at every heap edge", "[kernel]") {
  rex::test::KernelFixture fixture;
  const rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  const rex::kernel::MemoryView view(memory);
  CHECK(view.GetVirtualMembase() == memory.GetVirtualMembase());
  for (const u32 address : kHeapEdges) {
    CHECK(memory.TranslateVirtual(address) == HeapLookup(memory, address));
    CHECK(view.TranslateVirtual(address) == HeapLookup(memory, address));
  }
}

REX_TEST_CASE("HostToGuestVirtual inverts TranslateVirtual at every heap edge", "[kernel]") {
  rex::test::KernelFixture fixture;
  const rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  const rex::kernel::MemoryView view(memory);
  for (const u32 address : kHeapEdges) {
    CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(address)) == address);
    CHECK(view.HostToGuestVirtual(view.TranslateVirtual(address)) == address);
  }
}

REX_TEST_CASE("An allocated virtual page is writable through TranslateVirtual", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  const rex::kernel::MemoryView view(memory);
  rex::kernel::BaseHeap* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);
  u32 address = 0;
  REQUIRE(heap->Alloc(
      4096, 4096, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &address));
  u8* host = memory.TranslateVirtual(address);
  REQUIRE(host != nullptr);
  host[0] = 0xAB;
  CHECK(view.Load<u8>(address) == 0xAB);
  CHECK(memory.HostToGuestVirtual(host) == address);
  CHECK(view.HostToGuestVirtual(host) == address);
  CHECK(heap->Release(address));
}

REX_TEST_CASE("TranslateVirtual above the 0xE0 heap matches recompiled code", "[kernel]") {
  rex::test::KernelFixture fixture;
  const rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  const rex::kernel::MemoryView view(memory);
  for (const u32 address : {0xFFD00000u, 0xFFFFFFFFu}) {
    CHECK(memory.TranslateVirtual(address) ==
          rex::memory::GuestPtr(memory.GetVirtualMembase(), address));
    CHECK(view.TranslateVirtual(address) ==
          rex::memory::GuestPtr(memory.GetVirtualMembase(), address));
  }
}

REX_TEST_CASE("HostToGuestVirtual inverts TranslateVirtual above the 0xE0 heap", "[kernel]") {
  rex::test::KernelFixture fixture;
  const rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  const rex::kernel::MemoryView view(memory);
  for (const u32 address : {0xE0000FFFu, 0xFFCFF000u, 0xFFD00000u, 0xFFD00FFFu, 0xFFD01000u,
                            0xFFFFEFFFu, 0xFFFFF000u, 0xFFFFFFFFu}) {
    CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(address)) == address);
    CHECK(view.HostToGuestVirtual(view.TranslateVirtual(address)) == address);
  }
}

REX_TEST_CASE("Every physical heap reaches the physical byte the hardware maps", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  for (const u32 page_size : {4096u, 64u * 1024u, 16u * 1024u * 1024u}) {
    for (const bool top_down : {false, true}) {
      CAPTURE(page_size, top_down);
      rex::kernel::BaseHeap* heap = memory.LookupHeapByType(true, page_size);
      REQUIRE(heap != nullptr);
      u32 address = 0;
      REQUIRE(heap->Alloc(
          page_size * 2, page_size,
          rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
          rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, top_down, &address));
      for (const u32 offset : {0u, 0xFFFu, 0x1000u, page_size * 2 - 1}) {
        CheckAliases(memory, address + offset);
      }
      CHECK(heap->Release(address));
    }
  }
}

REX_TEST_CASE("The top guest page wraps to physical 0 as the hardware does", "[kernel]") {
  rex::test::KernelFixture fixture;
  const rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  if (rex::memory::detail::kPhysicalOffsetInCode) {
    CHECK(memory.TranslateVirtual(0xFFFFF000) == memory.TranslatePhysical(0));
    CHECK(memory.TranslateVirtual(0xFFFFFFFF) == memory.TranslatePhysical(0xFFF));
  }
  CHECK(memory.TranslateVirtual(0xE0000000) ==
        memory.GetVirtualMembase() + 0xE0000000u +
            rex::memory::detail::PhysicalHostOffset(0xE0000000u));
}

REX_TEST_CASE("TryTranslateVirtual rejects null, misaligned and unmapped addresses", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
    const rex::kernel::MemoryView view(memory);
    const u32 block = c.Alloc(64);
    CHECK(view.TryTranslateVirtual<u32*>(block) == memory.TranslateVirtual<u32*>(block));
    CHECK(view.TryTranslateVirtual<u32*>(block, rex::kernel::kMemoryProtectWrite) ==
          memory.TranslateVirtual<u32*>(block));
    CHECK(view.TryTranslateVirtual<u32*>(block + 1) == nullptr);
    CHECK(view.TryTranslateVirtual<u32*>(0) == nullptr);
    CHECK(memory.TryTranslateVirtual<u32*>(0) == nullptr);
    CHECK(memory.TryTranslateVirtual<u32*>(block + 1) == nullptr);
    CHECK(memory.TryTranslateVirtual<u64*>(block + 4) == nullptr);
    CHECK(memory.TryTranslateVirtual<u8*>(block + 1) == memory.TranslateVirtual<u8*>(block + 1));
    CHECK(memory.TryTranslateVirtual<u32*>(block) == memory.TranslateVirtual<u32*>(block));
    CHECK(memory.TryTranslateVirtual<void*>(block + 3) ==
          memory.TranslateVirtual<void*>(block + 3));
    CHECK(memory.TryTranslateVirtual<u32*>(0x00000004) == nullptr);
    CHECK(memory.TryTranslateVirtual<u32*>(0xFFFFFFFC) == nullptr);
  });
}

REX_TEST_CASE("TryTranslateVirtual rejects a page that is not readable", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    c.Store32(base_ptr, 0);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, 0x3000, 0x04, 0}) == 0);
    const u32 block = c.Load32(base_ptr);
    CHECK(memory.TryTranslateVirtual<u32*>(block) == memory.TranslateVirtual<u32*>(block));
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
    CHECK(memory.TryTranslateVirtual<u32*>(block) == nullptr);
    c.Store32(base_ptr, block);
    c.Store32(size_ptr, 0);
    CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, 0x8000, 0}) == 0);
    CHECK(memory.TryTranslateVirtual<u32*>(block) == nullptr);
  });
}

REX_TEST_CASE("MemoryView Load reads big endian values at any alignment", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    const u32 block = c.Alloc(32);
    u8* bytes = view.TranslateVirtual<u8*>(block);
    const u8 pattern[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    std::memcpy(bytes, pattern, sizeof(pattern));
    CHECK(view.Load<u8>(block) == 0x01);
    CHECK(view.Load<i8>(block) == 0x01);
    CHECK(view.Load<u16>(block) == 0x0102);
    CHECK(view.Load<u32>(block) == 0x01020304);
    CHECK(view.Load<u64>(block) == 0x0102030405060708);
    CHECK(view.Load<u32>(block + 1) == 0x02030405);
    CHECK(view.Load<u16>(block + 3) == 0x0405);
    const u8 negative[] = {0xFF, 0xFE};
    std::memcpy(bytes + 16, negative, sizeof(negative));
    CHECK(view.Load<i16>(block + 16) == -2);
    const u8 one[] = {0x3F, 0x80, 0x00, 0x00};
    std::memcpy(bytes + 20, one, sizeof(one));
    CHECK(view.Load<f32>(block + 20) == 1.0f);
    const u8 two[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(bytes + 24, two, sizeof(two));
    CHECK(view.Load<f64>(block + 24) == 2.0);
  });
}

REX_TEST_CASE("MemoryView Store writes big endian values at any alignment", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    const u32 block = c.Alloc(32);
    const u8* bytes = view.TranslateVirtual<const u8*>(block);
    view.Store<u32>(block, 0x01020304);
    CHECK(bytes[0] == 0x01);
    CHECK(bytes[1] == 0x02);
    CHECK(bytes[2] == 0x03);
    CHECK(bytes[3] == 0x04);
    view.Store<u16>(block + 5, 0xA1B2);
    CHECK(bytes[5] == 0xA1);
    CHECK(bytes[6] == 0xB2);
    view.Store<u64>(block + 8, 0x1122334455667788);
    CHECK(bytes[8] == 0x11);
    CHECK(bytes[15] == 0x88);
    view.Store<f32>(block + 16, 1.0f);
    CHECK(bytes[16] == 0x3F);
    CHECK(bytes[17] == 0x80);
    view.Store<i16>(block + 20, -2);
    CHECK(bytes[20] == 0xFF);
    CHECK(bytes[21] == 0xFE);
    view.Store<u8>(block + 24, 0x7E);
    CHECK(bytes[24] == 0x7E);
    view.Store<f64>(block + 26, 2.0);
    CHECK(view.Load<f64>(block + 26) == 2.0);
    CHECK(view.Load<u32>(block) == 0x01020304);
  });
}

REX_TEST_CASE("MemoryView TryLoad and TryStore reject addresses the title cannot use", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    const u32 block = c.Alloc(16);
    CHECK(view.TryStore<u32>(block, 0xCAFEBABE));
    REQUIRE(view.TryLoad<u32>(block).has_value());
    CHECK(*view.TryLoad<u32>(block) == 0xCAFEBABE);
    CHECK(view.TryStore<u32>(block + 1, 0x01020304));
    CHECK(*view.TryLoad<u32>(block + 1) == 0x01020304);
    CHECK_FALSE(view.TryLoad<u32>(0).has_value());
    CHECK_FALSE(view.TryStore<u32>(0, 1));
    CHECK_FALSE(view.TryLoad<u32>(0x00000004).has_value());
    CHECK_FALSE(view.TryStore<u32>(0x00000004, 1));
    CHECK_FALSE(view.TryLoad<u32>(0xFFFFFFFE).has_value());
    CHECK_FALSE(view.TryStore<u64>(0xFFFFFFFC, 1));
  });
}

REX_TEST_CASE("MemoryView TryLoad and TryStore check every byte and the write bit", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    c.Store32(base_ptr, 0);
    c.Store32(size_ptr, 0x20000);
    REQUIRE(c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, 0x3000, 0x04, 0}) == 0);
    const u32 block = c.Load32(base_ptr);
    const u32 edge = block + 0x10000;
    c.Store32(base_ptr, edge);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
    CHECK(view.TryLoad<u32>(edge - 4).has_value());
    CHECK_FALSE(view.TryLoad<u32>(edge - 3).has_value());
    CHECK_FALSE(view.TryStore<u32>(edge - 2, 1));
    CHECK_FALSE(view.TryLoad<u8>(edge).has_value());
    c.Store32(base_ptr, block);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x02, old_ptr, 0}) == 0);
    CHECK(view.TryLoad<u32>(block).has_value());
    CHECK_FALSE(view.TryStore<u32>(block, 1));
  });
}

REX_TEST_CASE("MemoryView GetCString reads a null terminated string", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    const u32 block = c.Alloc(32);
    std::memcpy(view.TranslateVirtual<char*>(block), "hello\0world", 12);
    CHECK(view.GetCString(block) == "hello");
    CHECK(view.GetCString(block + 6) == "world");
    CHECK(view.GetCString(block + 5).empty());
    CHECK(view.GetCString(0).empty());
    CHECK(view.GetCString(block).data() == view.TranslateVirtual<const char*>(block));
  });
}

REX_TEST_CASE("MemoryView TryGetCString stops where the title cannot read", "[kernel]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    c.Store32(base_ptr, 0);
    c.Store32(size_ptr, 0x20000);
    REQUIRE(c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, 0x3000, 0x04, 0}) == 0);
    const u32 block = c.Load32(base_ptr);
    const u32 edge = block + 0x10000;
    std::memset(view.TranslateVirtual<char*>(block), 'x', 0x10000);
    std::memcpy(view.TranslateVirtual<char*>(edge), "tail", 5);
    char* longer = view.TranslateVirtual<char*>(edge + 0x100);
    std::memset(longer, 'y', 5000);
    longer[5000] = '\0';
    CHECK(view.TryGetCString(edge).value() == "tail");
    CHECK(view.TryGetCString(edge + 0x100).value().size() == 5000);
    CHECK(view.TryGetCString(block, 10).value() == "xxxxxxxxxx");
    CHECK(view.TryGetCString(edge - 3, 100).value() == "xxxtail");
    CHECK_FALSE(view.TryGetCString(0).has_value());
    CHECK_FALSE(view.TryGetCString(0x00000004).has_value());

    c.Store32(base_ptr, edge);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
    CHECK(view.TryGetCString(edge - 3).value() == "xxx");
    CHECK(view.TryGetCString(block + 0x0FFFF).value() == "x");
    CHECK_FALSE(view.TryGetCString(edge).has_value());
  });
}

REX_TEST_CASE("The 0xE0000000 view reaches physical 0x1000 on every host", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::kernel::Memory& memory = *StateOf(fixture.GetRuntime()).GetMemory();
  rex::kernel::BaseHeap* heap = memory.LookupHeapByType(true, 4096);
  REQUIRE(heap != nullptr);
  u32 block = 0;
  REQUIRE(heap->Alloc(
      0x2000, 0x1000, rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
      rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite, false, &block));
  REQUIRE(block >= 0xE0000000);
  for (const u32 x :
       {block - 0xE0000000, block - 0xE0000000 + 0xFFF, block - 0xE0000000 + 0x1FFF}) {
    CAPTURE(x);
    *memory.TranslateVirtual(0xE0000000 + x) = 0x3C;
    CHECK(*memory.TranslatePhysical(0x1000 + x) == 0x3C);
    *memory.TranslatePhysical(0x1000 + x) = 0xC3;
    CHECK(*memory.TranslateVirtual(0xE0000000 + x) == 0xC3);
    if (!rex::memory::detail::kPhysicalOffsetInCode) {
      CHECK(memory.GetVirtualMembase()[0xE0000000u + x] == 0xC3);
    }
  }
  CHECK(heap->Release(block));
}

REX_TEST_CASE("RemovePhysicalHostOffset inverts the host offset at the 0xE0000000 view edges",
              "[kernel]") {
  for (const u32 address : {0xDFFFFFFFu, 0xE0000000u, 0xE0000FFFu, 0xE0001000u, 0xFFCFFFFFu,
                            0xFFFFF000u, 0xFFFFFFFFu}) {
    CAPTURE(address);
    const std::size_t host_offset =
        static_cast<std::size_t>(address) + rex::memory::detail::PhysicalHostOffset(address);
    CHECK(rex::memory::detail::RemovePhysicalHostOffset(host_offset) == address);
  }
}
