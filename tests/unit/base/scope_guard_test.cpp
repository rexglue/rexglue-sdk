/**
 * @file        tests/unit/base/scope_guard_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <utility>

#include <rex/types.h>
#include <rex/utility.h>

REX_TEST_CASE("ScopeGuard runs its function once on destruction", "[base][scope_guard]") {
  i32 runs = 0;
  {
    rex::ScopeGuard guard([&runs] { ++runs; });
    CHECK(runs == 0);
  }
  CHECK(runs == 1);
}

REX_TEST_CASE("A default ScopeGuard does nothing", "[base][scope_guard]") {
  rex::ScopeGuard guard;
  guard.Reset();
}

REX_TEST_CASE("A moved ScopeGuard runs once and the source does nothing", "[base][scope_guard]") {
  i32 runs = 0;
  {
    rex::ScopeGuard first([&runs] { ++runs; });
    rex::ScopeGuard second(std::move(first));
    first.Reset();
    CHECK(runs == 0);
  }
  CHECK(runs == 1);
}

REX_TEST_CASE("Move assignment runs the old function", "[base][scope_guard]") {
  i32 first_runs = 0;
  i32 second_runs = 0;
  {
    rex::ScopeGuard guard([&first_runs] { ++first_runs; });
    guard = rex::ScopeGuard([&second_runs] { ++second_runs; });
    CHECK(first_runs == 1);
    CHECK(second_runs == 0);
  }
  CHECK(second_runs == 1);
}

REX_TEST_CASE("Reset clears the function before calling it", "[base][scope_guard]") {
  i32 runs = 0;
  rex::ScopeGuard guard;
  guard = rex::ScopeGuard([&guard, &runs] {
    ++runs;
    guard.Reset();
  });
  guard.Reset();
  CHECK(runs == 1);
}
