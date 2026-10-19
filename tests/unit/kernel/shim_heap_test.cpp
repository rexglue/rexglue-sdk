/**
 * @file        tests/unit/kernel/shim_heap_test.cpp
 * @brief       The heap behind the shim Rtl heap routines
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/kernel/shim/heap.h>
#include <rex/runtime.h>

#include "kernel_fixture.h"

using rex::kernel::shim::GuestHeap;

namespace {

constexpr u32 kHeapBytes = 4 * 1024 * 1024;

}  // namespace

REX_TEST_CASE("A heap rebuilt over the same memory reuses the range it released",
              "[kernel][shim_heap]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory* memory = fixture.GetRuntime().GetMemory();
  GuestHeap heap;
  REQUIRE(heap.Init(kHeapBytes, memory));
  const u32 first = heap.Alloc(16);
  REQUIRE(first != 0);
  REQUIRE(heap.Init(kHeapBytes, memory));
  CHECK(heap.Alloc(16) == first);
}

REX_TEST_CASE("A heap rebuilt across 2 runtimes holds no stale pointers", "[kernel][shim_heap]") {
  GuestHeap heap;
  u32 first = 0;
  {
    rex::test::KernelFixture fixture;
    REQUIRE(heap.Init(kHeapBytes, fixture.GetRuntime().GetMemory()));
    first = heap.Alloc(16);
    REQUIRE(first != 0);
    heap.Shutdown();
  }
  rex::test::KernelFixture fixture;
  REQUIRE(heap.Init(kHeapBytes, fixture.GetRuntime().GetMemory()));
  CHECK(heap.Alloc(16) == first);
  heap.Shutdown();
}

REX_TEST_CASE("A block keeps its contents through Realloc and reports its size",
              "[kernel][shim_heap]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory* memory = fixture.GetRuntime().GetMemory();
  GuestHeap heap;
  REQUIRE(heap.Init(kHeapBytes, memory));
  const u32 block = heap.Alloc(32, true);
  REQUIRE(block != 0);
  CHECK(heap.GetSize(block) == 32);
  *memory->TranslateVirtual<u8*>(block) = 0x5A;
  const u32 grown = heap.Realloc(block, 4096, true);
  REQUIRE(grown != 0);
  CHECK(heap.GetSize(grown) == 4096);
  CHECK(*memory->TranslateVirtual<u8*>(grown) == 0x5A);
  heap.Free(grown);
}
