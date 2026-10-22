/**
 * @file        tests/unit/runtime/guest_global_test.cpp
 * @brief       Globals bound to fixed guest addresses
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <thread>

#include <rex/runtime/guest_global.h>
#include <rex/runtime/memory_view.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xtypes.h>

#include "../kernel/kernel_fixture.h"

namespace {

struct Pair {
  rex::be_u32 first;
  rex::be_u32 second;
};

constexpr u32 kRegion = 0x50000000;

}  // namespace

REX_GUEST_GLOBAL(rex::be_u32, kCounter, kRegion + 0x10);
REX_GUEST_GLOBAL(Pair, kPair, kRegion + 0x20);

static_assert(kCounter.GetAddress() == kRegion + 0x10);
static_assert(kPair.GetAddress() == kRegion + 0x20);
static_assert(std::is_empty_v<decltype(kCounter)>);

namespace {

void MapRegion(rex::test::Caller& c, u32 protect = 0x04) {
  const u32 base_ptr = c.Alloc(4);
  const u32 size_ptr = c.Alloc(4);
  c.Store32(base_ptr, kRegion);
  c.Store32(size_ptr, 0x10000);
  REQUIRE(c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, 0x3000, protect, 0}) == 0);
  REQUIRE(c.Load32(base_ptr) == kRegion);
}

}  // namespace

REX_TEST_CASE("GuestGlobal resolves its address in guest memory", "[runtime][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    MapRegion(c);
    const auto* memory = fixture.GetRuntime().GetMemory();
    CHECK(kCounter.get() == memory->TranslateVirtual<rex::be_u32*>(kRegion + 0x10));
    CHECK(kPair.get() == memory->TranslateVirtual<Pair*>(kRegion + 0x20));
    CHECK(kCounter.TryGet() == kCounter.get());
    c.Store32(kRegion + 0x10, 0xCAFE);
    CHECK(kCounter->Get() == 0xCAFE);
    CHECK(*kCounter == 0xCAFE);
    c.Store32(kRegion + 0x20, 11);
    c.Store32(kRegion + 0x24, 22);
    CHECK(kPair->first == 11);
    CHECK(kPair->second == 22);
    kPair->second = 33;
    CHECK(c.Load32(kRegion + 0x24) == 33);
    (*kCounter).Set(7);
    CHECK(c.Load32(kRegion + 0x10) == 7);
  });
}

REX_TEST_CASE("GuestGlobal resolves through a memory view without a bound thread",
              "[runtime][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    MapRegion(c);
    c.Store32(kRegion + 0x10, 0xBEEF);
    const rex::runtime::MemoryView view(*fixture.GetRuntime().GetMemory());
    CHECK(kCounter.get(view) == kCounter.get());
    u32 seen = 0;
    const rex::be_u32* host_pointer = nullptr;
    const rex::be_u32* unbound = host_pointer + 1;
    std::thread host([&] {
      unbound = kCounter.TryGet();
      host_pointer = kCounter.get(view);
      seen = *host_pointer;
    });
    host.join();
    CHECK(unbound == nullptr);
    CHECK(host_pointer == kCounter.get());
    CHECK(seen == 0xBEEF);
  });
}

REX_TEST_CASE("GuestGlobal TryGet is null until the title can read the address",
              "[runtime][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    CHECK(kCounter.TryGet() == nullptr);
    MapRegion(c);
    CHECK(kCounter.TryGet() != nullptr);
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    c.Store32(base_ptr, kRegion);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
    CHECK(kCounter.TryGet() == nullptr);
    CHECK(kPair.TryGet() == nullptr);
  });
}

REX_TEST_CASE("GuestGlobal TryGet through a memory view needs no bound thread",
              "[runtime][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::runtime::MemoryView view(*fixture.GetRuntime().GetMemory());
    CHECK(kCounter.TryGet(view) == nullptr);
    MapRegion(c);
    CHECK(kCounter.TryGet(view) == kCounter.get());
    const rex::be_u32* seen = nullptr;
    std::thread host([&] { seen = kCounter.TryGet(view); });
    host.join();
    CHECK(seen == kCounter.get());
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    c.Store32(base_ptr, kRegion);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
    CHECK(kCounter.TryGet(view) == nullptr);
  });
}
