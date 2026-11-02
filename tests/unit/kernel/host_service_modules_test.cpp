/**
 * @file        tests/unit/kernel/host_service_modules_test.cpp
 * @brief       Tool mode builds no APU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <filesystem>

#include <rex/kernel/title_runtime.h>

#include "kernel/apu.h"
#include "test_memory.h"
#include "kernel/title_fixture.h"
#include "kernel/title_state.h"

namespace {

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Tool mode builds no APU", "[kernel][modules]") {
  rex::test::LoadedTitle loaded;
  rex::kernel::TitleRuntime& runtime = loaded.runtime;
  CHECK(StateOf(runtime).GetAPU() == nullptr);
  runtime.SetAudioPaused(true);
  CHECK_FALSE(runtime.IsAudioPaused());
}
