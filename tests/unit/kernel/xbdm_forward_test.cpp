/**
 * @file        tests/unit/kernel/xbdm_forward_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel/xbdm/forward.h"
#include "xbdm/xbdm_bodies.h"

extern "C" {
void __imp__DmWalkLoadedModules(PPCContext& ctx, rex::u8* base);
void __imp__DmRegisterCommandProcessorEx(PPCContext& ctx, rex::u8* base);
}

namespace {

void Idle(PPCContext&, rex::u8*) {}

}  // namespace

REX_TEST_CASE("the XBDM bodies and the kernel forwards pair up by export name", "[kernel][xbdm]") {
  const rex::xbdm::XBDMExports exports;
  const auto bodies = exports.GetBodies();
  REQUIRE_FALSE(bodies.empty());
  REQUIRE(rex::kernel::BindXBDMForwards(bodies));
  rex::kernel::UnbindXBDMForwards();
}

REX_TEST_CASE("binding without a body names an XBDM export that has none", "[kernel][xbdm]") {
  const auto bound = rex::kernel::BindXBDMForwards({});
  REQUIRE_FALSE(bound);
  CHECK(bound.error().message.find("__imp__") != std::string::npos);
}

REX_TEST_CASE("binding an XBDM body no kernel symbol forwards to is refused", "[kernel][xbdm]") {
  const rex::xbdm::XBDMExports exports;
  std::vector<rex::xbdm::XBDMBody> bodies(exports.GetBodies().begin(), exports.GetBodies().end());
  bodies.push_back(rex::xbdm::XBDMBody{"__imp__NoSuchXBDMExport", &Idle});
  const auto bound = rex::kernel::BindXBDMForwards(bodies);
  REQUIRE_FALSE(bound);
  CHECK(bound.error().message.find("__imp__NoSuchXBDMExport") != std::string::npos);
}

REX_TEST_CASE("an XBDM export without a bound body logs and returns its fixed value",
              "[kernel][xbdm]") {
  PPCContext ctx{};
  ctx.r3.u64 = 0xFFFF;
  __imp__DmWalkLoadedModules(ctx, nullptr);
  CHECK(ctx.r3.u32 == 0x82DA0104);
  ctx.r3.u64 = 0xFFFF;
  __imp__DmRegisterCommandProcessorEx(ctx, nullptr);
  CHECK(ctx.r3.u32 == 0);
}
