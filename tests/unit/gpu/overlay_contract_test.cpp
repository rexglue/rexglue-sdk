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

#include "rex_test.h"

#include <algorithm>
#include <atomic>
#include <memory>
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
#include "modules/test_module_sink.h"

namespace {

std::vector<std::string> g_events;
std::atomic<rex::i32> g_owned_destroyed{0};

void Sink(const char*, const char* event) {
  g_events.emplace_back(event);
}

class ShownDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ShownDialog(const rex::ui::OverlayContext&) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    ImGui::SetNextWindowSize(ImVec2(200, 100));
    if (ImGui::Begin("contract")) {
      ImGui::TextUnformatted("contract");
    }
    ImGui::End();
  }
};

class OwnedDialog final : public rex::ui::ImGuiDialog {
 public:
  ~OwnedDialog() override { ++g_owned_destroyed; }
  using ImGuiDialog::Close;

 protected:
  void OnDraw(ImGuiIO&) override {
    ImGui::SetNextWindowSize(ImVec2(200, 100));
    if (ImGui::Begin("owned")) {
      ImGui::TextUnformatted("owned");
    }
    ImGui::End();
  }
};

REX_OVERLAY_TABLE(kContractOverlays,
                  REX_OVERLAY(ShownDialog, "contract",
                              ::rex::hid::KeyChord{::rex::hid::VirtualKey::kF10}));

class TestDrawContext final : public rex::gpu::OverlayDrawContext {
 public:
  explicit TestDrawContext(rex::gpu::IPresenter* presenter)
      : OverlayDrawContext(presenter, 1280, 720) {}
};

class NullGPUApp : public rex::ui::TitleApp {
 public:
  using TitleApp::GetUI;
  rex::gpu::IPresenter* GetTitlePresenter() {
    return GetAPI().Get<rex::gpu::IGPUBackend>()->GetPresenter();
  }

 protected:
  [[nodiscard]] rex::Result<void> OnConfigure(rex::Config& config) override {
    config.modules = {"rexmod-test-null-gpu", "rexui-sdl3"};
    config.ui.video_driver = "dummy";
    config.paths.config_file.clear();
    return rex::Ok();
  }
  void OnCreateWindow(rex::ui::TitleUI& title_ui) override {
    REQUIRE(title_ui.GetUI().AddOverlays(kContractOverlays));
    title_ui.GetUI().ShowOverlay("contract", true);
  }
};

class NullGPUModule {
 public:
  NullGPUModule() {
    REQUIRE(library_.Load(
        rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexmod-test-null-gpu"),
        rex::platform::SymbolResolution::kImmediate));
    library_.GetSymbol<void (*)(ReXModTestSink)>("rexmod_test_set_sink")(&Sink);
  }
  void FailTextures(rex::i32 count) {
    library_.GetSymbol<void (*)(rex::i32)>("rexmod_test_null_gpu_fail_textures")(count);
  }

 private:
  rex::platform::DynamicLibrary library_;
};

bool Open(NullGPUApp& app) {
  if (!app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}), rex::PPCImage{})) {
    return false;
  }
  return static_cast<bool>(app.CreateTitleWindow());
}

std::vector<std::string> After(const std::string& first) {
  auto it = std::ranges::find(g_events, first);
  return {it, g_events.end()};
}

}  // namespace

REX_TEST_CASE("The title window draws 1 overlay frame through the backend drawer",
              "[gpu][overlay]") {
  g_events.clear();
  NullGPUModule module;
  NullGPUApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  CHECK(std::ranges::count(g_events, std::string("create_drawer")) == 1);
  REQUIRE(app.GetTitlePresenter() != nullptr);
  app.GetTitlePresenter()->Paint(true);
  g_events.clear();
  app.GetTitlePresenter()->Paint(true);
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

REX_TEST_CASE("The title window reports its SDL window", "[gpu][overlay]") {
  g_events.clear();
  NullGPUModule module;
  NullGPUApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  CHECK(app.GetUI().GetWindow()->GetNative().sdl != nullptr);
  ImGuiIO* io = app.GetUI().GetImGuiIO();
  REQUIRE(io != nullptr);
  CHECK(io->Fonts == app.GetUI().GetFonts());
  REQUIRE(app.Shutdown() == 0);
}

REX_TEST_CASE("DrawOverlays draws the shown overlays through the overlay drawer",
              "[gpu][overlay]") {
  g_events.clear();
  NullGPUModule module;
  NullGPUApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.GetTitlePresenter() != nullptr);
  REQUIRE(app.GetUI().GetOverlayDrawer() != nullptr);
  app.GetTitlePresenter()->Paint(true);
  g_events.clear();
  TestDrawContext context(app.GetTitlePresenter());
  app.GetUI().DrawOverlays(context);
  const auto begin = std::ranges::find(g_events, std::string("begin_batch"));
  const auto draw = std::ranges::find(g_events, std::string("draw"));
  const auto end = std::ranges::find(g_events, std::string("end_batch"));
  REQUIRE(begin != g_events.end());
  REQUIRE(draw != g_events.end());
  REQUIRE(end != g_events.end());
  CHECK(begin < draw);
  CHECK(draw < end);
  REQUIRE(app.Shutdown() == 0);
}

REX_TEST_CASE("A failed font texture is created again on the next frame", "[gpu][overlay]") {
  g_events.clear();
  NullGPUModule module;
  module.FailTextures(1);
  NullGPUApp app;
  if (!Open(app)) {
    module.FailTextures(0);
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.GetTitlePresenter() != nullptr);
  CHECK(std::ranges::count(g_events, std::string("create_texture")) == 0);
  app.GetTitlePresenter()->Paint(true);
  CHECK(std::ranges::count(g_events, std::string("create_texture_failed")) == 1);
  CHECK(std::ranges::count(g_events, std::string("create_texture")) == 1);
  REQUIRE(app.Shutdown() == 0);
}

REX_TEST_CASE("A dialog closed while its owner holds it survives the frame", "[gpu][overlay]") {
  g_events.clear();
  g_owned_destroyed = 0;
  NullGPUModule module;
  NullGPUApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.GetTitlePresenter() != nullptr);
  app.GetUI().ShowOverlay("contract", false);
  REQUIRE_FALSE(app.GetUI().HasDialogs());
  auto dialog = std::make_unique<OwnedDialog>();
  app.GetUI().AddDialog(*dialog);
  REQUIRE(app.GetUI().HasDialogs());
  dialog->Close();
  app.GetTitlePresenter()->Paint(true);
  CHECK(dialog->IsClosed());
  CHECK_FALSE(app.GetUI().HasDialogs());
  CHECK(g_owned_destroyed == 0);
  dialog.reset();
  CHECK(g_owned_destroyed == 1);
  REQUIRE(app.Shutdown() == 0);
}
