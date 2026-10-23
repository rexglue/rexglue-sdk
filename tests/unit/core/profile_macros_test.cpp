/**
 * @file        tests/unit/core/profile_macros_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>
#include <string_view>

#include <rex/dbg.h>

REX_TEST_CASE("Profiling macros compile in every mode", "[core][profiling]") {
  const std::string zone_name = "ProfilingMacrosTest";
  REX_PROFILE_SCOPE("ProfilingMacrosTest");
  REX_PROFILE_SCOPE_DYNAMIC(zone_name);
  REX_PROFILE_PLOT("ProfilingMacrosTest Integer", 3);
  REX_PROFILE_PLOT_F64("ProfilingMacrosTest Float", 0.5f);
  REX_PROFILE_FRAME();
  CHECK_FALSE(REX_PROFILE_IS_CONNECTED());
}

REX_TEST_CASE("Memory profiling macros compile in every mode and skip their arguments",
              "[core][profiling]") {
  [[maybe_unused]] int block = 0;
  int evaluated = 0;
  REX_PROFILE_ALLOC(&block, evaluated++);
  REX_PROFILE_FREE(&block);
  REX_PROFILE_ALLOC_NAMED(&block, evaluated++, "ProfilingMacrosTest");
  REX_PROFILE_FREE_NAMED(&block, "ProfilingMacrosTest");
  REX_PROFILE_ALLOC_STACK(&block, evaluated++, 8);
  REX_PROFILE_FREE_STACK(&block, 8);
  REX_PROFILE_ALLOC_NAMED_STACK(&block, evaluated++, 8, "ProfilingMacrosTest");
  REX_PROFILE_FREE_NAMED_STACK(&block, 8, "ProfilingMacrosTest");
  CHECK(evaluated == 0);
}
