/**
 * @file        tests/unit/runtime/process_runtime_test.cpp
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

#include <rex/runtime.h>

#include "../kernel/kernel_fixture.h"

REX_TEST_CASE("GetProcessRuntime is null before Setup and after Shutdown", "[runtime]") {
  CHECK(rex::GetProcessRuntime() == nullptr);
  {
    rex::test::KernelFixture fixture;
    CHECK(rex::GetProcessRuntime() == &fixture.GetRuntime());
  }
  CHECK(rex::GetProcessRuntime() == nullptr);
}

REX_TEST_CASE("GetProcessRuntime works on a thread with no runtime", "[runtime]") {
  rex::test::KernelFixture fixture;
  rex::Runtime* seen = nullptr;
  std::thread host([&seen] { seen = rex::GetProcessRuntime(); });
  host.join();
  CHECK(seen == &fixture.GetRuntime());
}
