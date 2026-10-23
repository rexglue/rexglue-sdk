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

REX_TEST_CASE("IsTitleLaunched follows the launch and exit reports", "[kernel][title_exit]") {
  rex::Config config;
  rex::Runtime runtime(config);
  CHECK_FALSE(runtime.IsTitleLaunched());
  runtime.ReportTitleLaunched();
  CHECK(runtime.IsTitleLaunched());
  runtime.ReportTitleExit(TitleExitReason::kNormalExit);
  CHECK_FALSE(runtime.IsTitleLaunched());
}

REX_TEST_CASE("IsTitleLaunched is false inside the exit callback", "[kernel][title_exit]") {
  rex::Config config;
  rex::Runtime runtime(config);
  bool seen_launched = true;
  runtime.OnTitleExit([&](TitleExitReason) { seen_launched = runtime.IsTitleLaunched(); });
  runtime.ReportTitleLaunched();
  runtime.ReportTitleExit(TitleExitReason::kHardExit);
  CHECK_FALSE(seen_launched);
}

REX_TEST_CASE("Shutdown clears IsTitleLaunched", "[kernel][title_exit]") {
  rex::Config config;
  rex::Runtime runtime(config);
  runtime.ReportTitleLaunched();
  runtime.Shutdown();
  CHECK_FALSE(runtime.IsTitleLaunched());
}
