/**
 * @file        tests/unit/kernel/best_fit_range_allocator_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>

#include "kernel/best_fit_range_allocator.h"

using rex::kernel::BestFitRangeAllocator;

namespace {

constexpr u32 kRange = 0x60000;
constexpr u32 kBlock = 0x20000;

}  // namespace

REX_TEST_CASE("A freed block of the same size is taken again at once", "[kernel][allocator]") {
  BestFitRangeAllocator ranges(kRange);
  const auto first = ranges.Allocate(kBlock);
  const auto second = ranges.Allocate(kBlock);
  if (!first || !second) {
    FAIL("a block was not allocated");
    return;
  }
  CHECK(*first != *second);
  CHECK(ranges.Free(*first) == kBlock);
  const auto again = ranges.Allocate(kBlock);
  if (!again) {
    FAIL("a block was not allocated");
    return;
  }
  CHECK(*again == *first);
  CHECK(ranges.GetAllocated() == 2 * kBlock);
}

REX_TEST_CASE("The range holds three blocks of a third and no more", "[kernel][allocator]") {
  BestFitRangeAllocator ranges(kRange);
  CHECK(ranges.Allocate(kBlock));
  CHECK(ranges.Allocate(kBlock));
  CHECK(ranges.Allocate(kBlock));
  CHECK_FALSE(ranges.Allocate(1));
  CHECK(ranges.GetFreeRangeCount() == 0);
  CHECK(ranges.GetAllocated() == kRange);
}

REX_TEST_CASE("Three adjacent blocks join into one range in every free order",
              "[kernel][allocator]") {
  std::array<int, 3> order = {0, 1, 2};
  do {
    BestFitRangeAllocator ranges(kRange);
    std::array<u32, 3> blocks{};
    for (u32& block : blocks) {
      const auto allocated = ranges.Allocate(kBlock);
      if (!allocated) {
        FAIL("a block was not allocated");
        return;
      }
      block = *allocated;
    }
    for (const int index : order) {
      CHECK(ranges.Free(blocks[index]) == kBlock);
    }
    CHECK(ranges.GetFreeRangeCount() == 1);
    CHECK(ranges.GetBlockCount() == 0);
    CHECK(ranges.GetAllocated() == 0);
    const auto whole = ranges.Allocate(kRange);
    if (!whole) {
      FAIL("a block was not allocated");
      return;
    }
    CHECK(*whole == 0);
  } while (std::next_permutation(order.begin(), order.end()));
}

REX_TEST_CASE("Best fit takes the smaller of two holes", "[kernel][allocator]") {
  BestFitRangeAllocator ranges(0x10000);
  const auto a = ranges.Allocate(0x2000);
  const auto keep_a = ranges.Allocate(0x100);
  const auto b = ranges.Allocate(0x1000);
  const auto keep_b = ranges.Allocate(0x100);
  if (!a || !keep_a || !b || !keep_b) {
    FAIL("a block was not allocated");
    return;
  }
  CHECK(ranges.Free(*a));
  CHECK(ranges.Free(*b));
  const auto small = ranges.Allocate(0xF00);
  if (!small) {
    FAIL("a block was not allocated");
    return;
  }
  CHECK(*small == *b);
  const auto large = ranges.Allocate(0x1800);
  if (!large) {
    FAIL("a block was not allocated");
    return;
  }
  CHECK(*large == *a);
}

REX_TEST_CASE("An exhausted range returns nothing and a free makes room", "[kernel][allocator]") {
  BestFitRangeAllocator ranges(0x1000);
  const auto all = ranges.Allocate(0x1000);
  if (!all) {
    FAIL("a block was not allocated");
    return;
  }
  CHECK_FALSE(ranges.Allocate(1).has_value());
  CHECK_FALSE(ranges.Allocate(0x2000).has_value());
  CHECK(ranges.Free(*all) == 0x1000);
  CHECK(ranges.Allocate(0x1000));
}

REX_TEST_CASE("Every block is 16 byte aligned and sizes round up to 16", "[kernel][allocator]") {
  BestFitRangeAllocator ranges(0x10000);
  u32 allocated = 0;
  for (const u32 size : {1u, 0u, 15u, 16u, 17u, 100u, 4097u}) {
    const auto block = ranges.Allocate(size);
    if (!block) {
      FAIL("a block was not allocated");
      return;
    }
    CHECK(*block % 16 == 0);
    const u32 rounded = (std::max(size, 1u) + 15) & ~15u;
    allocated += rounded;
    CHECK(ranges.GetAllocated() == allocated);
  }
}

REX_TEST_CASE("Freeing an offset that is not a live block changes nothing", "[kernel][allocator]") {
  BestFitRangeAllocator ranges(0x1000);
  const auto block = ranges.Allocate(0x100);
  if (!block) {
    FAIL("a block was not allocated");
    return;
  }
  CHECK_FALSE(ranges.Free(*block + 16).has_value());
  CHECK(ranges.Free(*block));
  CHECK_FALSE(ranges.Free(*block).has_value());
  CHECK(ranges.GetAllocated() == 0);
  CHECK(ranges.GetFreeRangeCount() == 1);
}
