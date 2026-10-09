/**
 * @file        tests/unit/kernel/char_strings_test.cpp
 * @brief       Rtl string exports compare over the lengths the caller gives
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string_view>

#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kStrlen = 0xFFFF;

u32 PutString(Caller& c, std::string_view text) {
  const u32 address = c.Alloc(static_cast<u32>(text.size()) + 1);
  std::memcpy(c.runtime().memory()->TranslateVirtual<char*>(address), text.data(), text.size());
  return address;
}
}  // namespace

TEST_CASE("strings: RtlCompareStringN resolves the strlen length before it compares",
          "[char][strings]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 shorter = PutString(c, "ab");
    const u32 longer = PutString(c, "abX");
    for (u32 case_insensitive : {0u, 1u}) {
      CHECK(c.Kernel("RtlCompareStringN", {shorter, kStrlen, longer, 3, case_insensitive}) == 0);
      CHECK(c.Kernel("RtlCompareStringN", {longer, 3, shorter, kStrlen, case_insensitive}) == 0);
    }
  });
}
