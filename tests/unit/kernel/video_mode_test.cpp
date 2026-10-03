/**
 * @file        tests/unit/kernel/video_mode_test.cpp
 * @brief       VdQueryVideoMode reads the runtime video mode
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/runtime.h>

#include "kernel_fixture.h"

TEST_CASE("VdQueryVideoMode returns the runtime video mode", "[kernel][video]") {
  rex::testing::KernelFixture fixture;
  auto* cvars = fixture.api().Get<rex::cvar::ICvarRegistry>();
  REQUIRE(cvars != nullptr);
  REQUIRE(cvars->Set("ui.video_mode_width", "1920"));
  REQUIRE(cvars->Set("ui.video_mode_height", "1080"));
  const rex::runtime::VideoMode expected = fixture.runtime().video_mode();
  fixture.RunOnGuestThread([expected](rex::testing::Caller&) {
    rex::runtime::X_VIDEO_MODE mode{};
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    CHECK(rex::u32(mode.display_width) == expected.width);
    CHECK(rex::u32(mode.display_height) == expected.height);
    CHECK(float(mode.refresh_rate) == expected.refresh_rate);
  });
  CHECK(expected.width == 1280);
  CHECK(expected.height == 720);
}
