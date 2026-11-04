/**
 * @file        tests/unit/gpu/settings_scope_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"
#include "temp_folder.h"

#include <string>
#include <string_view>
#include <vector>

#include <rex/app/app.h>
#include <rex/apu/audio_system.h>
#include <rex/cli.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_system.h>

namespace {

class GPUApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  explicit GPUApp(std::string driver, rex::GraphicsFamily family = rex::GraphicsFamily::kXenos)
      : ObjectApp("gpu_scope_test"), driver_(std::move(driver)), family_(family) {}

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::vector<std::string>{})); }

 protected:
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements requirements;
    requirements.graphics_family = family_;
    return requirements;
  }

  rex::Result<void> OnConfigure(rex::SettingsRegistry&) override {
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none");
    (void)GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "none");
    (void)GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none");
    (void)GetGraphics().GetSettings().driver.SetText(rex::SettingLayer::kTitle, driver_);
    return rex::Ok();
  }

 private:
  std::string driver_;
  rex::GraphicsFamily family_;
  rex::test::TempFolder folder_{"rex_gpu_scope_"};
};

bool HasScope(std::string_view scope) {
  return !REX_SETTINGS().Enumerate(scope).empty();
}

}  // namespace

REX_TEST_CASE("with no provider the GPU scopes are absent", "[gpu][settings]") {
  GPUApp app("none", rex::GraphicsFamily::kNone);
  REQUIRE(app.Start());
  CHECK_FALSE(HasScope("gpu.xenos"));
  CHECK_FALSE(HasScope("gpu.vulkan"));
  CHECK_FALSE(HasScope("gpu.d3d12"));
  app.Shutdown();
}

REX_TEST_CASE("the null Xenos provider brings the gpu.xenos scope", "[gpu][settings]") {
  GPUApp app("null");
  REQUIRE(app.Start());
  CHECK(HasScope("gpu.xenos"));
  CHECK_FALSE(HasScope("gpu.vulkan"));
  CHECK_FALSE(HasScope("gpu.d3d12"));
  app.Shutdown();
}

REX_TEST_CASE("loading a provider adds its scope and unloading removes it", "[gpu][settings]") {
  GPUApp app("null");
  CHECK_FALSE(HasScope("gpu.xenos"));
  REQUIRE(app.Start());
  CHECK(HasScope("gpu.xenos"));
  app.Shutdown();
  CHECK_FALSE(HasScope("gpu.xenos"));
}
