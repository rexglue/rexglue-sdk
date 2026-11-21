/**
 * @file        tests/unit/kernel/kernel_pool_test.cpp
 * @brief       The kernel pool and what it counts against the title budget
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <vector>

#include <rex/kernel/heap.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/object_block.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel/kernel_fixture.h"
#include "kernel/kernel_pool.h"
#include "kernel/title_state.h"
#include "test_memory.h"

using rex::u32;
using rex::kernel::GuestAddressSpace;
using rex::kernel::KernelPool;
using rex::test::Caller;
using rex::test::GetTestMemory;
using rex::test::KernelFixture;

namespace {

constexpr u32 kTag = 0x54657374;
constexpr u32 kTitlePool = 1;
constexpr u32 kSystemPool = 2;

}  // namespace

REX_TEST_CASE("A pool page holds 127 blocks of 1 unit and the next block takes a second page",
              "[kernel][pool]") {
  GuestAddressSpace& memory = GetTestMemory();
  memory.SetBudget(0);
  KernelPool pool(memory);
  const u32 before = memory.GetTitlePages();
  std::vector<u32> blocks;
  for (int i = 0; i < 127; ++i) {
    blocks.push_back(pool.Allocate(24, kTag, kTitlePool));
    REQUIRE(blocks.back() != 0);
  }
  CHECK(memory.GetTitlePages() == before + 1);
  blocks.push_back(pool.Allocate(24, kTag, kTitlePool));
  REQUIRE(blocks.back() != 0);
  CHECK(memory.GetTitlePages() == before + 2);
  for (const u32 block : blocks) {
    CHECK(pool.Free(block));
  }
}

REX_TEST_CASE("Freeing every block of a page returns the page and freeing all but one does not",
              "[kernel][pool]") {
  GuestAddressSpace& memory = GetTestMemory();
  memory.SetBudget(0);
  KernelPool pool(memory);
  const u32 before = memory.GetTitlePages();
  std::vector<u32> blocks;
  for (int i = 0; i < 9; ++i) {
    blocks.push_back(pool.Allocate(400, kTag, kTitlePool));
    REQUIRE(blocks.back() != 0);
  }
  CHECK(memory.GetTitlePages() == before + 1);
  for (size_t i = 0; i + 1 < blocks.size(); ++i) {
    CHECK(pool.Free(blocks[i]));
  }
  CHECK(memory.GetTitlePages() == before + 1);
  CHECK(pool.Free(blocks.back()));
  CHECK(memory.GetTitlePages() == before);
}

REX_TEST_CASE("A small block returns to its lookaside list and keeps its page", "[kernel][pool]") {
  GuestAddressSpace& memory = GetTestMemory();
  memory.SetBudget(0);
  KernelPool pool(memory);
  const u32 before = memory.GetTitlePages();
  const u32 first = pool.Allocate(24, kTag, kTitlePool);
  REQUIRE(first != 0);
  CHECK(pool.Free(first));
  CHECK(memory.GetTitlePages() == before + 1);
  CHECK(pool.Allocate(24, kTag, kTitlePool) == first);
  CHECK(pool.Free(first));
}

REX_TEST_CASE("A block freed from a host thread returns to the title pool", "[kernel][pool]") {
  GuestAddressSpace& memory = GetTestMemory();
  memory.SetBudget(0);
  KernelPool pool(memory);
  const u32 before = memory.GetTitlePages();
  const u32 block = pool.Allocate(400, kTag, kTitlePool);
  REQUIRE(block != 0);
  CHECK(memory.GetTitlePages() == before + 1);
  CHECK(pool.Free(block));
  CHECK(memory.GetTitlePages() == before);
}

REX_TEST_CASE("A system pool block counts nothing and an unknown type returns 0",
              "[kernel][pool]") {
  GuestAddressSpace& memory = GetTestMemory();
  memory.SetBudget(0);
  KernelPool pool(memory);
  const u32 before = memory.GetTitlePages();
  const u32 block = pool.Allocate(400, kTag, kSystemPool);
  REQUIRE(block != 0);
  CHECK(memory.GetTitlePages() == before);
  CHECK(pool.Free(block));
  CHECK(pool.Allocate(24, kTag, 3) == 0);
  CHECK(pool.Allocate(24, kTag, 4) == 0);
  CHECK(pool.Allocate(24, kTag, 7) == 0);
}

REX_TEST_CASE("A block above 0xFD8 counts its pages and returns them", "[kernel][pool]") {
  GuestAddressSpace& memory = GetTestMemory();
  memory.SetBudget(0);
  KernelPool pool(memory);
  const u32 before = memory.GetTitlePages();
  const u32 block = pool.Allocate(0xFD9, kTag, kTitlePool);
  REQUIRE(block != 0);
  CHECK(block % 0x1000 == 0);
  CHECK(memory.GetTitlePages() == before + 1);
  CHECK(pool.Free(block));
  CHECK(memory.GetTitlePages() == before);
  const u32 large = pool.Allocate(0x5000, kTag, kTitlePool);
  REQUIRE(large != 0);
  CHECK(memory.GetTitlePages() == before + 5);
  CHECK(pool.Free(large));
  CHECK(memory.GetTitlePages() == before);
}

REX_TEST_CASE("The pool writes the console block header", "[kernel][pool]") {
  GuestAddressSpace& memory = GetTestMemory();
  memory.SetBudget(0);
  KernelPool pool(memory);
  const u32 block = pool.Allocate(100, kTag, kSystemPool);
  REQUIRE(block != 0);
  const auto* header = memory.TranslateVirtual<const rex::kernel::PoolBlockHeader*>(block - 8);
  CHECK(header->units == 4);
  CHECK(header->previous_units == 0);
  CHECK(header->state == 0xAA);
  CHECK(static_cast<u32>(header->tag) == kTag);
  const u32 second = pool.Allocate(100, kTag, kSystemPool);
  REQUIRE(second != 0);
  CHECK(second > block);
  CHECK(pool.Free(block));
  CHECK(pool.Free(second));
}

REX_TEST_CASE("Pool type 0 on a title thread counts, type 2 does not", "[kernel][pool]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    GuestAddressSpace& memory = *StateOf(c.GetRuntime()).GetAddressSpace();
    const u32 before = memory.GetTitlePages();
    const u32 caller = c.Kernel("ExAllocatePoolTypeWithTag", {0x2000, kTag, 0});
    REQUIRE(caller != 0);
    CHECK(memory.GetTitlePages() == before + 2);
    c.Kernel("ExFreePool", {caller});
    CHECK(memory.GetTitlePages() == before);
    const u32 system = c.Kernel("ExAllocatePoolTypeWithTag", {0x2000, kTag, 2});
    REQUIRE(system != 0);
    CHECK(memory.GetTitlePages() == before);
    c.Kernel("ExFreePool", {system});
    CHECK(c.Kernel("ExAllocatePoolTypeWithTag", {24, kTag, 3}) == 0);
    CHECK(c.Kernel("ExAllocatePoolTypeWithTag", {24, kTag, 4}) == 0);
    CHECK(c.Kernel("ExAllocatePoolTypeWithTag", {24, kTag, 7}) == 0);
  });
}

REX_TEST_CASE(
    "Events from a title thread share pool pages and a system flagged block counts nothing",
    "[kernel][pool]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    GuestAddressSpace& memory = *StateOf(c.GetRuntime()).GetAddressSpace();
    const u32 before = memory.GetTitlePages();
    std::vector<u32> handles;
    const u32 handle_ptr = c.Alloc(4);
    for (int i = 0; i < 10; ++i) {
      REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0);
      handles.push_back(c.Load32(handle_ptr));
    }
    CHECK(memory.GetTitlePages() <= before + 1);
    for (const u32 handle : handles) {
      CHECK(c.Kernel("NtClose", {handle}) == 0);
    }

    const u32 type = StateOf(c.GetRuntime()).GetObjectTypeAddress(rex::kernel::ObjectType::kEvent);
    const u32 settled = memory.GetTitlePages();
    rex::kernel::ObjectBlock block = rex::kernel::AllocObjectBlock(
        &memory, 0x40, type, nullptr, rex::kernel::ObjectBlockOptions{.attributes = 0x2000});
    REQUIRE(block.address != 0);
    CHECK(memory.GetTitlePages() == settled);
    rex::kernel::FreeObjectBlock(&memory, block.address);
  });
}

REX_TEST_CASE("A named object block takes the console size and a thread block moves up by 8",
              "[kernel][pool]") {
  KernelFixture fixture;
  GuestAddressSpace& memory = *StateOf(fixture.GetRuntime()).GetAddressSpace();
  const u32 type =
      StateOf(fixture.GetRuntime()).GetObjectTypeAddress(rex::kernel::ObjectType::kEvent);
  const auto units_of = [&](u32 body) {
    return memory.TranslateVirtual<const rex::kernel::PoolBlockHeader*>(body - 16 - 8)->units;
  };
  rex::kernel::ObjectBlock unnamed = rex::kernel::AllocObjectBlock(
      &memory, 0x10, type, nullptr, rex::kernel::ObjectBlockOptions{.attributes = 0x2000});
  rex::kernel::ObjectBlock named = rex::kernel::AllocObjectBlock(
      &memory, 0x10, type, nullptr,
      rex::kernel::ObjectBlockOptions{.attributes = 0x2000, .name_length = 20});
  REQUIRE(unnamed.address != 0);
  REQUIRE(named.address != 0);
  CHECK(units_of(unnamed.address) == 2);
  CHECK(units_of(named.address) == 3);
  rex::kernel::FreeObjectBlock(&memory, unnamed.address);
  rex::kernel::FreeObjectBlock(&memory, named.address);

  rex::kernel::ObjectBlock thread = rex::kernel::AllocObjectBlock(
      &memory, 0x10, type, nullptr,
      rex::kernel::ObjectBlockOptions{.attributes = 0x2000, .thread = true});
  REQUIRE(thread.address != 0);
  const u32 block = thread.address - 16;
  CHECK(block % 32 == 16);
  CHECK(memory.GetPool().Owns(block - 8));
  *memory.TranslateVirtual<u8*>(block - 6) = 0xAA;
  rex::kernel::FreeObjectBlock(&memory, thread.address);
  CHECK_FALSE(memory.GetPool().Owns(block - 8));
}

namespace {

/** An export that does nothing, for the baseline of the call path. */
REX_FUNC(NoOp) {}

}  // namespace

REX_TEST_CASE("Pool allocate and free of 64 bytes", "[kernel][pool][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    const auto allocate = Caller::Resolve("ExAllocatePoolTypeWithTag");
    const auto release = Caller::Resolve("ExFreePool");
    const auto nothing = Caller::Resolve(&NoOp);
    BENCHMARK("baseline: 2 empty calls through the same path") {
      nothing(c, {64, kTag, kTitlePool});
      return nothing(c, {0});
    };
    BENCHMARK("allocate and free 64 bytes") {
      const u32 block = allocate(c, {64, kTag, kTitlePool});
      release(c, {block});
      return block;
    };
  });
}

REX_TEST_CASE("Pool allocate 1000 live blocks of 64 bytes and free them",
              "[kernel][pool][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    const auto allocate = Caller::Resolve("ExAllocatePoolTypeWithTag");
    const auto release = Caller::Resolve("ExFreePool");
    const auto nothing = Caller::Resolve(&NoOp);
    std::vector<u32> blocks(1000);
    BENCHMARK("baseline: 2000 empty calls through the same path") {
      for (u32& block : blocks) {
        block = nothing(c, {64, kTag, kTitlePool});
      }
      for (const u32 block : blocks) {
        nothing(c, {block});
      }
      return blocks.back();
    };
    BENCHMARK("allocate 1000 blocks, then free 1000 blocks") {
      for (u32& block : blocks) {
        block = allocate(c, {64, kTag, kTitlePool});
      }
      for (const u32 block : blocks) {
        release(c, {block});
      }
      return blocks.back();
    };
  });
}
