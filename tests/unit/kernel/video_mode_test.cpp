/**
 * @file        tests/unit/kernel/video_mode_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/kernel/xboxkrnl.h>
#include <rex/kernel/title_runtime.h>
#include <rex/title.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

REX_TEST_CASE("VdQueryVideoMode returns the runtime video mode", "[kernel][video]") {
  rex::test::KernelFixture fixture;
  const rex::VideoMode expected = StateOf(fixture.GetRuntime()).GetVideoMode();
  CHECK(expected.width == 1280);
  CHECK(expected.height == 720);
  CHECK(expected.refresh_rate == 60.0f);
  fixture.RunOnGuestThread([expected](rex::test::Caller&) {
    rex::kernel::X_VIDEO_MODE mode{};
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    CHECK(static_cast<rex::u32>(mode.display_width) == expected.width);
    CHECK(static_cast<rex::u32>(mode.display_height) == expected.height);
    CHECK(static_cast<f32>(mode.refresh_rate) == expected.refresh_rate);
    CHECK(static_cast<rex::u32>(mode.is_widescreen) == 1);
    CHECK(static_cast<rex::u32>(mode.is_hi_def) == 1);
    CHECK(static_cast<rex::u32>(mode.is_interlaced) == 0);
  });
}
