/**
 * @file        tests/unit/kernel/guest_memory_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <cstring>
#include <string>
#include <thread>

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/heap.h>
#include <rex/types.h>

#include "kernel/kernel_fixture.h"
#include "kernel/title_state.h"

using rex::test::KernelFixture;

REX_TEST_CASE("GuestMemory loads and stores big endian values", "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  REQUIRE(memory.Store<be_u32>(*address, be_u32(0x01020304)).has_value());
  const u8* raw = StateOf(fixture.GetRuntime()).GetMemory()->TranslateVirtual<const u8*>(*address);
  CHECK(raw[0] == 0x01);
  CHECK(raw[1] == 0x02);
  CHECK(raw[2] == 0x03);
  CHECK(raw[3] == 0x04);
  auto loaded = memory.Load<be_u32>(*address);
  REQUIRE(loaded.has_value());
  CHECK(static_cast<u32>(*loaded) == 0x01020304);
  CHECK(memory.Free(*address).has_value());
}

REX_TEST_CASE("GuestMemory refuses a range that is not mapped", "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  CHECK_FALSE(memory.GetReadOnlySpan(0x10, 4).has_value());
  CHECK_FALSE(memory.Load<be_u32>(0x10).has_value());
  CHECK_FALSE(memory.Store<be_u32>(0x10, be_u32(1)).has_value());

  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  CHECK(memory.GetReadOnlySpan(*address, 0x1000).has_value());
  CHECK_FALSE(memory.GetReadOnlySpan(0xFFFFFFFC, 8).has_value());
  REQUIRE(memory.Free(*address).has_value());
  CHECK_FALSE(memory.GetReadOnlySpan(*address, 4).has_value());
}

REX_TEST_CASE("GuestMemory refuses a range that runs past the allocation",
              "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x10000, 0x10000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  CHECK(memory.GetReadOnlySpan(*address + 0x10000 - 4, 4).has_value());
  CHECK_FALSE(memory.GetReadOnlySpan(*address + 0x10000 - 4, 0x20000).has_value());
  CHECK(memory.Free(*address).has_value());
}

REX_TEST_CASE("GuestMemory refuses a write span over read only memory", "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadOnly);
  REQUIRE(address.has_value());
  CHECK(memory.GetReadOnlySpan(*address, 16).has_value());
  CHECK_FALSE(memory.GetWritableSpan(*address, 16).has_value());
  const std::array<std::byte, 4> bytes = {};
  CHECK_FALSE(memory.CopyTo(*address, bytes).has_value());
  CHECK(memory.Free(*address).has_value());
}

REX_TEST_CASE("GuestMemory copies bytes both ways", "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  const std::array<std::byte, 3> in = {std::byte{7}, std::byte{8}, std::byte{9}};
  REQUIRE(memory.CopyTo(*address + 5, in).has_value());
  std::array<std::byte, 3> out = {};
  REQUIRE(memory.CopyFrom(*address + 5, out).has_value());
  CHECK(out == in);
  auto span = memory.GetWritableSpan(*address, 16);
  REQUIRE(span.has_value());
  CHECK(span->size() == 16);
  CHECK((*span)[5] == std::byte{7});
  CHECK(memory.Free(*address).has_value());
}

REX_TEST_CASE("GuestMemory reads zero terminated strings", "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  const std::array<char, 6> text = {'h', 'e', 'l', 'l', 'o', 0};
  REQUIRE(memory.CopyTo(*address, std::as_bytes(std::span(text))).has_value());
  auto narrow = memory.ReadString(*address);
  REQUIRE(narrow.has_value());
  CHECK(*narrow == "hello");
  CHECK(memory.ReadString(*address, 3).value() == "hel");

  const std::array<be_u16, 3> wide = {be_u16('h'), be_u16('i'), be_u16(0)};
  REQUIRE(memory.Store(*address + 0x100, wide).has_value());
  auto widened = memory.ReadString16(*address + 0x100);
  REQUIRE(widened.has_value());
  CHECK(*widened == u"hi");
  CHECK_FALSE(memory.ReadString(0x10).has_value());
  CHECK(memory.Free(*address).has_value());
}

REX_TEST_CASE("GuestMemory gives a view that a non title thread can use",
              "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  REQUIRE(memory.Store<be_u32>(*address, be_u32(0x01020304)).has_value());

  const rex::kernel::MemoryView view = memory.GetView();
  u32 loaded = 0;
  const u8* first = nullptr;
  std::thread other([&] {
    loaded = view.Load<u32>(*address);
    first = view.TryTranslateVirtual<const u8*>(*address);
  });
  other.join();
  CHECK(loaded == 0x01020304);
  REQUIRE(first != nullptr);
  CHECK(first[0] == 0x01);
  CHECK(view.TryTranslateVirtual<const u8*>(0x10) == nullptr);
  CHECK(memory.Free(*address).has_value());
}

REX_TEST_CASE("GuestMemory Protect changes the access of a page", "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  CHECK(memory.GetWritableSpan(*address, 16).has_value());
  REQUIRE(memory.Protect(*address, 0x1000, rex::kernel::GuestProtect::kReadOnly).has_value());
  CHECK_FALSE(memory.GetWritableSpan(*address, 16).has_value());
  CHECK(memory.GetReadOnlySpan(*address, 16).has_value());
  REQUIRE(memory.Protect(*address, 0x1000, rex::kernel::GuestProtect::kReadWrite).has_value());
  CHECK(memory.GetWritableSpan(*address, 16).has_value());
  CHECK(memory.Free(*address).has_value());
  CHECK_FALSE(memory.Protect(*address, 0x1000, rex::kernel::GuestProtect::kReadOnly).has_value());
}

REX_TEST_CASE("GuestMemory HostToGuest inverts the spans and refuses outside pointers",
              "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  auto address = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  REQUIRE(address.has_value());
  auto span = memory.GetWritableSpan(*address + 8, 16);
  REQUIRE(span.has_value());
  auto round_trip = memory.HostToGuest(span->data());
  REQUIRE(round_trip.has_value());
  CHECK(*round_trip == *address + 8);

  int local = 0;
  CHECK_FALSE(memory.HostToGuest(&local).has_value());
  CHECK_FALSE(memory.HostToGuest(nullptr).has_value());
  CHECK(memory.Free(*address).has_value());
}

REX_TEST_CASE("GuestMemory allocates from the system heap and frees from it",
              "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  rex::kernel::Memory* raw = StateOf(fixture.GetRuntime()).GetMemory();

  auto title = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite);
  auto system = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadWrite,
                                rex::kernel::GuestHeap::kSystem);
  REQUIRE(title.has_value());
  REQUIRE(system.has_value());
  CHECK(raw->LookupHeap(*title)->GetPageSize() == 0x10000);
  CHECK(raw->LookupHeap(*system)->GetPageSize() == 0x1000);

  auto span = memory.GetWritableSpan(*system, 0x1000);
  REQUIRE(span.has_value());
  CHECK((*span)[0] == std::byte{0});
  CHECK(memory.Free(*system).has_value());
  CHECK_FALSE(memory.GetReadOnlySpan(*system, 4).has_value());

  auto read_only = memory.Allocate(0x1000, 0x1000, rex::kernel::GuestProtect::kReadOnly,
                                   rex::kernel::GuestHeap::kSystem);
  REQUIRE(read_only.has_value());
  CHECK_FALSE(memory.GetWritableSpan(*read_only, 16).has_value());
  CHECK(memory.Free(*read_only).has_value());
  CHECK(memory.Free(*title).has_value());
}

REX_TEST_CASE("GuestMemory allocates physical memory in 64K pages and frees it",
              "[kernel][guest_memory]") {
  KernelFixture fixture;
  rex::kernel::GuestMemory& memory = fixture.GetRuntime().GetGuestMemory();
  rex::kernel::Memory* raw = StateOf(fixture.GetRuntime()).GetMemory();

  const u32 before = raw->GetTitlePages();
  auto address = memory.Allocate(0x10000, 0x10000, rex::kernel::GuestProtect::kReadWrite,
                                 rex::kernel::GuestHeap::kPhysical);
  REQUIRE(address.has_value());
  CHECK(*address >= 0xA0000000);
  CHECK(*address < 0xC0000000);
  CHECK(*address % 0x10000 == 0);
  CHECK(raw->LookupHeap(*address)->GetPageSize() == 0x10000);
  CHECK(raw->GetTitlePages() == before + 0x10000 / 0x1000);
  CHECK(memory.GetWritableSpan(*address, 0x10000).has_value());

  REQUIRE(memory.Free(*address).has_value());
  CHECK(raw->GetTitlePages() == before);
  CHECK_FALSE(memory.GetReadOnlySpan(*address, 4).has_value());
}
