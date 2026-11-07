/**
 * @file        tests/unit/kernel/workspace_test.cpp
 * @brief       Title workspace and the XAM allocations it serves
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <vector>

#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/heap.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

using rex::u32;
using rex::kernel::WorkspaceHeap;
using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

constexpr u32 kNotEnoughMemory = 8;
constexpr u32 kOutOfMemory = 0x8007000E;
constexpr u32 kInvalidArg = 0x80070057;
constexpr u32 kUnexpected = 0x8000FFFF;

std::vector<u32> Fill(Caller& c, u32 size) {
  const u32 out = c.Alloc(4);
  std::vector<u32> blocks;
  while (c.XAM("XamAlloc", {0x10000000, size, out}) == 0) {
    blocks.push_back(c.Load32(out));
    CHECKED_ELSE(blocks.size() <= 0x60000 / 16) {
      FAIL_CHECK("the check above failed");
      return blocks;
    }
  }
  return blocks;
}

void FreeAll(Caller& c, const std::vector<u32>& blocks) {
  for (const u32 block : blocks) {
    c.XAM("XamFree", {block});
  }
}

}  // namespace

REX_TEST_CASE("ResolveSize keeps 384 KB by default, at least 256 KB, in 16 KB steps",
              "[kernel][workspace]") {
  CHECK(WorkspaceHeap::ResolveSize(0) == 0x60000);
  CHECK(WorkspaceHeap::ResolveSize(0x10000) == 0x40000);
  CHECK(WorkspaceHeap::ResolveSize(0x3FFFF) == 0x40000);
  CHECK(WorkspaceHeap::ResolveSize(0x40000) == 0x40000);
  CHECK(WorkspaceHeap::ResolveSize(0x40001) == 0x44000);
  CHECK(WorkspaceHeap::ResolveSize(0x43FFF) == 0x44000);
  CHECK(WorkspaceHeap::ResolveSize(0x44000) == 0x44000);
  CHECK(WorkspaceHeap::ResolveSize(0x44001) == 0x48000);
  CHECK(WorkspaceHeap::ResolveSize(0x80000) == 0x80000);
}

REX_TEST_CASE(
    "The tool runtime has a workspace at the default size and workspaces count against the budget",
    "[kernel][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& memory = *StateOf(c.GetRuntime()).GetAddressSpace();
    CHECK(StateOf(c.GetRuntime()).GetWorkspace().GetSize() == 0x60000);
    const u32 used = memory.GetTitlePages();
    {
      auto second = WorkspaceHeap::Create(memory, 0x40000);
      CHECKED_ELSE(second) {
        FAIL_CHECK("the check above failed");
        return;
      }
      CHECK((*second)->GetSize() == 0x40000);
      CHECK(memory.GetTitlePages() == used + 64);
    }
    CHECK(memory.GetTitlePages() == used);
  });
}

REX_TEST_CASE("XamAlloc returns 8 and a 0 pointer on a full workspace and XamFree makes room",
              "[kernel][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u32> blocks = Fill(c, 0x1000);
    CHECK(blocks.size() >= 16);
    const u32 out = c.Alloc(4);
    c.Store32(out, 0xFFFFFFFF);
    CHECK(c.XAM("XamAlloc", {0x10000000, 0x1000, out}) == kOutOfMemory);
    CHECK(c.Load32(out) == 0);
    for (const u32 block : blocks) {
      c.XAM("XamFree", {block});
    }
    CHECK(c.XAM("XamAlloc", {0x10000000, 0x1000, out}) == 0);
    CHECK(c.Load32(out) != 0);
    CHECK(c.Load32(c.Load32(out)) == 0);
    c.XAM("XamFree", {c.Load32(out)});
  });
}

REX_TEST_CASE("XamFree of an address outside the workspace is ignored", "[kernel][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 outside = c.Alloc(16);
    c.XAM("XamFree", {outside});
    c.XAM("XamFree", {0});
    const std::vector<u32> blocks = Fill(c, 0x1000);
    CHECK(blocks.size() >= 16);
    FreeAll(c, blocks);
  });
}

REX_TEST_CASE("A 128 KB XamAlloc fits the default workspace and a second one does not",
              "[kernel][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(4);
    CHECKED_ELSE(c.XAM("XamAlloc", {0x10000000, 0x20000, out}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 first = c.Load32(out);
    CHECK(first != 0);
    CHECK(c.XAM("XamAlloc", {0x10000000, 0x20000, out}) == kOutOfMemory);
    c.XAM("XamFree", {first});
    CHECK(c.XAM("XamAlloc", {0x10000000, 0x20000, out}) == 0);
    c.XAM("XamFree", {c.Load32(out)});
  });
}

REX_TEST_CASE("XamAlloc returns E_INVALIDARG for a null out pointer and E_UNEXPECTED for no heap",
              "[kernel][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.XAM("XamAlloc", {0x10000000, 0x100, 0}) == kInvalidArg);
    const u32 out = c.Alloc(4);
    c.Store32(out, 0xFFFFFFFF);
    CHECK(c.XAM("XamAlloc", {0x1, 0x100, out}) == kUnexpected);
    CHECK(c.Load32(out) == 0);
  });
}

REX_TEST_CASE("XamAlloc with flags 0 allocates from the title workspace", "[kernel][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(4);
    CHECKED_ELSE(c.XAM("XamAlloc", {0, 0x20000, out}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 block = c.Load32(out);
    CHECK(block != 0);
    CHECK(c.XAM("XamAlloc", {0x10000000, 0x20000, out}) == kOutOfMemory);
    c.XAM("XamFree", {block});
    CHECK(c.XAM("XamAlloc", {0x10000000, 0x20000, out}) == 0);
    c.XAM("XamFree", {c.Load32(out)});
  });
}

REX_TEST_CASE("An enumerator on a full workspace returns 8", "[kernel][workspace]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_out = c.Alloc(4);
    std::vector<u32> blocks = Fill(c, 0x1000);
    const std::vector<u32> small = Fill(c, 0x10);
    blocks.insert(blocks.end(), small.begin(), small.end());
    CHECK(c.XAM("XamContentCreateDeviceEnumerator", {1, 0, 2, 0, handle_out}) == kNotEnoughMemory);
    FreeAll(c, blocks);
    CHECKED_ELSE(c.XAM("XamContentCreateDeviceEnumerator", {1, 0, 2, 0, handle_out}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_out);
    const u32 address = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, address}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 object = c.Load32(address);
    c.Kernel("ObDereferenceObject", {object});
    CHECK(object >=
          StateOf(c.GetRuntime()).GetAddressSpace()->LookupHeapByType(false, 4096)->GetHeapBase());
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    const std::vector<u32> again = Fill(c, 0x1000);
    CHECK(again.size() >= 16);
    FreeAll(c, again);
  });
}
