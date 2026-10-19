/**
 * @file        tests/unit/kernel/bugcheck_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>

#include "kernel_fixture.h"

using rex::runtime::TitleExitReason;
using rex::test::Caller;
using rex::test::KernelFixture;

REX_TEST_CASE("KeBugCheckEx never returns and reports a bug check", "[kernel][bugcheck]") {
  static std::atomic<bool> g_returned;
  static std::atomic<int> g_reported;
  g_returned = false;
  g_reported = -1;
  KernelFixture fixture;
  fixture.GetRuntime().OnTitleExit(
      [](TitleExitReason reason) { g_reported = static_cast<int>(reason); });
  fixture.RunUntilGuestThreadEnds([](Caller& c) {
    c.Kernel("KeBugCheckEx", {0x1234, 1, 2, 3, 4});
    g_returned = true;
  });
  CHECK_FALSE(g_returned);
  CHECK(g_reported == static_cast<int>(TitleExitReason::kBugCheck));
}

REX_TEST_CASE("KeBugCheck never returns and reports a bug check", "[kernel][bugcheck]") {
  static std::atomic<bool> g_returned;
  static std::atomic<int> g_reported;
  g_returned = false;
  g_reported = -1;
  KernelFixture fixture;
  fixture.GetRuntime().OnTitleExit(
      [](TitleExitReason reason) { g_reported = static_cast<int>(reason); });
  fixture.RunUntilGuestThreadEnds([](Caller& c) {
    c.Kernel("KeBugCheck", {0x1234});
    g_returned = true;
  });
  CHECK_FALSE(g_returned);
  CHECK(g_reported == static_cast<int>(TitleExitReason::kBugCheck));
}
