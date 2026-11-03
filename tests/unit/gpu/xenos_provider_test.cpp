/**
 * @file        tests/unit/gpu/xenos_provider_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string_view>

#include <rex/app/app.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/app/app_info.h>
#include <rex/platform.h>

#include "base/module_group.h"
#include "base/module_registration.h"
#include "gpu/fake_driver_context.h"

namespace {

class FakeAppInfo final : public rex::IAppInfo {
 public:
  const rex::AppSysRequirements& GetRequirements() const override { return needs; }
  std::string_view GetAppName() const override { return "xenos_provider_test"; }

  rex::AppSysRequirements needs;
};

struct GraphicsGroup {
  rex::test::FakeDriverContext context;
  FakeAppInfo info;
  rex::ModuleGroup group;

  GraphicsGroup() {
    group.Serve(static_cast<rex::IDriverContext&>(context), rex::IDriverContext::kInterface);
    group.Serve(static_cast<rex::IAppInfo&>(info), rex::IAppInfo::kInterface);
    REQUIRE(group.Load("rexgpu").has_value());
  }
};

const rex::DriverRegistration* FindGraphics(rex::ModuleGroup& group, std::string_view name) {
  for (const rex::DriverRegistration* registration :
       rex::test::GetRegistrations(group, rex::DriverType::kGraphics)) {
    if (registration->name == name) {
      return registration;
    }
  }
  return nullptr;
}

}  // namespace

REX_TEST_CASE("null provider registrations null in family xenos", "[gpu][xenos]") {
  GraphicsGroup graphics;
  rex::ModuleGroup& group = graphics.group;
  REQUIRE(group.Load("rexgpu-xenos-null").has_value());
  REQUIRE(group.Connect().has_value());
  const rex::DriverRegistration* registration = FindGraphics(group, "null");
  REQUIRE(registration != nullptr);
  CHECK(registration->family == rex::GraphicsFamily::kXenos);
  CHECK(registration->type == rex::DriverType::kGraphics);
}

REX_TEST_CASE("vulkan provider registrations vulkan in family xenos", "[gpu][xenos]") {
  GraphicsGroup graphics;
  rex::ModuleGroup& group = graphics.group;
  REQUIRE(group.Load("rexgpu-xenos-vulkan").has_value());
  REQUIRE(group.Connect().has_value());
  const rex::DriverRegistration* registration = FindGraphics(group, "vulkan");
  REQUIRE(registration != nullptr);
  CHECK(registration->family == rex::GraphicsFamily::kXenos);
  CHECK(registration->type == rex::DriverType::kGraphics);
}

#if REX_PLATFORM_WIN32
REX_TEST_CASE("d3d12 provider registrations d3d12 in family xenos", "[gpu][xenos]") {
  GraphicsGroup graphics;
  rex::ModuleGroup& group = graphics.group;
  REQUIRE(group.Load("rexgpu-xenos-d3d12").has_value());
  REQUIRE(group.Connect().has_value());
  const rex::DriverRegistration* registration = FindGraphics(group, "d3d12");
  REQUIRE(registration != nullptr);
  CHECK(registration->family == rex::GraphicsFamily::kXenos);
}
#endif
