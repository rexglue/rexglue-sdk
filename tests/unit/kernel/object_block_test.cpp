/**
 * @file        tests/unit/kernel/object_block_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/kernel/object/object_block.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/heap.h>

#include "kernel/kernel_fixture.h"
#include "kernel/title_state.h"

using rex::test::KernelFixture;

REX_TEST_CASE("An object block keeps its type 8 bytes before the body", "[kernel][object]") {
  KernelFixture fixture;
  auto* memory = StateOf(fixture.GetRuntime()).GetMemory();
  const auto block = rex::kernel::AllocObjectBlock(memory, 0x10);
  REQUIRE(block.address != 0);
  const u32 header_address = block.address - static_cast<u32>(sizeof(rex::kernel::X_OBJECT_HEADER));
  const auto* header =
      memory->TranslateVirtual<const rex::kernel::X_OBJECT_HEADER*>(header_address);
  const u32 type = *memory->TranslateVirtual<const be_u32*>(block.address - 8);
  CHECK(type == header->object_type_ptr);
  CHECK(header->pointer_count == 0);
  CHECK(header->handle_count == 0);
  CHECK(header->flags == 0);
  CHECK(header->hash_index == 0);
  CHECK((type < header_address || type >= block.address + 0x10));
  rex::kernel::BaseHeap* heap = memory->LookupHeap(type);
  REQUIRE(heap != nullptr);
  rex::kernel::HeapAllocation info{};
  REQUIRE(heap->QueryRegionInfo(type, &info));
  CHECK((info.state & rex::kernel::kMemoryAllocationCommit) != 0);
  CHECK(info.region_size >= sizeof(rex::kernel::X_OBJECT_TYPE));
  rex::kernel::FreeObjectBlock(memory, block.address);
  const bool queried = heap->QueryRegionInfo(type, &info);
  CHECK((!queried || (info.state & rex::kernel::kMemoryAllocationCommit) == 0));
}
