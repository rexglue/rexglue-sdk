/**
 * @file        tests/unit/kernel/exit_title_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <vector>

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_exit.h>
#include "kernel/title_state.h"
#include "kernel/title_fixture.h"

using rex::kernel::TitleExitReason;

REX_TEST_CASE("ReportTitleExit calls the callback once with the first reason",
              "[kernel][title_exit]") {
  rex::kernel::TitleRuntime runtime{rex::ppc::PPCImage{}};
  std::vector<rex::kernel::TitleExitReason> seen;
  auto exit_guard =
      runtime.OnTitleExit([&](rex::kernel::TitleExitReason reason) { seen.push_back(reason); });
  StateOf(runtime).ReportTitleExit(rex::kernel::TitleExitReason::kHardExit);
  StateOf(runtime).ReportTitleExit(rex::kernel::TitleExitReason::kNormalExit);
  REQUIRE(seen ==
          std::vector<rex::kernel::TitleExitReason>{rex::kernel::TitleExitReason::kHardExit});
}

REX_TEST_CASE("dropping the guard unsubscribes the title exit callback", "[kernel][title_exit]") {
  rex::kernel::TitleRuntime runtime{rex::ppc::PPCImage{}};
  int calls = 0;
  {
    auto exit_guard = runtime.OnTitleExit([&](rex::kernel::TitleExitReason) { ++calls; });
  }
  StateOf(runtime).ReportTitleExit(rex::kernel::TitleExitReason::kHardExit);
  CHECK(calls == 0);
}

REX_TEST_CASE("an old guard does not clear a newer title exit callback", "[kernel][title_exit]") {
  rex::kernel::TitleRuntime runtime{rex::ppc::PPCImage{}};
  int first_calls = 0;
  int second_calls = 0;
  auto first = runtime.OnTitleExit([&](rex::kernel::TitleExitReason) { ++first_calls; });
  auto second = runtime.OnTitleExit([&](rex::kernel::TitleExitReason) { ++second_calls; });
  first.Reset();
  StateOf(runtime).ReportTitleExit(rex::kernel::TitleExitReason::kHardExit);
  CHECK(first_calls == 0);
  CHECK(second_calls == 1);
}

REX_TEST_CASE("IsTitleLaunched follows the launch and exit reports", "[kernel][title_exit]") {
  rex::kernel::TitleRuntime runtime{rex::ppc::PPCImage{}};
  CHECK_FALSE(StateOf(runtime).IsTitleLaunched());
  StateOf(runtime).ReportTitleLaunched();
  CHECK(StateOf(runtime).IsTitleLaunched());
  StateOf(runtime).ReportTitleExit(rex::kernel::TitleExitReason::kNormalExit);
  CHECK_FALSE(StateOf(runtime).IsTitleLaunched());
}

REX_TEST_CASE("IsTitleLaunched is false inside the exit callback", "[kernel][title_exit]") {
  rex::kernel::TitleRuntime runtime{rex::ppc::PPCImage{}};
  bool seen_launched = true;
  auto exit_guard = runtime.OnTitleExit(
      [&](rex::kernel::TitleExitReason) { seen_launched = StateOf(runtime).IsTitleLaunched(); });
  StateOf(runtime).ReportTitleLaunched();
  StateOf(runtime).ReportTitleExit(rex::kernel::TitleExitReason::kHardExit);
  CHECK_FALSE(seen_launched);
}

REX_TEST_CASE("Unload clears IsTitleLaunched", "[kernel][title_exit]") {
  rex::test::LoadedTitle loaded;
  rex::kernel::TitleRuntime& runtime = loaded.runtime;
  StateOf(runtime).ReportTitleLaunched();
  runtime.Unload();
  CHECK_FALSE(StateOf(runtime).IsTitleLaunched());
}
