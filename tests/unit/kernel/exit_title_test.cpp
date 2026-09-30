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

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <rex/api.h>
#include <rex/runtime.h>

using rex::runtime::TitleExitReason;

TEST_CASE("ReportTitleExit calls the callback once with the first reason", "[title_exit]") {
  rex::Config config;
  rex::Runtime runtime(config);
  std::vector<TitleExitReason> seen;
  runtime.OnTitleExit([&](TitleExitReason reason) { seen.push_back(reason); });
  runtime.ReportTitleExit(TitleExitReason::HardExit);
  runtime.ReportTitleExit(TitleExitReason::NormalExit);
  REQUIRE(seen == std::vector<TitleExitReason>{TitleExitReason::HardExit});
}
