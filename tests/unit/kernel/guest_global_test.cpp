/**
 * @file        tests/unit/kernel/guest_global_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <thread>

#include <rex/kernel/memory_view.h>
#include <rex/kernel/thread_state.h>

#include "../kernel/kernel_fixture.h"
#include "kernel/title_state.h"

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

REX_TEST_CASE("GuestGlobal resolves its address in guest memory", "[kernel][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    MapRegion(c);
    const auto* memory = StateOf(fixture.GetRuntime()).GetMemory();
    CHECK(kCounter.GetHost() == memory->TranslateVirtual<rex::be_u32*>(kRegion + 0x10));
    CHECK(kPair.GetHost() == memory->TranslateVirtual<Pair*>(kRegion + 0x20));
    CHECK(kCounter.TryGetHost() == kCounter.GetHost());
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
              "[kernel][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    MapRegion(c);
    c.Store32(kRegion + 0x10, 0xBEEF);
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    CHECK(kCounter.GetHost(view) == kCounter.GetHost());
    u32 seen = 0;
    const rex::be_u32* host_pointer = nullptr;
    const rex::be_u32* unbound = host_pointer + 1;
    std::thread host([&] {
      unbound = kCounter.TryGetHost();
      host_pointer = kCounter.GetHost(view);
      seen = *host_pointer;
    });
    host.join();
    CHECK(unbound == nullptr);
    CHECK(host_pointer == kCounter.GetHost());
    CHECK(seen == 0xBEEF);
  });
}

REX_TEST_CASE("GuestGlobal TryGetHost is null until the title can read the address",
              "[kernel][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    CHECK(kCounter.TryGetHost() == nullptr);
    MapRegion(c);
    CHECK(kCounter.TryGetHost() != nullptr);
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    c.Store32(base_ptr, kRegion);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
    CHECK(kCounter.TryGetHost() == nullptr);
    CHECK(kPair.TryGetHost() == nullptr);
  });
}

REX_TEST_CASE("GuestGlobal TryGetHost through a memory view needs no bound thread",
              "[kernel][guest_global]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](rex::test::Caller& c) {
    const rex::kernel::MemoryView view(*StateOf(fixture.GetRuntime()).GetMemory());
    CHECK(kCounter.TryGetHost(view) == nullptr);
    MapRegion(c);
    CHECK(kCounter.TryGetHost(view) == kCounter.GetHost());
    const rex::be_u32* seen = nullptr;
    std::thread host([&] { seen = kCounter.TryGetHost(view); });
    host.join();
    CHECK(seen == kCounter.GetHost());
    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    const u32 old_ptr = c.Alloc(4);
    c.Store32(base_ptr, kRegion);
    c.Store32(size_ptr, 0x10000);
    REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
    CHECK(kCounter.TryGetHost(view) == nullptr);
  });
}
