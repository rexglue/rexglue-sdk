/**
 * @file        tests/unit/kernel/xmp_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>

#include "xam/app/xam_apps.h"
#include "xam/xam_state.h"

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

u32 PlayEmptyPlaylist(Caller& c, rex::xam::apps::XMPApp& app) {
  const u32 handle = c.Alloc(4);
  REQUIRE(app.XMPCreateTitlePlaylist(0, 0, 0, u"", 0, 0, handle) == rex::kernel::X_E_SUCCESS);
  REQUIRE(app.XMPPlayTitlePlaylist(c.Load32(handle), 0) == rex::kernel::X_E_SUCCESS);
  return c.Load32(handle);
}

}  // namespace

REX_TEST_CASE("xmp: next and previous on an empty playlist find no song", "[kernel][xmp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::xam::apps::XMPApp app(rex::xam::CurrentTitle());
    PlayEmptyPlaylist(c, app);
    CHECK(app.XMPNext() == rex::kernel::X_E_NOTFOUND);
    CHECK(app.XMPPrevious() == rex::kernel::X_E_NOTFOUND);
  });
}

REX_TEST_CASE("xmp: song info on an empty playlist fails", "[kernel][xmp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::xam::apps::XMPApp app(rex::xam::CurrentTitle());
    PlayEmptyPlaylist(c, app);
    const u32 args = c.Alloc(12);
    c.Store32(args, 2);
    c.Store32(args + 4, 0);
    c.Store32(args + 8, c.Alloc(0x400));
    CHECK(app.DispatchMessageSync(0x0007000E, args, 12) == rex::kernel::X_E_FAIL);
  });
}

REX_TEST_CASE("xmp: the playback controller query reports controller 0 unlocked", "[kernel][xmp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::xam::apps::XMPApp app(rex::xam::CurrentTitle());
    const u32 args = c.Alloc(12);
    c.Store32(args, 2);
    const u32 controller = c.Alloc(4);
    const u32 locked = c.Alloc(4);
    c.Store32(controller, 0xFFFFFFFF);
    c.Store32(locked, 0xFFFFFFFF);
    c.Store32(args + 4, controller);
    c.Store32(args + 8, locked);
    CHECK(app.DispatchMessageSync(0x0007001B, args, 12) == rex::kernel::X_E_SUCCESS);
    CHECK(c.Load32(controller) == 0);
    CHECK(c.Load32(locked) == 0);
  });
}
