/**
 * @file        tests/unit/kernel/xam_forward_test.cpp
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

#include "kernel/xam/forward.h"
#include "xam/xam_bodies.h"

namespace {

void Idle(PPCContext&, rex::u8*) {}

}  // namespace

REX_TEST_CASE("the XAM bodies and the kernel forwards pair up by export name", "[kernel][xam]") {
  const rex::xam::XAMExports exports;
  const auto bodies = exports.GetBodies();
  REQUIRE_FALSE(bodies.empty());
  REQUIRE(rex::kernel::BindXAMForwards(bodies));
  rex::kernel::UnbindXAMForwards();
}

REX_TEST_CASE("binding without a body names an export that has none", "[kernel][xam]") {
  const auto bound = rex::kernel::BindXAMForwards({});
  REQUIRE_FALSE(bound);
  const std::string& message = bound.error().message;
  CHECK((message.find("__imp__") != std::string::npos ||
         message.find("rexshim_") != std::string::npos));
}

REX_TEST_CASE("binding a body no kernel symbol forwards to is refused", "[kernel][xam]") {
  const rex::xam::XAMExports exports;
  std::vector<rex::xam::XAMBody> bodies(exports.GetBodies().begin(), exports.GetBodies().end());
  bodies.push_back(rex::xam::XAMBody{"__imp__NoSuchXAMExport", &Idle});
  const auto bound = rex::kernel::BindXAMForwards(bodies);
  REQUIRE_FALSE(bound);
  CHECK(bound.error().message.find("__imp__NoSuchXAMExport") != std::string::npos);
}
