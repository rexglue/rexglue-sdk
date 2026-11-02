/**
 * @file        tests/unit/kernel/guest_stack_range_test.cpp
 * @brief       Guest stacks stay below the physical alias range
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/kernel/object/kernel_thread.h>

#include "kernel/guest_stack.h"
#include "test_memory.h"

namespace {

constexpr rex::u32 kPhysicalAliasBase = 0xE0000000;
constexpr rex::u32 kStackSize = 0x10000;

}  // namespace

REX_TEST_CASE("The guest stack range ends below the physical alias range", "[kernel][stack]") {
  CHECK(rex::kernel::KernelThread::kStackAddressRangeBegin <
        rex::kernel::KernelThread::kStackAddressRangeEnd);
  CHECK(rex::kernel::KernelThread::kStackAddressRangeEnd < kPhysicalAliasBase);
}

REX_TEST_CASE("A thread stack lies inside the stack range", "[kernel][stack]") {
  auto& memory = rex::test::GetTestMemory();
  auto stack = rex::kernel::AllocateGuestStack(memory, kStackSize, 0, true);
  REQUIRE(stack);
  CHECK(stack->alloc_base >= rex::kernel::KernelThread::kStackAddressRangeBegin);
  CHECK(stack->base < rex::kernel::KernelThread::kStackAddressRangeEnd);
  CHECK(stack->base < kPhysicalAliasBase);
  rex::kernel::FreeGuestStack(memory, stack->alloc_base);
}

REX_TEST_CASE("A stack made for deferred procedures lies inside the stack range",
              "[kernel][stack]") {
  auto& memory = rex::test::GetTestMemory();
  auto stack = rex::kernel::AllocateGuestStack(memory, kStackSize, 0, false);
  REQUIRE(stack);
  CHECK(stack->base < rex::kernel::KernelThread::kStackAddressRangeEnd);
  rex::kernel::FreeGuestStack(memory, stack->alloc_base);
}

REX_TEST_CASE("A fiber stack with thread storage lies inside the stack range", "[kernel][stack]") {
  auto& memory = rex::test::GetTestMemory();
  auto stack = rex::kernel::AllocateGuestStack(memory, kStackSize, 0x400, true);
  REQUIRE(stack);
  CHECK(stack->base < rex::kernel::KernelThread::kStackAddressRangeEnd);
  rex::kernel::FreeGuestStack(memory, stack->alloc_base);
}
