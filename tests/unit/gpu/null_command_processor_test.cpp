/**
 * @file        tests/unit/gpu/null_command_processor_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>
#include <thread>

#include "fake_driver_context.h"
#include "fake_title_context.h"
#include "gpu/xenos/null/null_xenos_graphics_driver.h"
#include "test_memory.h"

namespace {

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
  ~Fixture() { rex::test::ReleaseTestMemory(); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Shutdown stops vblanks before the command processor goes away",
                     "[gpu]") {
  for (rex::u32 cycle = 0; cycle < 10; ++cycle) {
    rex::test::FakeTitleContext title(rex::test::GetTestMemory());
    rex::test::FakeDriverContext context;
    rex::gpu::null::NullXenosGraphicsDriver driver;
    REQUIRE(driver.Initialize(context));
    REQUIRE(driver.AttachTitle(rex::TitleInfo{}, title, title, title));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    driver.DetachTitle();
    driver.Shutdown();
  }
}

REX_TEST_CASE_METHOD(Fixture, "The command processor shuts down promptly while it waits idle",
                     "[gpu]") {
  constexpr auto kIdleTime = std::chrono::milliseconds(200);
  constexpr auto kShutdownBound = std::chrono::seconds(2);

  rex::test::FakeTitleContext title(rex::test::GetTestMemory());
  rex::test::FakeDriverContext context;
  rex::gpu::null::NullXenosGraphicsDriver driver;
  REQUIRE(driver.Initialize(context));
  REQUIRE(driver.AttachTitle(rex::TitleInfo{}, title, title, title));

  std::this_thread::sleep_for(kIdleTime);
  const auto start = std::chrono::steady_clock::now();
  driver.DetachTitle();
  CHECK(std::chrono::steady_clock::now() - start < kShutdownBound);
  driver.Shutdown();
}
