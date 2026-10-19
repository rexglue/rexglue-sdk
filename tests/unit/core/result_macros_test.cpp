/**
 * @file        tests/unit/core/result_macros_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <type_traits>

#include <rex/result.h>

namespace {

[[nodiscard]] rex::Result<int> Give(bool ok) {
  if (ok) {
    return 7;
  }
  return rex::Err<int>(rex::ErrorCategory::kValidation, "no");
}

[[nodiscard]] rex::Result<void> Check(bool ok) {
  REX_ERROR_IF(!ok, rex::Error(rex::ErrorCategory::kValidation, "check failed"));
  return rex::Ok();
}

[[nodiscard]] rex::Result<void> ErrorWhen(bool fail) {
  REX_ERROR_IF(fail, rex::Error(rex::ErrorCategory::kValidation, "condition held"));
  return rex::Ok();
}

[[nodiscard]] rex::Result<int> UseTry(bool ok) {
  REX_TRY(int value, Give(ok));
  return value + 1;
}

[[nodiscard]] rex::Result<void> UseTryVoid(bool ok) {
  REX_TRY_VOID(Check(ok));
  return rex::Ok();
}

}  // namespace

REX_TEST_CASE("REX_TRY unwraps a value and propagates an error", "[core][macros]") {
  REQUIRE(UseTry(true).value() == 8);
  auto failed = UseTry(false);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().category == rex::ErrorCategory::kValidation);
}

REX_TEST_CASE("REX_TRY_VOID and REX_ERROR_IF propagate", "[core][macros]") {
  REQUIRE(UseTryVoid(true));
  auto failed = UseTryVoid(false);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().message == "check failed");
}

REX_TEST_CASE("REX_ERROR_IF returns the error when its condition holds", "[core][macros]") {
  auto failed = ErrorWhen(true);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().message == "condition held");
  REQUIRE(ErrorWhen(false));
}

REX_TEST_CASE("Ok makes a successful Result<void>", "[core][macros]") {
  rex::Result<void> s = rex::Ok();
  REQUIRE(s);
  static_assert(std::is_same_v<rex::Result<void>, rex::Result<void>>);
}
