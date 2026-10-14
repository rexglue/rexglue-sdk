/**
 * @file        tests/unit/kernel/char_xmp_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/kernel/xam/apps/xmp_app.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

u32 PlayEmptyPlaylist(Caller& c, rex::kernel::xam::apps::XmpApp& app) {
  const u32 handle = c.Alloc(4);
  REQUIRE(app.XMPCreateTitlePlaylist(0, 0, 0, u"", 0, 0, handle) == rex::X_E_SUCCESS);
  REQUIRE(app.XMPPlayTitlePlaylist(c.Load32(handle), 0) == rex::X_E_SUCCESS);
  return c.Load32(handle);
}

}  // namespace

REX_TEST_CASE("xmp: next and previous on an empty playlist find no song", "[char][xmp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::kernel::xam::apps::XmpApp app(c.GetRuntime());
    PlayEmptyPlaylist(c, app);
    CHECK(app.XMPNext() == rex::X_E_NOTFOUND);
    CHECK(app.XMPPrevious() == rex::X_E_NOTFOUND);
  });
}

REX_TEST_CASE("xmp: song info on an empty playlist fails", "[char][xmp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::kernel::xam::apps::XmpApp app(c.GetRuntime());
    PlayEmptyPlaylist(c, app);
    const u32 args = c.Alloc(12);
    c.Store32(args, 2);
    c.Store32(args + 4, 0);
    c.Store32(args + 8, c.Alloc(0x400));
    CHECK(app.DispatchMessageSync(0x0007000E, args, 12) == rex::X_E_FAIL);
  });
}
