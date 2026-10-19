/**
 * @file        tests/unit/runtime/object_block_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/runtime/kernel_object.h>
#include <rex/runtime/object.h>

#include "kernel/kernel_fixture.h"

using rex::test::KernelFixture;

REX_TEST_CASE("An object block keeps its type 8 bytes before the body", "[runtime][object]") {
  KernelFixture fixture;
  auto* memory = fixture.GetRuntime().GetMemory();
  const auto block = rex::runtime::AllocObjectBlock(memory, 0x10);
  REQUIRE(block.address != 0);
  const auto* header = memory->TranslateVirtual<const rex::runtime::X_OBJECT_HEADER*>(
      block.address - static_cast<u32>(sizeof(rex::runtime::X_OBJECT_HEADER)));
  const u32 type = *memory->TranslateVirtual<const be_u32*>(block.address - 8);
  CHECK(type != 0);
  CHECK(type == header->object_type_ptr);
  rex::runtime::FreeObjectBlock(memory, block.address);
}
