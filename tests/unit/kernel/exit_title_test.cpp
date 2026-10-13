/**
 * @file        tests/unit/kernel/exit_title_test.cpp
 * @brief       Unit tests for the title exit report
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <vector>

#include <rex/api.h>
#include <rex/runtime.h>

using rex::runtime::TitleExitReason;

REX_TEST_CASE("ReportTitleExit calls the callback once with the first reason",
              "[kernel][title_exit]") {
  rex::Config config;
  rex::Runtime runtime(config);
  std::vector<TitleExitReason> seen;
  runtime.OnTitleExit([&](TitleExitReason reason) { seen.push_back(reason); });
  runtime.ReportTitleExit(TitleExitReason::kHardExit);
  runtime.ReportTitleExit(TitleExitReason::kNormalExit);
  REQUIRE(seen == std::vector<TitleExitReason>{TitleExitReason::kHardExit});
}
