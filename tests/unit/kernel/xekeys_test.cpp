/**
 * @file        tests/unit/kernel/xekeys_test.cpp
 * @brief       The console sign and signature verification exports return 0
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {
constexpr u32 kOutputSize = 0x100;
}

REX_TEST_CASE("xekeys: the console sign export returns 0 and leaves the output untouched",
              "[kernel][crypt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 hash = c.Alloc(0x14);
    const u32 output = c.Alloc(kOutputSize);
    for (u32 offset = 0; offset < kOutputSize; offset += 4) {
      c.Store32(output + offset, 0xCDCDCDCD);
    }
    CHECK(c.Kernel("XeKeysConsolePrivateKeySign", {hash, output}) == 0);
    for (u32 offset = 0; offset < kOutputSize; offset += 4) {
      CHECK(c.Load32(output + offset) == 0xCDCDCDCD);
    }
    CHECK(c.Kernel("XeKeysConsolePrivateKeySign", {hash, 0}) == 0);
  });
}

REX_TEST_CASE("xekeys: the console signature verification export returns 0", "[kernel][crypt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 buffer = c.Alloc(0x100);
    CHECK(c.Kernel("XeKeysConsoleSignatureVerification", {buffer, buffer, buffer}) == 0);
  });
}
