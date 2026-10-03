/**
 * @file        tests/unit/ui/title_app_test.cpp
 * @brief       Unit tests for TitleApp without a title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_events.h>

#include <rex/cvar/registry.h>
#include <rex/platform/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay.h>
#include <rex/ui/title_app.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/window_sdl.h>

#include "kernel/xam/xam_dialog.h"

namespace {

std::atomic<rex::i32> g_constructed{0};
std::atomic<rex::i32> g_destroyed{0};

class TestOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TestOverlay(const rex::ui::OverlayContext& context) : ImGuiDialog(&context.drawer()) {
    ++g_constructed;
  }
  ~TestOverlay() override { ++g_destroyed; }
};

std::atomic<rex::i32> g_dialogs_destroyed{0};

class CountedDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit CountedDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  ~CountedDialog() override { ++g_dialogs_destroyed; }
};

REX_OVERLAY_TABLE(kTestOverlays, REX_OVERLAY(TestOverlay, "test", "F9"));

class FooApp : public rex::ui::TitleApp {
 public:
  std::vector<std::string> modules;
  bool veto_close = false;
  rex::i32 close_events = 0;
  rex::i32 closing_events = 0;
  rex::i32 closing_events_at_shutdown = -1;
  bool dialogs_at_closing = false;
  bool created = false;
  rex::i32 events_before_create = 0;
  rex::ui::Window::Phase phase_at_shutdown = rex::ui::Window::Phase::kOpen;
  using TitleApp::config;
  using TitleApp::drawer;
  using TitleApp::overlays;
  using TitleApp::window;

 protected:
  rex::Result<void> OnConfigure(rex::Config& config) override {
    config.modules = modules;
    config.ui.video_driver = "dummy";
    config.paths.config_file.clear();
    return rex::Ok();
  }
  void OnCreateWindow(rex::ui::TitleUI& ui) override {
    REQUIRE(ui.overlays().Add(kTestOverlays));
    created = true;
  }
  bool OnEvent(const rex::ui::UIEvent& event) override {
    if (!created) {
      ++events_before_create;
    }
    if (event.type() == rex::ui::UIEvent::Type::Close) {
      ++close_events;
      return veto_close;
    }
    if (event.type() == rex::ui::UIEvent::Type::Closing) {
      ++closing_events;
      dialogs_at_closing = drawer().HasDialogs();
    }
    return false;
  }
  void OnShutdown() override {
    if (!created) {
      return;
    }
    phase_at_shutdown = window().phase();
    closing_events_at_shutdown = closing_events;
  }
};

bool Open(FooApp& app) {
  auto initialized =
      app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}), rex::PPCImageInfo{});
  if (!initialized) {
    WARN("Init failed: " << initialized.error().message);
    return false;
  }
  auto created = app.CreateTitleWindow();
  if (!created) {
    WARN("CreateTitleWindow failed: " << created.error().message);
    return false;
  }
  return true;
}

void PushClose(FooApp& app) {
  SDL_Event event{};
  event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
  static_cast<rex::ui::WindowSDL&>(app.window()).HandleWindowEvent(event);
}

}  // namespace

TEST_CASE("A title window opens and Shutdown joins every thread", "[title_app]") {
  rex::platform::detail::ShutdownTimerQueue();
  const rex::u64 before = rex::platform::detail::LiveCount();
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.RunFrame());
  rex::RequestShutdown(0);
  REQUIRE_FALSE(app.RunFrame());
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(rex::platform::detail::LiveCount() == before);
}

TEST_CASE("A consumed Close keeps the app running", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  app.veto_close = true;
  PushClose(app);
  app.RunFrame();
  REQUIRE(app.close_events == 1);
  REQUIRE_FALSE(rex::ShutdownRequested());
  app.veto_close = false;
  PushClose(app);
  app.RunFrame();
  REQUIRE(rex::ShutdownRequested());
  REQUIRE(app.Shutdown() == 0);
}

TEST_CASE("InvokeFunction runs on the UI thread and fails after Shutdown", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  const auto ui_thread = std::this_thread::get_id();
  std::atomic<bool> ran_on_ui{false};
  std::jthread worker([&] {
    REQUIRE(rex::ui::InvokeFunction([&] { ran_on_ui = std::this_thread::get_id() == ui_thread; }));
  });
  worker.join();
  app.RunFrame();
  REQUIRE(ran_on_ui);
  app.Shutdown();
  REQUIRE_FALSE(rex::ui::InvokeFunction([] {}));
}

TEST_CASE("LaunchTitle without a game directory fails", "[title_app]") {
  const rex::u64 before = rex::platform::detail::LiveCount();
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE_FALSE(app.LaunchTitle());
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(rex::platform::detail::LiveCount() == before);
}

TEST_CASE("Shutdown closes an open window before destroying it", "[title_app]") {
  struct ClosingProbe final : rex::ui::WindowListener {
    rex::i32 closing = 0;
    void OnClosing(rex::ui::UIEvent&) override { ++closing; }
  } probe;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  app.window().AddListener(&probe);
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(probe.closing == 1);
}

TEST_CASE("Shutdown releases a thread blocked on a dialog fence", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  std::atomic<bool> released{false};
  std::jthread blocked([&] {
    rex::platform::Fence fence;
    REQUIRE(rex::ui::InvokeFunction([&] {
      rex::ui::ImGuiDialog::ShowMessageBox(&app.drawer(), "Test", "Body")->Then(&fence);
    }));
    fence.Wait();
    released = true;
  });
  while (!app.drawer().HasDialogs()) {
    app.RunFrame();
  }
  app.Shutdown();
  blocked.join();
  REQUIRE(released);
}

TEST_CASE("Stop closes the window and reports Closing before OnShutdown", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(app.phase_at_shutdown == rex::ui::Window::Phase::kClosedOpenable);
  REQUIRE(app.closing_events_at_shutdown == 1);
}

TEST_CASE("DestroyDialogs leaves owned dialogs to their owner", "[title_app]") {
  g_dialogs_destroyed = 0;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  auto owned = std::make_unique<CountedDialog>(&app.drawer());
  owned->SetOwned(true);
  new CountedDialog(&app.drawer());
  app.drawer().DestroyDialogs();
  REQUIRE(g_dialogs_destroyed == 1);
  REQUIRE(app.drawer().HasDialogs());
  owned.reset();
  REQUIRE(g_dialogs_destroyed == 2);
  REQUIRE(app.Shutdown() == 0);
}

TEST_CASE("Stop destroys the drawer's dialogs before the window closes", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  new CountedDialog(&app.drawer());
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(app.closing_events == 1);
  REQUIRE_FALSE(app.dialogs_at_closing);
}

TEST_CASE("Closing the window destroys the overlays", "[title_app]") {
  g_constructed = 0;
  g_destroyed = 0;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  app.overlays().Show("test", true);
  app.window().RequestClose();
  REQUIRE(g_destroyed == 1);
  REQUIRE(app.overlays().Get("test") == nullptr);
  REQUIRE_FALSE(app.overlays().AnyVisible());
  REQUIRE(app.api().Get<rex::cvar::ICvarRegistry>()->Find("bind.test") == nullptr);
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_constructed == 1);
  REQUIRE(g_destroyed == 1);
}

TEST_CASE("Overlay tables bind, toggle without destroying, and die with the app", "[title_app]") {
  g_constructed = 0;
  g_destroyed = 0;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.api().Get<rex::cvar::ICvarRegistry>()->Find("bind.test") != nullptr);
  app.overlays().Toggle("test");
  app.overlays().Toggle("test");
  REQUIRE(g_constructed == 1);
  REQUIRE(g_destroyed == 0);
  app.Shutdown();
  REQUIRE(g_destroyed == 1);
}

TEST_CASE("RequestShutdown before Run ends Run at once", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  rex::RequestShutdown(4);
  app.Run();
  REQUIRE(app.Shutdown() == 4);
}

TEST_CASE("A XAM dialog is built on the UI thread and Shutdown releases it", "[title_app]") {
  g_dialogs_destroyed = 0;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  const auto ui_thread = std::this_thread::get_id();
  rex::ui::WindowedAppContext* context = &app.window().app_context();
  std::atomic<bool> shown{false};
  std::atomic<bool> built_on_ui{false};
  std::atomic<bool> released{false};
  std::jthread worker([&] {
    rex::platform::Fence fence;
    shown = rex::kernel::xam::ShowDialog(
        context,
        [&]() -> rex::ui::ImGuiDialog* {
          built_on_ui = std::this_thread::get_id() == ui_thread;
          return new CountedDialog(&app.drawer());
        },
        fence);
    if (shown) {
      fence.Wait();
    }
    released = true;
  });
  for (rex::i32 frame = 0; frame < 500 && !app.drawer().HasDialogs() && !released; ++frame) {
    app.RunFrame();
  }
  app.Shutdown();
  worker.join();
  REQUIRE(shown);
  REQUIRE(built_on_ui);
  REQUIRE(released);
  REQUIRE(g_dialogs_destroyed == 1);
}

TEST_CASE("ShowDialog after the UI loop quits builds nothing", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  rex::ui::WindowedAppContext* context = &app.window().app_context();
  context->QuitFromUIThread();
  std::atomic<bool> shown{true};
  std::atomic<bool> built{false};
  std::jthread worker([&] {
    rex::platform::Fence fence;
    shown = rex::kernel::xam::ShowDialog(
        context,
        [&]() -> rex::ui::ImGuiDialog* {
          built = true;
          return new CountedDialog(&app.drawer());
        },
        fence);
  });
  worker.join();
  REQUIRE_FALSE(shown);
  REQUIRE_FALSE(built);
  REQUIRE(app.Shutdown() == 0);
}

TEST_CASE("A shutdown requested during Init skips the window and the title", "[title_app]") {
  FooApp app;
  REQUIRE(app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}), rex::PPCImageInfo{}));
  rex::RequestShutdown(6);
  REQUIRE(app.CreateTitleWindow());
  REQUIRE(app.LaunchTitle());
  app.Run();
  REQUIRE_FALSE(app.created);
  REQUIRE(app.Shutdown() == 6);
}

TEST_CASE("OnEvent receives nothing before OnCreateWindow returns", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.events_before_create == 0);
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(app.closing_events == 1);
}

TEST_CASE("TitleApp puts rexfilesystem and rexruntime first", "[title_app]") {
  FooApp app;
  app.modules = {"rexmod-test-good"};
  REQUIRE(app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}), rex::PPCImageInfo{}));
  REQUIRE(app.config().modules ==
          std::vector<std::string>{"rexfilesystem", "rexruntime", "rexmod-test-good"});
  REQUIRE(app.Shutdown() == 0);
}
