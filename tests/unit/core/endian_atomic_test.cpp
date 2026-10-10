/**
 * @file        tests/unit/core/endian_atomic_test.cpp
 * @brief       Atomic load, store and compare exchange on byte order types
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>

#include <rex/types.h>

REX_TEST_CASE("An atomic store on a big endian value keeps the raw bytes big endian",
              "[core][types]") {
  be_u32 field;
  field.store(0x11223344, std::memory_order_release);
  CHECK(field.value == rex::byte_swap(rex::u32{0x11223344}));
  CHECK(field.load(std::memory_order_acquire) == 0x11223344);
}

REX_TEST_CASE("compare_exchange on a big endian value stores the desired value when it matches",
              "[core][types]") {
  be_u32 field = 0;
  rex::u32 expected = 0;
  CHECK(field.compare_exchange(expected, 0x82001000, std::memory_order_acquire));
  CHECK(expected == 0);
  CHECK(field.value == rex::byte_swap(rex::u32{0x82001000}));
  CHECK(field == 0x82001000);
}

REX_TEST_CASE("compare_exchange on a big endian value returns the current value when it differs",
              "[core][types]") {
  be_u32 field = 0x82001000;
  rex::u32 expected = 0;
  CHECK_FALSE(field.compare_exchange(expected, 0x82002000, std::memory_order_acquire));
  CHECK(expected == 0x82001000);
  CHECK(field == 0x82001000);
}
