/**
 * @file        tests/unit/ui/title_app_test.cpp
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
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/hid/key_chord.h>
#include <rex/logging.h>
#include <rex/platform/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay.h>
#include <rex/ui/title_app.h>

#include "ui/sdl3/app_context.h"

namespace rex::ui {

struct TitleAppProbe {
  static bool Event(TitleApp& app, const UIEvent& event) { return app.OnWindowEvent(event); }
};

}  // namespace rex::ui

namespace {

std::atomic<rex::i32> g_constructed{0};
std::atomic<rex::i32> g_destroyed{0};

class TestOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TestOverlay(const rex::ui::OverlayContext&) { ++g_constructed; }
  ~TestOverlay() override { ++g_destroyed; }
};

constexpr rex::hid::KeyChord kTestKey{rex::hid::VirtualKey::kF9, rex::hid::KeyModifiers::Ctrl};

REX_OVERLAY_TABLE(kTestOverlays, REX_OVERLAY(TestOverlay, "test", kTestKey));

class FooApp : public rex::ui::TitleApp {
 public:
  std::vector<std::string> modules{"rex-ui-sdl3"};
  bool veto_close = false;
  rex::i32 close_events = 0;
  rex::i32 closing_events = 0;
  rex::i32 closing_events_at_shutdown = -1;
  bool dialogs_at_closing = false;
  bool created = false;
  rex::i32 events_before_create = 0;
  using TitleApp::config;
  using TitleApp::GetWindow;
  using TitleApp::UI;

  void ExitTitle(rex::runtime::TitleExitReason reason) { OnTitleExit(reason); }

 protected:
  [[nodiscard]] rex::Result<void> OnConfigure(rex::Config& config) override {
    config.modules = modules;
    config.ui.video_driver = "dummy";
    config.paths.config_file.clear();
    return rex::Ok();
  }
  void OnCreateWindow(rex::ui::TitleUI& title_ui) override {
    REQUIRE(title_ui.Overlays().AddOverlays(kTestOverlays));
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
      dialogs_at_closing = UI().HasDialogs();
    }
    return false;
  }
  void OnShutdown() override {
    if (!created) {
      return;
    }
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

rex::ui::PromptDesc TestPrompt() {
  return {.title = "Test", .body = "Body"};
}

bool PumpUntil(FooApp& app, const std::function<bool()>& ready) {
  for (rex::i32 frame = 0; frame < 500 && !ready(); ++frame) {
    app.RunFrame();
  }
  return ready();
}

std::atomic<rex::i32> g_dialogs_destroyed{0};

class CountedDialog final : public rex::ui::ImGuiDialog {
 public:
  ~CountedDialog() override { ++g_dialogs_destroyed; }
  using ImGuiDialog::Close;
};

}  // namespace

REX_TEST_CASE("A title window opens and Shutdown joins every thread", "[title_app]") {
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

REX_TEST_CASE("A consumed Close keeps the app running", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  const rex::ui::UIEvent close(rex::ui::UIEvent::Type::Close, nullptr);
  app.veto_close = true;
  REQUIRE(rex::ui::TitleAppProbe::Event(app, close));
  REQUIRE(app.close_events == 1);
  REQUIRE_FALSE(rex::ShutdownRequested());
  app.veto_close = false;
  REQUIRE_FALSE(rex::ui::TitleAppProbe::Event(app, close));
  REQUIRE(app.close_events == 2);
  REQUIRE(rex::ShutdownRequested());
  REQUIRE(app.Shutdown() == 0);
}

REX_TEST_CASE("InvokeFunction runs on the UI thread and fails after Shutdown", "[title_app]") {
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

REX_TEST_CASE("LaunchTitle without a game directory fails", "[title_app]") {
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

REX_TEST_CASE("Shutdown closes the window and reports Closing before OnShutdown", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(app.closing_events == 1);
  REQUIRE(app.closing_events_at_shutdown == 1);
}

REX_TEST_CASE("A prompt pending on a guest thread completes cancelled when Stop runs",
              "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  const auto timeout = app.config().runtime.terminate_timeout;
  const auto ui_thread = std::this_thread::get_id();
  std::atomic<bool> completed{false};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> on_ui{false};
  rex::platform::Fence fence;
  std::jthread app_thread([&] {
    app.UI().Prompt(TestPrompt(), [&](const rex::ui::PromptResult& result) {
      cancelled = result.cancelled;
      on_ui = std::this_thread::get_id() == ui_thread;
      completed = true;
      fence.Signal();
    });
    fence.Wait();
  });
  REQUIRE(PumpUntil(app, [&] { return app.UI().HasDialogs(); }));
  const auto start = std::chrono::steady_clock::now();
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(std::chrono::steady_clock::now() - start < timeout);
  app_thread.join();
  REQUIRE(completed);
  REQUIRE(cancelled);
  REQUIRE(on_ui);
}

REX_TEST_CASE("Prompt after the UI loop quits completes cancelled at once", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  app.UI().Quit();
  app.RunFrame();
  std::atomic<bool> completed{false};
  std::atomic<bool> cancelled{false};
  std::jthread worker([&] {
    app.UI().Prompt(TestPrompt(), [&](const rex::ui::PromptResult& result) {
      cancelled = result.cancelled;
      completed = true;
    });
  });
  worker.join();
  REQUIRE(completed);
  REQUIRE(cancelled);
  REQUIRE_FALSE(app.UI().HasDialogs());
  REQUIRE(app.Shutdown() == 0);
}

REX_TEST_CASE("Stop destroys the prompts before the window closes", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  std::atomic<bool> completed{false};
  app.UI().Prompt(TestPrompt(), [&](const rex::ui::PromptResult&) { completed = true; });
  REQUIRE(PumpUntil(app, [&] { return app.UI().HasDialogs(); }));
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(completed);
  REQUIRE(app.closing_events == 1);
  REQUIRE_FALSE(app.dialogs_at_closing);
}

REX_TEST_CASE("CloseWindow leaves a dialog its owner holds to the owner", "[title_app]") {
  g_dialogs_destroyed = 0;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  auto owned = std::make_unique<CountedDialog>();
  app.UI().AddDialog(*owned);
  REQUIRE(app.UI().HasDialogs());
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_dialogs_destroyed == 0);
  owned.reset();
  REQUIRE(g_dialogs_destroyed == 1);
}

REX_TEST_CASE("Closing the window destroys the overlays", "[title_app]") {
  g_constructed = 0;
  g_destroyed = 0;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  app.UI().ShowOverlay("test", true);
  app.UI().CloseWindow();
  REQUIRE(g_destroyed == 1);
  REQUIRE(app.UI().Overlay("test") == nullptr);
  REQUIRE_FALSE(app.UI().AnyOverlayVisible());
  REQUIRE_FALSE(app.api().Get<rex::cvar::ICvarRegistry>()->Find("bind.test"));
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_constructed == 1);
  REQUIRE(g_destroyed == 1);
}

REX_TEST_CASE("Overlay tables bind, toggle without destroying, and die with the app",
              "[title_app]") {
  g_constructed = 0;
  g_destroyed = 0;
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.api().Get<rex::cvar::ICvarRegistry>()->Find("bind.test"));
  REQUIRE(app.api().Get<rex::cvar::ICvarRegistry>()->Get("bind.test").value() == "Ctrl+F9");
  app.UI().ToggleOverlay("test");
  app.UI().ToggleOverlay("test");
  REQUIRE(g_constructed == 1);
  REQUIRE(g_destroyed == 0);
  app.Shutdown();
  REQUIRE(g_destroyed == 1);
}

REX_TEST_CASE("RequestShutdown before Run ends Run at once", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  rex::RequestShutdown(4);
  app.Run();
  REQUIRE(app.Shutdown() == 4);
}

REX_TEST_CASE("A shutdown requested during Init skips the window and the title", "[title_app]") {
  FooApp app;
  REQUIRE(app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}), rex::PPCImageInfo{}));
  rex::RequestShutdown(6);
  REQUIRE(app.CreateTitleWindow());
  REQUIRE(app.LaunchTitle());
  app.Run();
  REQUIRE_FALSE(app.created);
  REQUIRE(app.Shutdown() == 6);
}

REX_TEST_CASE("OnEvent receives nothing before OnCreateWindow returns", "[title_app]") {
  FooApp app;
  if (!Open(app)) {
    app.Shutdown();
    SKIP("SDL dummy video driver unavailable");
  }
  REQUIRE(app.events_before_create == 0);
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(app.closing_events == 1);
}

REX_TEST_CASE("TitleApp puts rex-filesystem and rex-runtime first", "[title_app]") {
  FooApp app;
  app.modules = {"rexmod-test-good"};
  REQUIRE(app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}), rex::PPCImageInfo{}));
  REQUIRE(app.config().modules ==
          std::vector<std::string>{"rex-filesystem", "rex-runtime", "rexmod-test-good"});
  REQUIRE(app.Shutdown() == 0);
}

REX_TEST_CASE("A title app with no ui module runs headless and Shutdown returns 0",
              "[title_app][headless]") {
  FooApp app;
  app.modules = {};
  REQUIRE(Open(app));
  REQUIRE(app.api().Get<rex::ui::IUserInterface>() == nullptr);
  REQUIRE(app.UI().GetWindow() == nullptr);
  REQUIRE_FALSE(app.created);
  bool cancelled = false;
  app.UI().Prompt(TestPrompt(),
                  [&](const rex::ui::PromptResult& result) { cancelled = result.cancelled; });
  REQUIRE(cancelled);
  REQUIRE(app.RunFrame());
  rex::RequestShutdown(0);
  REQUIRE_FALSE(app.RunFrame());
  REQUIRE(app.Shutdown() == 0);
}

REX_TEST_CASE("InvokeFunction from a guest thread runs on the null provider loop",
              "[title_app][headless]") {
  FooApp app;
  app.modules = {};
  REQUIRE(Open(app));
  const auto loop_thread = std::this_thread::get_id();
  std::atomic<bool> posted{false};
  std::atomic<bool> ran_on_loop{false};
  std::jthread app_thread([&] {
    posted = rex::ui::InvokeFunction([&] {
      ran_on_loop = std::this_thread::get_id() == loop_thread;
      app.ExitTitle(rex::runtime::TitleExitReason::NormalExit);
    });
  });
  app.Run();
  app_thread.join();
  REQUIRE(posted);
  REQUIRE(ran_on_loop);
  REQUIRE(rex::ShutdownRequested());
  REQUIRE(app.Shutdown() == 0);
}

namespace {

class InertAppContext final : public rex::ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

}  // namespace

REX_TEST_CASE("A pending UI function that throws at shutdown is logged", "[ui][app_context]") {
  rex::Shutdown();
  auto started = rex::Init({});
  REQUIRE(started);
  auto* logger = (*started)->Get<rex::ILogger>();
  REQUIRE(logger);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  bool later_ran = false;
  {
    InertAppContext context;
    std::thread([&] {
      context.CallInUIThreadDeferred([] { throw std::runtime_error("pending function failed"); });
      context.CallInUIThreadDeferred([&] { later_ran = true; });
    }).join();
  }
  std::vector<rex::log::CapturedRecord> records;
  sink.CopySince(0, records);
  logger->RemoveSink(sink);
  rex::Shutdown();
  CHECK(std::ranges::any_of(records, [](const rex::log::CapturedRecord& record) {
    return record.text == "teardown step failed, pending function failed";
  }));
  CHECK(later_ran);
}
