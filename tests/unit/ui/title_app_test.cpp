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
#include "object_app.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <rex/app/app.h>
#include <rex/path_settings.h>
#include <rex/cli.h>
#include <rex/platform/thread.h>
#include <rex/app_info.h>
#include <rex/platform/sync.h>
#include <rex/ui/ui_system.h>
#include <rex/app/title_app.h>

#include "app/lifecycle.h"
#include "kernel/title_entries.h"
#include "kernel/title_fakes.h"
#include "kernel/title_image.h"
#include "kernel/settings_access.h"
#include "temp_folder.h"
#include "ui/ui_harness.h"

namespace {

using rex::test_ui::g_ui;

std::atomic<rex::i32> g_overlays_constructed{0};
std::atomic<rex::i32> g_overlays_destroyed{0};
std::atomic<rex::i32> g_dialogs_destroyed{0};

class TestOverlay final : public rex::ui::DialogBase {
 public:
  TestOverlay() { ++g_overlays_constructed; }
  ~TestOverlay() override { ++g_overlays_destroyed; }

  bool Draw(ImGuiIO&) override { return true; }
};

std::unique_ptr<rex::ui::IDialog> CreateTestOverlay(rex::ui::IUISystem&) {
  return std::make_unique<TestOverlay>();
}

constexpr rex::ui::OverlayConfig kTestOverlays[] = {
    {"test", {rex::VirtualKey::kF9, rex::KeyModifiers::kCtrl}, &CreateTestOverlay},
};

class CountedDialog final : public rex::ui::DialogBase {
 public:
  ~CountedDialog() override { ++g_dialogs_destroyed; }

  bool Draw(ImGuiIO&) override { return true; }
};

class FooApp : public rex::test::ObjectApp<rex::app::TitleApp> {
 public:
  explicit FooApp(bool display = true, const rex::ppc::PPCImage& image = {})
      : ObjectApp("foo", image), display_(display) {
    static_cast<rex::test_ui::FakeUIData&>(g_ui) = rex::test_ui::FakeUIData{};
    g_ui.has_display = display;
  }

  std::filesystem::path game_root;

 protected:
  rex::AppSysRequirements GetRequirements() const override { return {}; }

  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
  }

  [[nodiscard]] rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    REX_TRY_VOID(REX_PATHS().config_file.SetText(rex::SettingLayer::kTitle, ""));
    REX_TRY_VOID(
        settings.SetText(rex::SettingLayer::kTitle, "title.game_root", game_root.string()));
    REX_TRY_VOID(GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle,
                                                      display_ ? "fake" : "none"));
    return rex::Ok();
  }

 private:
  bool display_;
  rex::test_ui::FakeUIProvider provider_;
};

bool Open(FooApp& app) {
  auto initialized = app.Init(rex::CLIArgList::Parse(std::span<const std::string>{}));
  if (!initialized) {
    WARN("Init failed: " << initialized.error().message);
    return false;
  }
  return true;
}

rex::ui::PromptConfig TestPrompt() {
  return {.title = "Test", .body = "Body"};
}

bool PumpUntil(FooApp& app, const std::function<bool()>& ready) {
  for (rex::i32 frame = 0; frame < 500 && !ready(); ++frame) {
    app.RunFrame();
  }
  return ready();
}

}  // namespace

REX_TEST_CASE("A title window opens and Shutdown closes it", "[ui][title_app]") {
  FooApp app;
  REQUIRE(Open(app));
  CHECK(app.GetUI().HasWindow());
  CHECK(g_ui.window_open);
  REQUIRE(app.Shutdown() == 0);
  CHECK_FALSE(g_ui.window_open);
}

REX_TEST_CASE("LaunchTitle without a game directory fails", "[ui][title_app]") {
  const rex::u64 before = rex::platform::detail::GetThreadCount();
  {
    FooApp app;
    REQUIRE(Open(app));
    REQUIRE_FALSE(app.LaunchTitle());
    REQUIRE(app.Shutdown() == 1);
  }
  REQUIRE(rex::platform::detail::GetThreadCount() == before);
}

REX_TEST_CASE("LaunchTitle finds an entrypoint whose file name differs in case",
              "[ui][title_app]") {
  const rex::test::TempFolder folder("rexglue_title_app_case_test_");
  const std::filesystem::path& root = folder.path;
  { std::ofstream(root / "DEFAULT.XEX", std::ios::binary) << "not an xex"; }
  const rex::ppc::PPCImage image = {
      .code_base = 0x82000000,
      .code_size = 0x1000,
      .image_base = 0x82000000,
      .image_size = 0x10000,
      .func_mappings = nullptr,
  };
  FooApp app(true, image);
  app.game_root = root;
  REQUIRE(Open(app));
  auto launched = app.LaunchTitle();
  REQUIRE_FALSE(launched);
  CHECK(launched.error().message.find("was not found") == std::string::npos);
  app.Shutdown();
}

REX_TEST_CASE("A prompt pending on a guest thread completes cancelled when Stop runs",
              "[ui][title_app]") {
  FooApp app;
  REQUIRE(Open(app));
  const std::chrono::milliseconds timeout(
      rex::kernel::GetRuntimeSettings().terminate_timeout_ms.Get());
  const auto ui_thread = std::this_thread::get_id();
  std::atomic<bool> completed{false};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> on_ui{false};
  rex::platform::Fence fence;
  std::jthread app_thread([&] {
    app.GetUI().ShowPrompt(TestPrompt(), [&](const rex::ui::PromptResult& result) {
      cancelled = result.canceled;
      on_ui = std::this_thread::get_id() == ui_thread;
      completed = true;
      fence.Signal();
    });
    fence.Wait();
  });
  REQUIRE(PumpUntil(app, [&] { return app.GetUI().HasDialogs(); }));
  const auto start = std::chrono::steady_clock::now();
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(std::chrono::steady_clock::now() - start < timeout);
  app_thread.join();
  REQUIRE(completed);
  REQUIRE(cancelled);
  REQUIRE(on_ui);
}

REX_TEST_CASE("Prompt after Shutdown completes cancelled at once", "[ui][title_app]") {
  FooApp app;
  REQUIRE(Open(app));
  REQUIRE(app.Shutdown() == 0);
  std::atomic<bool> completed{false};
  std::atomic<bool> cancelled{false};
  std::jthread worker([&] {
    app.GetUI().ShowPrompt(TestPrompt(), [&](const rex::ui::PromptResult& result) {
      cancelled = result.canceled;
      completed = true;
    });
  });
  worker.join();
  REQUIRE(completed);
  REQUIRE(cancelled);
  REQUIRE_FALSE(app.GetUI().HasDialogs());
}

REX_TEST_CASE("Stop destroys the prompts before the window closes", "[ui][title_app]") {
  FooApp app;
  REQUIRE(Open(app));
  std::atomic<bool> completed{false};
  bool window_open_at_completion = false;
  app.GetUI().ShowPrompt(TestPrompt(), [&](const rex::ui::PromptResult&) {
    window_open_at_completion = g_ui.window_open;
    completed = true;
  });
  REQUIRE(PumpUntil(app, [&] { return app.GetUI().HasDialogs(); }));
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(completed);
  CHECK(window_open_at_completion);
  CHECK_FALSE(app.GetUI().HasDialogs());
}

REX_TEST_CASE("Closing the window leaves a dialog its owner holds to the owner",
              "[ui][title_app]") {
  g_dialogs_destroyed = 0;
  FooApp app;
  REQUIRE(Open(app));
  auto owned = std::make_unique<CountedDialog>();
  auto owned_guard = app.GetUI().AddDialog(*owned);
  REQUIRE(app.GetUI().HasDialogs());
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_dialogs_destroyed == 0);
  owned.reset();
  REQUIRE(g_dialogs_destroyed == 1);
}

REX_TEST_CASE("Overlays toggle without destroying and die with the app", "[ui][title_app]") {
  g_overlays_constructed = 0;
  g_overlays_destroyed = 0;
  FooApp app;
  REQUIRE(Open(app));
  REQUIRE(app.GetUI().AddOverlays(kTestOverlays));
  app.GetUI().ToggleOverlay("test");
  CHECK(app.GetUI().IsAnyOverlayVisible());
  app.GetUI().ToggleOverlay("test");
  CHECK_FALSE(app.GetUI().IsAnyOverlayVisible());
  REQUIRE(g_overlays_constructed == 1);
  REQUIRE(g_overlays_destroyed == 0);
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_overlays_destroyed == 1);
}

REX_TEST_CASE("RequestShutdown before Run ends Run at once", "[ui][title_app]") {
  FooApp app;
  REQUIRE(Open(app));
  rex::app::RequestShutdown(4);
  app.Run();
  REQUIRE(app.Shutdown() == 4);
}

REX_TEST_CASE("A shutdown requested during Init skips the title", "[ui][title_app]") {
  FooApp app;
  REQUIRE(Open(app));
  rex::app::RequestShutdown(6);
  REQUIRE(app.LaunchTitle());
  app.Run();
  REQUIRE(app.Shutdown() == 6);
}

REX_TEST_CASE(
    "A title app with no UI requirement and no display runs headless and Shutdown returns 0",
    "[ui][title_app][headless]") {
  FooApp app(false);
  REQUIRE(Open(app));
  REQUIRE_FALSE(app.GetUI().HasWindow());
  bool cancelled = false;
  app.GetUI().ShowPrompt(TestPrompt(),
                         [&](const rex::ui::PromptResult& result) { cancelled = result.canceled; });
  REQUIRE(cancelled);
  REQUIRE(app.RunFrame());
  rex::app::RequestShutdown(0);
  REQUIRE_FALSE(app.RunFrame());
  REQUIRE(app.Shutdown() == 0);
}

namespace {

using namespace std::chrono_literals;

class Watchdog {
 public:
  explicit Watchdog(std::chrono::seconds limit)
      : thread_([limit](const std::stop_token& stop) {
          std::mutex mutex;
          std::condition_variable_any wake;
          std::unique_lock lock(mutex);
          if (!wake.wait_for(lock, stop, limit, [] { return false; }) && !stop.stop_requested()) {
            std::fputs("the title app did not end in time\n", stderr);
            std::abort();
          }
        }) {}

 private:
  std::jthread thread_;
};

}  // namespace

REX_TEST_CASE("A title that exits on its own ends Run and unloads on the UI thread",
              "[ui][title_app]") {
  rex::test::g_entry.Reset();
  const rex::test::TempFolder folder("rex_title_app_exit_");
  rex::test::WriteTitle(folder.path);
  FooApp app(false, rex::test::MakeTitleImage(rex::test::kReturningEntry));
  app.game_root = folder.path;
  REQUIRE(Open(app));
  Watchdog watchdog(10s);
  REQUIRE(app.LaunchTitle());
  app.Run();
  CHECK(rex::test::g_entry.finished == 1);
  CHECK(app.Shutdown() == 0);
}

REX_TEST_CASE("A shutdown request from another thread unloads a title blocked in a wait",
              "[ui][title_app]") {
  rex::test::g_entry.Reset();
  const rex::test::TempFolder folder("rex_title_app_request_");
  rex::test::WriteTitle(folder.path);
  FooApp app(false, rex::test::MakeTitleImage(rex::test::kEventEntry));
  app.game_root = folder.path;
  REQUIRE(Open(app));
  Watchdog watchdog(10s);
  REQUIRE(app.LaunchTitle());
  std::jthread requester([] {
    while (rex::test::g_entry.handle == 0) {
      std::this_thread::sleep_for(1ms);
    }
    rex::app::RequestShutdown(0);
  });
  app.Run();
  CHECK(app.Shutdown() == 0);
}

namespace {

std::vector<std::string> g_steps;

class StepApp : public rex::test::ObjectApp<rex::app::TitleApp> {
 public:
  explicit StepApp(const rex::ppc::PPCImage& image) : ObjectApp("title_steps", image) {
    rex::test::WriteTitle(folder_.path / "game");
  }

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::span<const std::string>{})); }

  void Restart() { RestartTitle(); }

 protected:
  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
  }

  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    g_steps.push_back("configure");
    REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "kernel.user_language", "0"));
    REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "title.game_root",
                                  (folder_.path / "game").string()));
    REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "app.user_root",
                                  (folder_.path / "user").string()));
    REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "app.cache_root",
                                  (folder_.path / "cache").string()));
    REX_TRY_VOID(REX_PATHS().config_file.SetText(rex::SettingLayer::kTitle,
                                                 (folder_.path / "title_steps.toml").string()));
    REX_TRY_VOID(root.SetText(rex::SettingLayer::kTitle, folder_.path.string()));
    REX_TRY_VOID(GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "count"));
    REX_TRY_VOID(GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "count"));
    REX_TRY_VOID(GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "count"));
    return settings.SetText(rex::SettingLayer::kTitle, "gpu.driver", "count");
  }

  rex::Result<void> OnInit() override {
    g_steps.push_back("init");
    return rex::Ok();
  }

  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override {
    g_steps.push_back(title.IsLoaded() && !title.IsRunning() ? "launch" : "launch out of order");
    return rex::Ok();
  }

  void OnRun() override {
    g_steps.push_back("run");
    rex::app::TitleApp::OnRun();
  }

  void OnShutdown() override { g_steps.push_back("shutdown"); }

 private:
  rex::test::TempFolder folder_{"rex_title_steps_"};
  rex::test::CountingProvider provider_;
};

struct StepFixture {
  StepFixture() {
    g_steps.clear();
    rex::test::g_counts.Reset();
    rex::test::g_entry.Reset();
    static_cast<rex::test_ui::FakeUIData&>(g_ui) = rex::test_ui::FakeUIData{};
  }
};

}  // namespace

REX_TEST_CASE_METHOD(StepFixture, "title app runs its steps in order", "[ui][title_app]") {
  StepApp app(rex::test::MakeTitleImage(rex::test::kDelayingEntry));
  REQUIRE(app.Start());
  REQUIRE(app.LaunchTitle());
  CHECK(app.GetPhase() == rex::app::AppPhase::kTitleRunning);
  REQUIRE(rex::test::WaitFor([] { return rex::test::g_entry.entered == 1; }));
  rex::app::RequestShutdown(0);
  app.Run();
  CHECK(app.Shutdown() == 0);
  CHECK(g_steps == std::vector<std::string>{"configure", "init", "launch", "run", "shutdown"});
  CHECK(rex::test::g_counts.graphics_attach == 1);
  CHECK(rex::test::g_counts.graphics_detach == 1);
}

REX_TEST_CASE_METHOD(StepFixture, "restart relaunches in process", "[ui][title_app]") {
  StepApp app(rex::test::MakeTitleImage(rex::test::kDelayingEntry));
  REQUIRE(app.Start());
  REQUIRE(app.LaunchTitle());
  REQUIRE(rex::test::WaitFor([] { return rex::test::g_entry.entered == 1; }));
  app.Restart();
  CHECK(app.GetPhase() == rex::app::AppPhase::kTitleRunning);
  REQUIRE(rex::test::WaitFor([] { return rex::test::g_entry.entered == 2; }));
  CHECK(rex::test::g_counts.graphics_detach == 1);
  CHECK(rex::test::g_counts.graphics_attach == 2);
  CHECK(app.GetTitleRuntime().IsRunning());
  CHECK(app.Shutdown() == 0);
  CHECK(rex::test::g_counts.graphics_detach == 2);
}

REX_TEST_CASE_METHOD(StepFixture, "title app main runs frames before launch", "[ui][title_app]") {
  StepApp app(rex::test::MakeTitleImage(rex::test::kDelayingEntry));
  REQUIRE(app.Start());
  for (int frame = 0; frame < 3; ++frame) {
    REQUIRE(app.RunFrame());
    CHECK(app.GetPhase() == rex::app::AppPhase::kRunning);
    CHECK(rex::test::g_counts.graphics_attach == 0);
  }
  REQUIRE(app.LaunchTitle());
  CHECK(rex::test::g_counts.graphics_attach == 1);
  CHECK(app.Shutdown() == 0);
}

namespace {

std::atomic<rex::i32> g_startup_boxes{0};

void CountStartupBox(rex::MessageType, std::string_view) {
  ++g_startup_boxes;
}

struct StartupBoxScope {
  StartupBoxScope() {
    g_startup_boxes = 0;
    rex::app::detail::SetStartupBoxForTest(&CountStartupBox);
  }
  ~StartupBoxScope() { rex::app::detail::SetStartupBoxForTest(nullptr); }
};

class FailingStartApp : public rex::test::ObjectApp<rex::app::TitleApp> {
 public:
  explicit FailingStartApp(bool missing_module, bool headless_setting, bool fail_configure = true)
      : ObjectApp("failing_start", {}),
        missing_module_(missing_module),
        headless_setting_(headless_setting),
        fail_configure_(fail_configure) {}

  rex::Result<void> Start(std::vector<std::string> args) {
    return Init(rex::CLIArgList::Parse(std::span<const std::string>(args)));
  }

 protected:
  rex::AppSysRequirements GetRequirements() const override { return {}; }

  void OnModules(std::vector<std::string>& modules) override {
    ObjectApp::OnModules(modules);
    if (missing_module_) {
      modules.push_back("rexmissing");
    }
  }

  [[nodiscard]] rex::Result<void> OnConfigure(rex::SettingsRegistry&) override {
    REX_TRY_VOID(REX_PATHS().config_file.SetText(rex::SettingLayer::kTitle, ""));
    if (headless_setting_) {
      REX_TRY_VOID(GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none"));
    }
    if (fail_configure_) {
      return rex::Err<void>(rex::ErrorCategory::kConfig, "configure failed");
    }
    return rex::Ok();
  }

 private:
  bool missing_module_;
  bool headless_setting_;
  bool fail_configure_;
};

}  // namespace

REX_TEST_CASE("A failed startup with ui.driver none shows no box", "[ui][title_app]") {
  StartupBoxScope scope;
  {
    FailingStartApp app(false, true);
    REQUIRE_FALSE(app.Start({}));
    CHECK(app.Shutdown() == 1);
  }
  CHECK(g_startup_boxes == 0);
}

REX_TEST_CASE("A module that fails to load with --ui.driver=none shows no box", "[ui][title_app]") {
  StartupBoxScope scope;
  {
    FailingStartApp app(true, false);
    REQUIRE_FALSE(app.Start({"--ui.driver=none"}));
    CHECK(app.Shutdown() == 1);
  }
  CHECK(g_startup_boxes == 0);
}

REX_TEST_CASE("An unknown option shows one box", "[ui][title_app]") {
  StartupBoxScope scope;
  {
    FailingStartApp app(false, false, false);
    REQUIRE_FALSE(app.Start({"--nope=1"}));
    CHECK(app.Shutdown() == 1);
  }
  CHECK(g_startup_boxes == 1);
}

REX_TEST_CASE("A module that fails to load shows one box", "[ui][title_app]") {
  StartupBoxScope scope;
  {
    FailingStartApp app(true, false);
    REQUIRE_FALSE(app.Start({}));
    CHECK(app.Shutdown() == 1);
  }
  CHECK(g_startup_boxes == 1);
}
