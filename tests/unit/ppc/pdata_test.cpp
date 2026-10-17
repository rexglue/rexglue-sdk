/**
 * @file        tests/unit/ppc/pdata_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ppc/pdata.h>

using rex::u32;
using rex::ppc::DecodeRuntimeFunction;
using rex::ppc::RuntimeFunctionType;

REX_TEST_CASE("A pdata word decodes each of the 4 types", "[ppc][pdata]") {
  CHECK(DecodeRuntimeFunction(0x82000000, 0x00000A05).type == RuntimeFunctionType::SaveMillicode);
  CHECK(DecodeRuntimeFunction(0x82000000, 0x40000A05).type == RuntimeFunctionType::NoHandler);
  CHECK(DecodeRuntimeFunction(0x82000000, 0x80000A05).type ==
        RuntimeFunctionType::RestoreMillicode);
  CHECK(DecodeRuntimeFunction(0x82000000, 0xC0000A05).type == RuntimeFunctionType::Handler);
}

REX_TEST_CASE("A pdata word splits the length and the prologue", "[ppc][pdata]") {
  const auto entry = DecodeRuntimeFunction(0x82001000, 0x40001205);
  CHECK(entry.begin == 0x82001000);
  CHECK(entry.length == 0x12);
  CHECK(entry.prolog == 5);
}

REX_TEST_CASE("A pdata word keeps the 22 bit length and the 8 bit prologue apart", "[ppc][pdata]") {
  const auto longest = DecodeRuntimeFunction(0, 0x7FFFFF00);
  CHECK(longest.type == RuntimeFunctionType::NoHandler);
  CHECK(longest.length == 0x3FFFFF);
  CHECK(longest.prolog == 0);
  const auto widest = DecodeRuntimeFunction(0, 0x000000FF);
  CHECK(widest.type == RuntimeFunctionType::SaveMillicode);
  CHECK(widest.length == 0);
  CHECK(widest.prolog == 0xFF);
}
