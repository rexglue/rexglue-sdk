/**
 * @file        tests/unit/kernel/char_workspace_test.cpp
 * @brief       Title workspace and the Xam allocations it serves
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <rex/runtime/workspace_heap.h>
#include <rex/runtime/xmemory.h>

#include "kernel_fixture.h"

using rex::u32;
using rex::runtime::WorkspaceHeap;
using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

constexpr u32 kNotEnoughMemory = 8;

std::vector<u32> Fill(Caller& c, u32 size) {
  const u32 out = c.Alloc(4);
  std::vector<u32> blocks;
  while (c.XAM("XamAlloc", {0, size, out}) == 0) {
    blocks.push_back(c.Load32(out));
    REQUIRE(blocks.size() <= 0x60000 / 16);
  }
  return blocks;
}

void FreeAll(Caller& c, const std::vector<u32>& blocks) {
  for (const u32 block : blocks) {
    c.XAM("XamFree", {block});
  }
}

}  // namespace

TEST_CASE("ResolveSize keeps 384 KB by default, at least 256 KB, in 16 KB steps",
          "[char][workspace]") {
  CHECK(WorkspaceHeap::ResolveSize(0) == 0x60000);
  CHECK(WorkspaceHeap::ResolveSize(0x10000) == 0x40000);
  CHECK(WorkspaceHeap::ResolveSize(0x40001) == 0x44000);
  CHECK(WorkspaceHeap::ResolveSize(0x80000) == 0x80000);
}

TEST_CASE(
    "The tool runtime has a workspace at the default size and workspaces count against the budget",
    "[char][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& memory = *c.runtime().memory();
    CHECK(c.runtime().workspace().size() == 0x60000);
    const u32 used = memory.title_pages();
    {
      auto second = WorkspaceHeap::Create(memory, 0x40000);
      REQUIRE(second);
      CHECK((*second)->size() == 0x40000);
      CHECK(memory.title_pages() == used + 64);
    }
    CHECK(memory.title_pages() == used);
  });
}

TEST_CASE("XamAlloc returns 8 and a 0 pointer on a full workspace and XamFree makes room",
          "[char][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u32> blocks = Fill(c, 0x1000);
    CHECK(blocks.size() >= 16);
    const u32 out = c.Alloc(4);
    c.Store32(out, 0xFFFFFFFF);
    CHECK(c.XAM("XamAlloc", {0, 0x1000, out}) == kNotEnoughMemory);
    CHECK(c.Load32(out) == 0);
    for (const u32 block : blocks) {
      CHECK(c.XAM("XamFree", {block}) == 0);
    }
    CHECK(c.XAM("XamAlloc", {0, 0x1000, out}) == 0);
    CHECK(c.Load32(out) != 0);
    CHECK(c.Load32(c.Load32(out)) == 0);
    c.XAM("XamFree", {c.Load32(out)});
  });
}

TEST_CASE("XamFree of an address outside the workspace is ignored", "[char][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 outside = c.Alloc(16);
    CHECK(c.XAM("XamFree", {outside}) == 0);
    CHECK(c.XAM("XamFree", {0}) == 0);
    const std::vector<u32> blocks = Fill(c, 0x1000);
    CHECK(blocks.size() >= 16);
    FreeAll(c, blocks);
  });
}

TEST_CASE("A 128 KB XamAlloc fits the default workspace and a second one does not",
          "[char][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(4);
    REQUIRE(c.XAM("XamAlloc", {0, 0x20000, out}) == 0);
    const u32 first = c.Load32(out);
    CHECK(first != 0);
    CHECK(c.XAM("XamAlloc", {0, 0x20000, out}) == kNotEnoughMemory);
    CHECK(c.XAM("XamFree", {first}) == 0);
    CHECK(c.XAM("XamAlloc", {0, 0x20000, out}) == 0);
    c.XAM("XamFree", {c.Load32(out)});
  });
}

TEST_CASE("An enumerator on a full workspace returns 8", "[char][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_out = c.Alloc(4);
    std::vector<u32> blocks = Fill(c, 0x1000);
    const std::vector<u32> small = Fill(c, 0x10);
    blocks.insert(blocks.end(), small.begin(), small.end());
    CHECK(c.XAM("XamContentCreateDeviceEnumerator", {1, 0, 2, 0, handle_out}) == kNotEnoughMemory);
    FreeAll(c, blocks);
    REQUIRE(c.XAM("XamContentCreateDeviceEnumerator", {1, 0, 2, 0, handle_out}) == 0);
    const u32 handle = c.Load32(handle_out);
    const u32 address = c.Alloc(4);
    REQUIRE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, address}) == 0);
    const u32 object = c.Load32(address);
    c.Kernel("ObDereferenceObject", {object});
    CHECK(object >= c.runtime().memory()->LookupHeapByType(false, 4096)->heap_base());
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    const std::vector<u32> again = Fill(c, 0x1000);
    CHECK(again.size() >= 16);
    FreeAll(c, again);
  });
}
