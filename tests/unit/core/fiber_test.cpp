/**
 * @file        tests/unit/core/fiber_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include <rex/platform/fiber.h>

using rex::platform::Fiber;

// Fiber entry functions cannot capture closures, so these are globals.
static Fiber* s_main = nullptr;
static int s_count = 0;

static void CountingFiber(void*) {
  ++s_count;
  Fiber::SwitchTo(s_main);
  ++s_count;
  Fiber::SwitchTo(s_main);
}

REX_TEST_CASE("rex::platform::Fiber - basic context switch", "[core][fiber]") {
  s_count = 0;
  s_main = Fiber::ConvertCurrentThread();
  auto* f = Fiber::Create(size_t{256} * 1024, CountingFiber, nullptr);

  REQUIRE(s_count == 0);
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 1);
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 2);

  f->Destroy();
  s_main->Destroy();
  s_main = nullptr;
}
