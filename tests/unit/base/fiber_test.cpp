/**
 * @file        tests/unit/base/fiber_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include <rex/platform/thread.h>

using rex::platform::Fiber;

// Fiber entry functions cannot capture closures, so these are globals.
static Fiber* g_main = nullptr;
static int g_count = 0;

static void CountingFiber(void*) {
  ++g_count;
  Fiber::SwitchTo(g_main);
  ++g_count;
  Fiber::SwitchTo(g_main);
}

REX_TEST_CASE("rex::platform::Fiber switches context", "[base][fiber]") {
  g_count = 0;
  g_main = Fiber::ConvertCurrentThread();
  auto* f = Fiber::Create(size_t{256} * 1024, CountingFiber, nullptr);

  REQUIRE(g_count == 0);
  Fiber::SwitchTo(f);
  REQUIRE(g_count == 1);
  Fiber::SwitchTo(f);
  REQUIRE(g_count == 2);

  f->Destroy();
  g_main->Destroy();
  g_main = nullptr;
}
