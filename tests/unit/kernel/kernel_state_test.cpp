/**
 * @file        tests/unit/kernel/kernel_state_test.cpp
 * @brief       Kernel state belongs to one runtime and is empty after it shuts down
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "kernel/shim/heap.h"
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/runtime_settings.h>

#include "kernel/title_fixture.h"
#include "test_memory.h"
#include "kernel/settings_access.h"
#include "kernel/title_state.h"

namespace {

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "unload resets the kernel state", "[kernel][kernel_state]") {
  rex::test::LoadedTitle loaded;
  rex::kernel::TitleRuntime& runtime = loaded.runtime;

  rex::kernel::TitleState& state = StateOf(runtime);
  REQUIRE(rex::kernel::shim::InitHeap(8, StateOf(runtime).GetMemory()));
  CHECK(state.heap.Alloc(64, true) != 0);
  state.fiber_count = 7;
  state.audio_submit_logs = 10;

  runtime.Unload();

  CHECK(state.fiber_count == 0);
  CHECK(state.audio_submit_logs == 0);
  CHECK(state.strtok_context == nullptr);
}

REX_TEST_CASE_METHOD(Fixture, "two runtimes in turn start from the same kernel state",
                     "[kernel][kernel_state]") {
  rex::test::TitleBoot boot;
  {
    rex::kernel::TitleRuntime first{rex::ppc::PPCImage{}};
    REQUIRE(first.Load());
    StateOf(first).fiber_count = 9;
    first.Unload();
  }
  rex::kernel::TitleRuntime second{rex::ppc::PPCImage{}};
  REQUIRE(second.Load());
  CHECK(StateOf(second).fiber_count == 0);
}

REX_TEST_CASE_METHOD(Fixture, "launch closes the until launch settings and unload opens them",
                     "[kernel][settings]") {
  rex::test::LoadedTitle loaded;
  rex::kernel::TitleRuntime& runtime = loaded.runtime;

  rex::kernel::RuntimeSettings& settings = rex::kernel::GetRuntimeSettings();
  CHECK(settings.title_memory_mb.Set(100));
  CHECK(settings.ignore_thread_priorities.Set(false));

  StateOf(runtime).ReportTitleLaunched();
  CHECK_FALSE(settings.title_memory_mb.Set(200));
  CHECK(settings.title_memory_mb.Get() == 100);
  CHECK(settings.ignore_thread_priorities.Set(true));

  runtime.Unload();
  CHECK(settings.title_memory_mb.Set(300));
  CHECK(settings.title_memory_mb.Get() == 300);
}
