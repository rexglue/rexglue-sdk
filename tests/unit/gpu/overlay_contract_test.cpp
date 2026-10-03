/**
 * @file        tests/unit/gpu/overlay_contract_test.cpp
 * @brief       The UI tier draws overlays through a backend's IOverlayDrawer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/api.h>
#include <rex/gpu.h>
#include <rex/platform/dynlib.h>
#include <rex/platform/file.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay.h>
#include <rex/ui/title_app.h>

#include "core/module_loader.h"
#include "modules/test_module_common.h"

namespace {

std::vector<std::string> g_events;

void Sink(const char*, const char* event) {
  g_events.emplace_back(event);
}

class ShownDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ShownDialog(const rex::ui::OverlayContext& context) : ImGuiDialog(&context.drawer()) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    ImGui::SetNextWindowSize(ImVec2(200, 100));
    if (ImGui::Begin("contract")) {
      ImGui::TextUnformatted("contract");
    }
    ImGui::End();
  }
};

REX_OVERLAY_TABLE(kContractOverlays, REX_OVERLAY(ShownDialog, "contract", "F10"));

class NullGPUApp : public rex::ui::TitleApp {
 public:
  rex::gpu::IPresenter* TitlePresenter() { return api().Get<rex::gpu::IBackend>()->presenter(); }

 protected:
  rex::Result<void> OnConfigure(rex::Config& config) override {
    config.modules = {"rexmod-test-null-gpu"};
    config.ui.video_driver = "dummy";
    config.paths.config_file.clear();
    return rex::Ok();
  }
  void OnCreateWindow(rex::ui::TitleUI& ui) override {
    REQUIRE(ui.overlays().Add(kContractOverlays));
    ui.overlays().Show("contract", true);
  }
};

class NullGPUModule {
 public:
  NullGPUModule() {
    REQUIRE(library_.Load(
        rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexmod-test-null-gpu"),
        rex::platform::SymbolResolution::kImmediate));
    library_.GetSymbol<void (*)(RexModTestSink)>("rexmod_test_set_sink")(&Sink);
  }
  void FailTextures(int count) {
    library_.GetSymbol<void (*)(int)>("rexmod_test_null_gpu_fail_textures")(count);
  }

 private:
  rex::platform::DynamicLibrary library_;
};

bool Open(NullGPUApp& app) {
  if (!app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}), rex::PPCImageInfo{})) {
    return false;
  }
  return bool(app.CreateTitleWindow());
}

std::vector<std::string> After(const std::string& first) {
  auto it = std::ranges::find(g_events, first);
  return {it, g_events.end()};
}

}  // namespace

TEST_CASE("The title window draws 1 overlay frame through the backend drawer", "[gpu][overlay]") {
  g_events.clear();
  NullGPUModule module;
  NullGPUApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  CHECK(std::ranges::count(g_events, std::string("create_drawer")) == 1);
  REQUIRE(app.TitlePresenter() != nullptr);
  app.TitlePresenter()->Paint(true);
  g_events.clear();
  app.TitlePresenter()->Paint(true);
  const auto frame = After("begin");
  REQUIRE(frame.size() >= 5);
  CHECK(frame.front() == "begin");
  CHECK(std::ranges::find(frame, "begin_batch") != frame.end());
  CHECK(std::ranges::find(frame, "draw") != frame.end());
  CHECK(std::ranges::find(frame, "end_batch") != frame.end());
  CHECK(frame.back() == "end");
  REQUIRE(app.Shutdown() == 0);
  CHECK(std::ranges::count(g_events, std::string("texture_freed")) >= 1);
}

TEST_CASE("A failed font texture is created again on the next frame", "[gpu][overlay]") {
  g_events.clear();
  NullGPUModule module;
  module.FailTextures(1);
  NullGPUApp app;
  if (!Open(app)) {
    module.FailTextures(0);
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.TitlePresenter() != nullptr);
  CHECK(std::ranges::count(g_events, std::string("create_texture")) == 0);
  app.TitlePresenter()->Paint(true);
  CHECK(std::ranges::count(g_events, std::string("create_texture_failed")) == 1);
  CHECK(std::ranges::count(g_events, std::string("create_texture")) == 1);
  REQUIRE(app.Shutdown() == 0);
}
