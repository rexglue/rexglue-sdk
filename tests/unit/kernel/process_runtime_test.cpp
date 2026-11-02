/**
 * @file        tests/unit/kernel/process_runtime_test.cpp
 * @brief       The process wide runtime accessor
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <thread>

#include <rex/kernel/title_runtime.h>

#include "../kernel/kernel_fixture.h"

REX_TEST_CASE("GetProcessRuntime is null before Setup and after Shutdown", "[kernel]") {
  CHECK(REX_RUNTIME() == nullptr);
  {
    rex::test::KernelFixture fixture;
    CHECK(REX_RUNTIME() == &fixture.GetRuntime());
  }
  CHECK(REX_RUNTIME() == nullptr);
}

REX_TEST_CASE("GetProcessRuntime works on a thread with no runtime", "[kernel]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleRuntime* seen = nullptr;
  std::thread host([&seen] { seen = REX_RUNTIME(); });
  host.join();
  CHECK(seen == &fixture.GetRuntime());
}
