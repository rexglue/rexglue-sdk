/**
 * @file        tests/unit/kernel/be_ptr_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstddef>
#include <thread>

#include <rex/runtime/memory_view.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::be_ptr;
using rex::be_u32;
using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

struct Pair {
  be_u32 first;
  be_u32 second;
};

}  // namespace

REX_TEST_CASE("be_ptr: a null pointer resolves to null", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller&) {
    be_ptr<Pair> pointer;
    pointer = 0;
    CHECK(pointer.GetAddress() == 0);
    CHECK_FALSE(static_cast<bool>(pointer));
    CHECK(pointer.get() == nullptr);
    const rex::runtime::MemoryView view(*rex::runtime::ThreadState::Get()->GetMemory());
    CHECK(pointer.get(view) == nullptr);
    pointer = 0x1234;
    pointer = nullptr;
    CHECK(pointer.GetAddress() == 0);
  });
}

REX_TEST_CASE("be_ptr: a guest address resolves to the guest memory", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 address = c.Alloc(sizeof(Pair));
    c.Store32(address, 0xCAFE);
    c.Store32(address + 4, 0xBEEF);
    be_ptr<Pair> pointer;
    pointer = address;
    CHECK(static_cast<bool>(pointer));
    CHECK(pointer.GetAddress() == address);
    REQUIRE(pointer.get() != nullptr);
    CHECK(pointer->first == 0xCAFE);
    CHECK(pointer->second == 0xBEEF);
    pointer->second = 0x1111;
    CHECK(c.Load32(address + 4) == 0x1111);
  });
}

REX_TEST_CASE("be_ptr: a host pointer round trips to the same guest address", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 address = c.Alloc(sizeof(Pair));
    be_ptr<Pair> source;
    source = address;
    Pair* host = source.get();
    be_ptr<Pair> pointer;
    pointer = host;
    CHECK(pointer.GetAddress() == address);
    pointer = static_cast<const Pair*>(host);
    CHECK(pointer.GetAddress() == address);
    CHECK(pointer.get() == host);
    pointer = static_cast<Pair*>(nullptr);
    CHECK(pointer.GetAddress() == 0);
  });
}

REX_TEST_CASE("be_ptr: the physical view round trips through the host pointer",
              "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller&) {
    const rex::runtime::MemoryView view(*rex::runtime::ThreadState::Get()->GetMemory());
    for (const u32 address : {0x00010000u, 0xDFFFFFF8u, 0xE0000000u, 0xE0001000u, 0xFFCFFFF8u}) {
      INFO(address);
      be_ptr<Pair> pointer;
      pointer = address;
      Pair* host = pointer.get();
      be_ptr<Pair> again;
      again = host;
      CHECK(again.GetAddress() == address);
      CHECK(pointer.get(view) == host);
    }
  });
}

REX_TEST_CASE("be_ptr: the memory view overload resolves without a bound thread",
              "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 address = c.Alloc(sizeof(Pair));
    c.Store32(address, 0x77);
    const rex::runtime::MemoryView view(*rex::runtime::ThreadState::Get()->GetMemory());
    be_ptr<Pair> pointer;
    pointer = address;
    REQUIRE(pointer.get(view) != nullptr);
    CHECK(pointer.get(view)->first == 0x77);
    CHECK(pointer.get(view) == pointer.get());
  });
}

REX_TEST_CASE("be_ptr: TryGet returns null for an address the title cannot read",
              "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 address = c.Alloc(sizeof(Pair));
    be_ptr<Pair> pointer;
    pointer = address;
    CHECK(pointer.TryGet() == pointer.get());
    pointer = address + 1;
    CHECK(pointer.TryGet() == nullptr);
    pointer = 0;
    CHECK(pointer.TryGet() == nullptr);
    pointer = 0x00000004;
    CHECK(pointer.TryGet() == nullptr);
  });
}

namespace {

u32 MakeVector(Caller& c, u32 element_size, u32 count, u32 capacity) {
  const u32 header = c.Alloc(12);
  const u32 storage = c.Alloc(element_size * capacity);
  c.Store32(header, storage);
  c.Store32(header + 4, storage + element_size * count);
  c.Store32(header + 8, storage + element_size * capacity);
  return header;
}

}  // namespace

static_assert(sizeof(rex::be_vector<Pair>) == 12);
static_assert(offsetof(rex::be_vector<Pair>, last) == 4);
static_assert(offsetof(rex::be_vector<Pair>, limit) == 8);

REX_TEST_CASE("be_vector: a zeroed vector is empty", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = c.Alloc(12);
    c.Store32(header, 0);
    c.Store32(header + 4, 0);
    c.Store32(header + 8, 0);
    const auto* vector = rex::runtime::ThreadState::Get()
                             ->GetMemory()
                             ->TranslateVirtual<const rex::be_vector<be_u32>*>(header);
    CHECK(vector->size() == 0);
    CHECK(vector->empty());
    CHECK(vector->begin() == nullptr);
    CHECK(vector->end() == nullptr);
  });
}

REX_TEST_CASE("be_vector: elements resolve by the element size", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = MakeVector(c, sizeof(Pair), 3, 8);
    auto* memory = rex::runtime::ThreadState::Get()->GetMemory();
    const auto* vector = memory->TranslateVirtual<const rex::be_vector<Pair>*>(header);
    const u32 storage = vector->first.GetAddress();
    for (u32 i = 0; i < 3; ++i) {
      c.Store32(storage + i * 8, 10 + i);
      c.Store32(storage + i * 8 + 4, 20 + i);
    }
    CHECK(vector->size() == 3);
    CHECK_FALSE(vector->empty());
    CHECK(vector->end() - vector->begin() == 3);
    CHECK((*vector)[0].first == 10);
    CHECK((*vector)[2].second == 22);
    u32 sum = 0;
    for (const Pair& pair : *vector) {
      sum += pair.first + pair.second;
    }
    CHECK(sum == 10 + 11 + 12 + 20 + 21 + 22);
    CHECK(vector->limit.GetAddress() == storage + 8 * 8);
  });
}

REX_TEST_CASE("be_vector: pointers out of order give an empty vector", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = MakeVector(c, 4, 2, 4);
    const auto* vector =
        rex::runtime::ThreadState::Get()->GetMemory()->TranslateVirtual<rex::be_vector<be_u32>*>(
            header);
    c.Store32(header + 4, vector->first.GetAddress() - 4);
    CHECK(vector->size() == 0);
    CHECK(vector->empty());
  });
}

REX_TEST_CASE("be_vector: a memory view resolves the elements without a bound thread",
              "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = MakeVector(c, 4, 2, 2);
    const rex::runtime::MemoryView view(*rex::runtime::ThreadState::Get()->GetMemory());
    const auto* vector = view.TranslateVirtual<const rex::be_vector<be_u32>*>(header);
    c.Store32(vector->first.GetAddress(), 7);
    c.Store32(vector->first.GetAddress() + 4, 9);
    CHECK(vector->begin(view) == vector->first.get(view));
    CHECK(vector->end(view) - vector->begin(view) == 2);
    CHECK(*vector->begin(view) == 7);
    CHECK(vector->begin(view)[1] == 9);
  });
}

REX_TEST_CASE("be_ptr: TryGet through a memory view needs no bound thread", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const rex::runtime::MemoryView view(*rex::runtime::ThreadState::Get()->GetMemory());
    const u32 address = c.Alloc(sizeof(Pair));
    be_ptr<Pair> pointer;
    pointer = address;
    CHECK(pointer.TryGet(view) == pointer.get(view));
    const Pair* seen = nullptr;
    std::thread host([&] { seen = pointer.TryGet(view); });
    host.join();
    CHECK(seen == pointer.get(view));
    pointer = address + 1;
    CHECK(pointer.TryGet(view) == nullptr);
    pointer = 0;
    CHECK(pointer.TryGet(view) == nullptr);
    pointer = 0x00000004;
    CHECK(pointer.TryGet(view) == nullptr);
  });
}

REX_TEST_CASE("be_vector: GetAddress gives element addresses and 0 out of range",
              "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = MakeVector(c, sizeof(Pair), 3, 8);
    const auto* vector = rex::runtime::ThreadState::Get()
                             ->GetMemory()
                             ->TranslateVirtual<const rex::be_vector<Pair>*>(header);
    const u32 storage = vector->first.GetAddress();
    CHECK(vector->GetAddress(0) == storage);
    CHECK(vector->GetAddress(2) == storage + 2 * sizeof(Pair));
    CHECK(vector->GetAddress(3) == 0);
    CHECK(vector->GetAddress(7) == 0);
    CHECK(vector->GetAddress(~size_t{0}) == 0);
  });
}

REX_TEST_CASE("be_vector: TryAt resolves elements the title can read", "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = MakeVector(c, sizeof(Pair), 3, 8);
    const rex::runtime::MemoryView view(*rex::runtime::ThreadState::Get()->GetMemory());
    const auto* vector = view.TranslateVirtual<const rex::be_vector<Pair>*>(header);
    c.Store32(vector->GetAddress(1), 5);
    REQUIRE(vector->TryAt(1) != nullptr);
    CHECK(vector->TryAt(1)->first == 5);
    CHECK(vector->TryAt(1) == &(*vector)[1]);
    CHECK(vector->TryAt(2, view) == &(*vector)[2]);
    CHECK(vector->TryAt(3) == nullptr);
    CHECK(vector->TryAt(3, view) == nullptr);
    const Pair* seen = nullptr;
    const Pair* unbound = seen + 1;
    std::thread host([&] {
      unbound = vector->TryAt(0);
      seen = vector->TryAt(0, view);
    });
    host.join();
    CHECK(unbound == nullptr);
    CHECK(seen == &(*vector)[0]);
  });
}

REX_TEST_CASE("be_vector: TryAt rejects an element that reaches unreadable memory",
              "[kernel][be_ptr]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
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
    const u32 header = c.Alloc(12);
    c.Store32(header, edge - 8);
    c.Store32(header + 4, edge + 4);
    c.Store32(header + 8, edge + 4);
    const rex::runtime::MemoryView view(*rex::runtime::ThreadState::Get()->GetMemory());
    const auto* vector = view.TranslateVirtual<const rex::be_vector<Pair>*>(header);
    REQUIRE(vector->size() == 1);
    CHECK(vector->TryAt(0) != nullptr);
    c.Store32(header, edge - 4);
    c.Store32(header + 4, edge + 4);
    REQUIRE(vector->size() == 1);
    CHECK(vector->TryAt(0) == nullptr);
    CHECK(vector->TryAt(0, view) == nullptr);
  });
}
