/**
 * @file        tests/unit/core/macros_test.cpp
 * @brief       Unit tests for REX_TRY, REX_TRY_VOID and REX_CHECK
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include <rex/macros.h>
#include <rex/result.h>

namespace {

rex::Result<int> Give(bool ok) {
  if (ok) {
    return 7;
  }
  return rex::Err<int>(rex::ErrorCategory::Validation, "no");
}

rex::Status Check(bool ok) {
  REX_CHECK(ok, rex::Error(rex::ErrorCategory::Validation, "check failed"));
  return rex::Ok();
}

rex::Result<int> UseTry(bool ok) {
  REX_TRY(int value, Give(ok));
  return value + 1;
}

rex::Status UseTryVoid(bool ok) {
  REX_TRY_VOID(Check(ok));
  return rex::Ok();
}

}  // namespace

TEST_CASE("REX_TRY unwraps a value and propagates an error", "[macros]") {
  REQUIRE(UseTry(true).value() == 8);
  auto failed = UseTry(false);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().category == rex::ErrorCategory::Validation);
}

TEST_CASE("REX_TRY_VOID and REX_CHECK propagate", "[macros]") {
  REQUIRE(UseTryVoid(true));
  auto failed = UseTryVoid(false);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().message == "check failed");
}

TEST_CASE("Status is the void result", "[macros]") {
  rex::Status s = rex::Ok();
  REQUIRE(s);
  static_assert(std::is_same_v<rex::Status, rex::VoidResult>);
}
