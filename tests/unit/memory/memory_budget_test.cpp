/**
 * @file        tests/unit/memory/memory_budget_test.cpp
 * @brief       The title memory budget at heap level and at the kernel exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <latch>
#include <thread>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/kernel/init.h>
#include <rex/runtime/error.h>
#include <rex/runtime.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/xmemory.h>

#include "core/xex_builder.h"
#include "kernel/kernel_fixture.h"
#include "test_memory.h"

using rex::u32;
using rex::X_STATUS;
using rex::runtime::BaseHeap;
using rex::runtime::HeapAllocationInfo;
using rex::runtime::Memory;
using rex::runtime::PhysicalHeap;
using rex::testing::Caller;
using rex::testing::GetTestMemory;
using rex::testing::KernelFixture;
using rex::testing::ReleaseTestMemory;

namespace {

constexpr u32 kRW = rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite;
constexpr u32 kCommit =
    rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit;
constexpr u32 kTitle = kCommit | rex::runtime::kMemoryAllocationTitle;

BaseHeap* VirtualHeap4K(Memory& memory) {
  return memory.LookupHeap(0x10000000);
}

u32 RoomAfter(Memory& memory, u32 room_pages) {
  memory.SetBudget(memory.title_pages() / 256 + 1);
  const u32 room = memory.budget_pages() - memory.title_pages();
  REQUIRE(room >= room_pages);
  u32 filler = 0;
  if (room > room_pages) {
    REQUIRE(VirtualHeap4K(memory)->Alloc((room - room_pages) * 4096, 4096, kTitle, kRW, false,
                                         &filler));
  }
  REQUIRE(memory.budget_pages() - memory.title_pages() == room_pages);
  return filler;
}

void ReleaseFiller(Memory& memory, u32 filler) {
  if (filler) {
    VirtualHeap4K(memory)->Release(filler);
  }
  memory.SetBudget(0);
}

}  // namespace

TEST_CASE("A title commit counts its pages and a release returns them", "[memory][budget]") {
  Memory& memory = GetTestMemory();
  memory.SetBudget(0);
  const u32 before = memory.title_pages();
  u32 address = 0;
  REQUIRE(VirtualHeap4K(memory)->Alloc(0x10000, 4096, kTitle, kRW, false, &address));
  CHECK(memory.title_pages() == before + 16);
  REQUIRE(VirtualHeap4K(memory)->Release(address));
  CHECK(memory.title_pages() == before);
}

TEST_CASE("Commits without the title flag and system heap blocks are not counted",
          "[memory][budget]") {
  Memory& memory = GetTestMemory();
  memory.SetBudget(0);
  const u32 before = memory.title_pages();
  u32 address = 0;
  REQUIRE(VirtualHeap4K(memory)->Alloc(0x10000, 4096, kCommit, kRW, false, &address));
  const u32 system = memory.SystemHeapAlloc(0x10000);
  CHECK(memory.title_pages() == before);
  memory.SystemHeapFree(system);
  REQUIRE(VirtualHeap4K(memory)->Release(address));
}

TEST_CASE("The commit that crosses the budget fails with nothing committed", "[memory][budget]") {
  Memory& memory = GetTestMemory();
  const u32 filler = RoomAfter(memory, 8);
  BaseHeap* heap = VirtualHeap4K(memory);
  const u32 unreserved = heap->unreserved_page_count();
  const u32 used = memory.title_pages();
  u32 address = 0;
  CHECK_FALSE(heap->Alloc(9 * 4096, 4096, kTitle, kRW, false, &address));
  CHECK(memory.title_pages() == used);
  CHECK(heap->unreserved_page_count() == unreserved);
  REQUIRE(heap->Alloc(8 * 4096, 4096, kTitle, kRW, false, &address));
  CHECK(memory.title_pages() == memory.budget_pages());
  REQUIRE(heap->Release(address));
  ReleaseFiller(memory, filler);
}

TEST_CASE("A commit in pieces counts each page once and a decommit returns it",
          "[memory][budget]") {
  Memory& memory = GetTestMemory();
  memory.SetBudget(0);
  BaseHeap* heap = VirtualHeap4K(memory);
  const u32 before = memory.title_pages();
  u32 address = 0;
  REQUIRE(heap->Alloc(0x10000, 4096, rex::runtime::kMemoryAllocationReserve, kRW, false, &address));
  CHECK(memory.title_pages() == before);
  const u32 commit = rex::runtime::kMemoryAllocationCommit | rex::runtime::kMemoryAllocationTitle;
  REQUIRE(heap->AllocFixed(address, 0x2000, 4096, commit, kRW));
  CHECK(memory.title_pages() == before + 2);
  REQUIRE(heap->AllocFixed(address, 0x4000, 4096, commit, kRW));
  CHECK(memory.title_pages() == before + 4);
  REQUIRE(heap->Decommit(address, 0x1000));
  CHECK(memory.title_pages() == before + 3);
  REQUIRE(heap->Release(address));
  CHECK(memory.title_pages() == before);
}

TEST_CASE("A physical commit counts its parent pages", "[memory][budget]") {
  Memory& memory = GetTestMemory();
  memory.SetBudget(0);
  BaseHeap* alias = memory.LookupHeapByType(true, 0x10000);
  const u32 before = memory.title_pages();
  u32 address = 0;
  REQUIRE(alias->Alloc(0x10000, 0x10000, kTitle, kRW, true, &address));
  CHECK(memory.title_pages() == before + 16);
  REQUIRE(alias->Release(address));
  CHECK(memory.title_pages() == before);
}

TEST_CASE("2 threads racing at the budget pass at most once", "[memory][budget]") {
  Memory& memory = GetTestMemory();
  for (int round = 0; round < 50; ++round) {
    const u32 filler = RoomAfter(memory, 16);
    std::latch start(2);
    std::atomic<u32> addresses[2] = {0, 0};
    auto attempt = [&](int index) {
      start.arrive_and_wait();
      u32 address = 0;
      if (VirtualHeap4K(memory)->Alloc(0x10000, 4096, kTitle, kRW, false, &address)) {
        addresses[index] = address;
      }
    };
    std::thread first(attempt, 0);
    std::thread second(attempt, 1);
    first.join();
    second.join();
    const int passed = (addresses[0] != 0) + (addresses[1] != 0);
    CHECK(passed == 1);
    for (auto& address : addresses) {
      if (address) {
        VirtualHeap4K(memory)->Release(address);
      }
    }
    ReleaseFiller(memory, filler);
  }
}

TEST_CASE("A budget of 0 removes the cap", "[memory][budget]") {
  Memory& memory = GetTestMemory();
  const u32 filler = RoomAfter(memory, 0);
  u32 address = 0;
  CHECK_FALSE(VirtualHeap4K(memory)->Alloc(0x10000, 4096, kTitle, kRW, false, &address));
  memory.SetBudget(0);
  CHECK(memory.budget_pages() == 0);
  REQUIRE(VirtualHeap4K(memory)->Alloc(0x10000, 4096, kTitle, kRW, false, &address));
  REQUIRE(VirtualHeap4K(memory)->Release(address));
  ReleaseFiller(memory, filler);
}

TEST_CASE("A failed alias pin returns the parent pages", "[memory][budget]") {
  Memory& memory = GetTestMemory();
  memory.SetBudget(0);
  auto* alias = static_cast<PhysicalHeap*>(memory.LookupHeapByType(true, 0x10000));
  BaseHeap* parent = memory.GetPhysicalHeap();
  const u32 reserve = rex::runtime::kMemoryAllocationReserve;
  u32 address = 0;
  REQUIRE(alias->Alloc(0x10000, 0x10000, reserve, kRW, true, &address));
  const u32 physical = alias->GetPhysicalAddress(address);
  REQUIRE(parent->Release(physical));
  HeapAllocationInfo info{};

  CHECK_FALSE(alias->AllocFixed(address, 0x10000, 0x10000, reserve, kRW));
  REQUIRE(parent->QueryRegionInfo(physical, &info));
  CHECK(info.state == 0);

  u32 ranged = 0;
  CHECK_FALSE(
      alias->AllocRange(address, address + 0xFFFF, 0x10000, 0x10000, reserve, kRW, false, &ranged));
  REQUIRE(parent->QueryRegionInfo(physical, &info));
  CHECK(info.state == 0);

  REQUIRE(parent->AllocFixed(physical, 0x10000, 0x10000, reserve, kRW));
  REQUIRE(alias->Release(address));
}

TEST_CASE("A fixed physical allocation over a partly used range fails", "[memory][budget]") {
  Memory& memory = GetTestMemory();
  memory.SetBudget(0);
  auto* alias = static_cast<PhysicalHeap*>(memory.LookupHeapByType(true, 0x1000));
  BaseHeap* parent = memory.GetPhysicalHeap();
  const u32 reserve = rex::runtime::kMemoryAllocationReserve;
  u32 address = 0;
  REQUIRE(alias->Alloc(0x2000, 0x1000, reserve, kRW, true, &address));
  const u32 physical = alias->GetPhysicalAddress(address);
  REQUIRE(alias->Release(address));
  REQUIRE(parent->AllocFixed(physical, 0x1000, 0x1000, reserve, kRW));
  const u32 before = memory.title_pages();

  CHECK_FALSE(alias->AllocFixed(address, 0x2000, 0x1000, reserve, kRW));
  HeapAllocationInfo info{};
  REQUIRE(parent->QueryRegionInfo(physical + 0x1000, &info));
  CHECK(info.state == 0);
  REQUIRE(parent->QueryRegionInfo(physical, &info));
  CHECK(info.region_size == 0x1000);
  CHECK(memory.title_pages() == before);

  REQUIRE(parent->Release(physical));
}

namespace {

u32 CommitVirtual(Caller& c, u32 size, u32* out_status) {
  const u32 base_ptr = c.Alloc(4);
  const u32 size_ptr = c.Alloc(4);
  c.Store32(size_ptr, size);
  *out_status = c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, 0x3000, 0x04, 0});
  return c.Load32(base_ptr);
}

void FreeVirtual(Caller& c, u32 address) {
  const u32 base_ptr = c.Alloc(4);
  const u32 size_ptr = c.Alloc(4);
  c.Store32(base_ptr, address);
  CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, 0x8000, 0}) == 0);
}

u32 AvailablePages(Caller& c) {
  const u32 stats = c.Alloc(104);
  c.Store32(stats, 104);
  REQUIRE(c.Kernel("MmQueryStatistics", {stats}) == 0);
  CHECK(c.Load32(stats + 4) == 0x20000);
  CHECK(c.Load32(stats + 8) == 0x2000);
  return c.Load32(stats + 12);
}

}  // namespace

TEST_CASE("NtAllocateVirtualMemory counts its commit and fails past the budget",
          "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    const u32 before = memory.title_pages();
    u32 status = 0;
    const u32 address = CommitVirtual(c, 0x100000, &status);
    REQUIRE(status == 0);
    CHECK(memory.title_pages() == before + 256);
    memory.SetBudget(memory.title_pages() / 256);
    CommitVirtual(c, 0x100000, &status);
    CHECK(status == 0xC0000017);
    FreeVirtual(c, address);
    CHECK(memory.title_pages() == before);
    memory.SetBudget(480);
  });
}

TEST_CASE("MmAllocatePhysicalMemoryEx counts the parent pages", "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    const u32 before = memory.title_pages();
    const u32 address =
        c.Kernel("MmAllocatePhysicalMemoryEx", {0, 0x10000, 0x04, 0, 0xFFFFFFFF, 0});
    REQUIRE(address != 0);
    CHECK(memory.title_pages() == before + 16);
    c.Kernel("MmFreePhysicalMemory", {0, address});
    CHECK(memory.title_pages() == before);
  });
}

TEST_CASE("MmQueryStatistics available pages track the title pages", "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    const u32 first = AvailablePages(c);
    CHECK(first == memory.budget_pages() - memory.title_pages());
    u32 status = 0;
    const u32 address = CommitVirtual(c, 0x100000, &status);
    REQUIRE(status == 0);
    CHECK(AvailablePages(c) == first - 256);
    FreeVirtual(c, address);
    CHECK(AvailablePages(c) == first);
  });
}

TEST_CASE("MmQueryStatistics without a cap reports the free physical pages",
          "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    memory.SetBudget(0);
    const u32 first = AvailablePages(c);
    const u32 address =
        c.Kernel("MmAllocatePhysicalMemoryEx", {0, 0x10000, 0x04, 0, 0xFFFFFFFF, 0});
    REQUIRE(address != 0);
    CHECK(AvailablePages(c) == first - 16);
    c.Kernel("MmFreePhysicalMemory", {0, address});
    CHECK(AvailablePages(c) == first);
    memory.SetBudget(480);
  });
}

TEST_CASE("SetBudget saturates instead of wrapping", "[memory][budget]") {
  KernelFixture fixture;
  Memory& memory = *fixture.runtime().memory();
  memory.SetBudget(0x01000000);
  CHECK(memory.budget_pages() == UINT32_MAX);
  memory.SetBudget(480);
  CHECK(memory.budget_pages() == 480 * 256);
}

TEST_CASE("Loading an XEX keeps the other allocations in its heap", "[memory][budget][kernel]") {
  KernelFixture fixture;
  rex::Runtime& runtime = fixture.runtime();
  Memory& memory = *runtime.memory();
  BaseHeap* heap = memory.LookupHeap(0x82000000);
  REQUIRE(heap == memory.LookupHeap(0x80000000));
  REQUIRE(heap == memory.LookupHeap(0x8F100000));
  HeapAllocationInfo startup{};
  HeapAllocationInfo table{};
  REQUIRE(heap->QueryRegionInfo(0x80000000, &startup));
  REQUIRE(heap->QueryRegionInfo(0x8F100000, &table));
  REQUIRE(startup.state != 0);
  REQUIRE(table.state != 0);

  rex::test::XEXSpec spec;
  spec.image = rex::test::TestImage(0x1000);
  const auto file = rex::test::BuildXEX(spec);
  const u32 before = memory.title_pages();
  rex::runtime::XexModule module(runtime.function_dispatcher(), runtime);
  REQUIRE(module.Load("budget", "game:\budget.xex", file.data(), file.size()));

  HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(0x80000000, &info));
  CHECK(info.state == startup.state);
  REQUIRE(heap->QueryRegionInfo(0x8F100000, &info));
  CHECK(info.state == table.state);
  REQUIRE(heap->QueryRegionInfo(0x82000000, &info));
  CHECK(memory.title_pages() == before + info.allocation_size / 4096);
  REQUIRE(heap->Release(0x82000000));
  CHECK(memory.title_pages() == before);
}

namespace {

u32 DecommitVirtual(Caller& c, u32 address, u32 size, u32* out_address, u32* out_size) {
  const u32 base_ptr = c.Alloc(4);
  const u32 size_ptr = c.Alloc(4);
  c.Store32(base_ptr, address);
  c.Store32(size_ptr, size);
  const u32 status = c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, 0x4000, 0});
  *out_address = c.Load32(base_ptr);
  *out_size = c.Load32(size_ptr);
  return status;
}

u32 PageState(BaseHeap* heap, u32 address) {
  HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(address, &info));
  return info.state;
}

}  // namespace

TEST_CASE("A decommit of size 0 at a region base decommits only that region",
          "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    BaseHeap* heap = memory.LookupHeap(0x40000000);
    REQUIRE(PageState(heap, 0x40000000) == 0);
    REQUIRE(PageState(heap, 0x40100000) == 0);
    REQUIRE(heap->AllocFixed(0x40000000, 0x20000, 0x10000, kTitle, kRW));
    REQUIRE(heap->AllocFixed(0x40100000, 0x10000, 0x10000, kTitle, kRW));
    const u32 before = memory.title_pages();

    u32 address = 0;
    u32 size = 0;
    CHECK(DecommitVirtual(c, 0x40000000, 0, &address, &size) == 0);
    CHECK(address == 0x40000000);
    CHECK(size == 0x20000);
    CHECK(PageState(heap, 0x40000000) == rex::runtime::kMemoryAllocationReserve);
    CHECK(PageState(heap, 0x40010000) == rex::runtime::kMemoryAllocationReserve);
    CHECK(PageState(heap, 0x40100000) == kCommit);
    CHECK(memory.title_pages() == before - 32);

    REQUIRE(heap->Release(0x40000000));
    REQUIRE(heap->Release(0x40100000));
  });
}

TEST_CASE("A decommit of size 0 inside a region fails and changes nothing",
          "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    BaseHeap* heap = VirtualHeap4K(memory);
    u32 region = 0;
    REQUIRE(heap->Alloc(0x3000, 4096, kTitle, kRW, false, &region));
    const u32 before = memory.title_pages();

    u32 address = 0;
    u32 size = 0;
    CHECK(DecommitVirtual(c, region + 0x1000, 0, &address, &size) != 0);
    CHECK(PageState(heap, region) == kCommit);
    CHECK(PageState(heap, region + 0x1000) == kCommit);
    CHECK(PageState(heap, region + 0x2000) == kCommit);
    CHECK(memory.title_pages() == before);

    REQUIRE(heap->Release(region));
  });
}

TEST_CASE("A decommit range that crosses a page boundary decommits both pages",
          "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    BaseHeap* heap = VirtualHeap4K(memory);
    u32 region = 0;
    REQUIRE(heap->Alloc(0x3000, 4096, kTitle, kRW, false, &region));
    const u32 before = memory.title_pages();

    u32 address = 0;
    u32 size = 0;
    CHECK(DecommitVirtual(c, region + 0xFFF, 2, &address, &size) == 0);
    CHECK(address == region);
    CHECK(size == 0x2000);
    CHECK(PageState(heap, region) == rex::runtime::kMemoryAllocationReserve);
    CHECK(PageState(heap, region + 0x1000) == rex::runtime::kMemoryAllocationReserve);
    CHECK(PageState(heap, region + 0x2000) == kCommit);
    CHECK(memory.title_pages() == before - 2);

    REQUIRE(heap->Release(region));
  });
}

namespace {

X_STATUS SetupWithBudget(u32 megabytes, u32* out_budget_pages) {
  ReleaseTestMemory();
  rex::Config config;
  config.modules = {"rexfilesystem", "rexruntime"};
  config.runtime.title_memory_mb = megabytes;
  auto api = rex::Init(config);
  REQUIRE(api);
  X_STATUS status = X_STATUS_UNSUCCESSFUL;
  {
    rex::Runtime runtime((*api)->config());
    rex::RuntimeConfig setup;
    setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
    setup.kernel_init = rex::kernel::InitializeKernel;
    setup.tool_mode = true;
    const rex::Result<void> result = runtime.Setup(setup);
    status = result ? X_STATUS_SUCCESS : rex::runtime::ToXStatus(result.error());
    *out_budget_pages = result ? runtime.memory()->budget_pages() : 0;
    runtime.Shutdown();
  }
  rex::Shutdown();
  return status;
}

}  // namespace

TEST_CASE("Setup rejects a title memory budget above 512 MB", "[memory][budget][runtime]") {
  u32 budget_pages = 1;
  CHECK(SetupWithBudget(513, &budget_pages) == X_STATUS_INVALID_PARAMETER);
  CHECK(SetupWithBudget(512, &budget_pages) == X_STATUS_SUCCESS);
  CHECK(budget_pages == 512 * 256);
  CHECK(SetupWithBudget(0, &budget_pages) == X_STATUS_SUCCESS);
  CHECK(budget_pages == 0);
}

TEST_CASE("A release of the free first page of a heap fails and changes nothing",
          "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    BaseHeap* heap = memory.LookupHeap(0x40000000);
    REQUIRE(PageState(heap, 0x40000000) == 0);
    const u32 unreserved = heap->GetUnreservedPageCount();
    const u32 before = memory.title_pages();

    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    c.Store32(base_ptr, 0x40000000);
    CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, 0x8000, 0}) != 0);
    CHECK(PageState(heap, 0x40000000) == 0);
    CHECK(heap->GetUnreservedPageCount() == unreserved);
    CHECK(memory.title_pages() == before);
  });
}

TEST_CASE("A release with a nonzero size fails and changes nothing", "[memory][budget][kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    Memory& memory = *c.runtime().memory();
    BaseHeap* heap = VirtualHeap4K(memory);
    u32 region = 0;
    REQUIRE(heap->Alloc(0x3000, 4096, kTitle, kRW, false, &region));
    const u32 before = memory.title_pages();

    const u32 base_ptr = c.Alloc(4);
    const u32 size_ptr = c.Alloc(4);
    c.Store32(base_ptr, region);
    c.Store32(size_ptr, 0x3000);
    CHECK(c.Kernel("NtFreeVirtualMemory", {base_ptr, size_ptr, 0x8000, 0}) == 0xC000000D);
    CHECK(PageState(heap, region) == kCommit);
    CHECK(memory.title_pages() == before);

    REQUIRE(heap->Release(region));
  });
}
