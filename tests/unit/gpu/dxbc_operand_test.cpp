/**
 * @file        tests/unit/gpu/dxbc_operand_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "gpu/xenos/format/dxbc.h"

namespace {

using rex::gpu::dxbc::Src;

constexpr rex::u32 kWZYX = 0b00011011;
constexpr rex::u32 kYZWX = 0b00111001;

}  // namespace

REX_TEST_CASE("An outer swizzle selects from the source swizzle", "[gpu][dxbc]") {
  CHECK(Src::R(3, kWZYX).SwizzleSwizzled(Src::kXXXX).swizzle == Src::kWWWW);
  CHECK(Src::R(3, kYZWX).SwizzleSwizzled(kWZYX).swizzle == 0b01101100);
}

REX_TEST_CASE("An identity outer swizzle keeps the source swizzle", "[gpu][dxbc]") {
  CHECK(Src::R(3, kYZWX).SwizzleSwizzled(Src::kXYZW).swizzle == kYZWX);
}
