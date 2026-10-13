/**
 * @file        tests/unit/core/assert_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/assert.h>
#include <rex/types.h>

namespace {

struct Sized {
  rex::u32 a;
  rex::u32 b;
};

REX_STATIC_ASSERT_SIZE(Sized, 8);

int g_calls = 0;

[[maybe_unused]] bool Touch() {
  ++g_calls;
  return true;
}

int Classify(int v) {
  switch (v) {
    case 0:
      return 10;
    case 1:
      return 20;
    default:
      REX_UNHANDLED_CASE(v);
      return -1;
  }
}

}  // namespace

REX_TEST_CASE("REX_ASSERT evaluates its condition once in Debug and never in Release",
              "[core][assert]") {
  g_calls = 0;
  REX_ASSERT(Touch());
#ifdef NDEBUG
  CHECK(g_calls == 0);
#else
  CHECK(g_calls == 1);
#endif
}

REX_TEST_CASE("REX_ASSERT takes an optional message", "[core][assert]") {
  const char* detail = "variable message";
  int value = 3;
  REX_ASSERT(value == 3);
  REX_ASSERT(value == 3, "literal message");
  REX_ASSERT(value == 3, detail);
  CHECK(value == 3);
}

REX_TEST_CASE("A failed assertion reports its expression and its message", "[core][assert]") {
  CHECK(rex::detail::FormatAssertFailure("a.cpp", 12, "x == 1", "") ==
        "Assertion failed: x == 1\n  Location: a.cpp:12\n");
  CHECK(rex::detail::FormatAssertFailure("a.cpp", 12, "x == 1", "ring overflow") ==
        "Assertion failed: x == 1\n  Message: ring overflow\n  Location: a.cpp:12\n");
}

REX_TEST_CASE("REX_ASSERT_ALWAYS and REX_UNHANDLED_CASE compile in unreached branches",
              "[core][assert]") {
  CHECK(Classify(0) == 10);
  CHECK(Classify(1) == 20);
  if (g_calls < 0) {
    REX_ASSERT_ALWAYS();
    REX_ASSERT_ALWAYS("never reached");
  }
}

REX_TEST_CASE("AssertFail never throws", "[core][assert]") {
  STATIC_REQUIRE(noexcept(rex::detail::AssertFail("", 0, "", "")));
}
