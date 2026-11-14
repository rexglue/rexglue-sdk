/**
 * @file        tests/unit/ppc/vector_tables_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

#include "rex_test.h"

#include <rex/ppc/vmx.h>

namespace {

bool IsAligned16(const void* address) {
  return (reinterpret_cast<std::uintptr_t>(address) & 15) == 0;
}

}  // namespace

REX_TEST_CASE("The vector mask and shift tables are 16 byte aligned", "[ppc][simd]") {
  CHECK(IsAligned16(rex::ppc::simd::kVectorMaskL));
  CHECK(IsAligned16(rex::ppc::simd::kVectorMaskR));
  CHECK(IsAligned16(rex::ppc::simd::kVectorShiftTableL));
  CHECK(IsAligned16(rex::ppc::simd::kVectorShiftTableR));
}
