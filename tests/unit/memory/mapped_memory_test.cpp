/**
 * @file        tests/unit/memory/mapped_memory_test.cpp
 * @brief       MappedMemory slices are non owning views
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <type_traits>

#include <rex/memory/mapped_memory.h>

using rex::memory::MappedMemory;

static_assert(std::is_constructible_v<MappedMemory, void*, size_t>);

TEST_CASE("Slice returns a non owning view of the same bytes", "[memory][mapped_memory]") {
  std::array<rex::u8, 16> bytes{};
  MappedMemory mapping(bytes.data(), bytes.size());
  auto view = mapping.Slice(4, 8);
  REQUIRE(view != nullptr);
  CHECK(view->data() == bytes.data() + 4);
  CHECK(view->size() == 8);
  view.reset();
  CHECK(mapping.data() == bytes.data());
}
