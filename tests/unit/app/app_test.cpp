/**
 * @file        tests/unit/app/app_test.cpp
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
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rex/app/app.h>
#include <rex/path_settings.h>
#include <rex/cli.h>
#include <rex/logging/logger.h>
#include <rex/platform/file.h>
#include <rex/logging/log_category.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/platform/timer.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_system.h>

#include "base/app_harness.h"
#include "app/lifecycle.h"
#include "base/timer_queue_state.h"
#include "temp_folder.h"
#include <array>
#include <rex/apu/audio_driver.h>
#include <rex/apu/audio_system.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/app_info.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/hid/input_system.h>
#include "../ui/ui_harness.h"

namespace {

std::vector<std::string> g_events;
std::vector<std::string> g_relaunched;

[[nodiscard]] rex::Result<void> FakeRelaunch(std::span<const std::string> args) {
  g_relaunched.assign(args.begin(), args.end());
  g_events.push_back("relaunch");
  return rex::Ok();
}

[[nodiscard]] rex::Result<void> FailRelaunch(std::span<const std::string>) {
  return rex::Err<void>(rex::ErrorCategory::kRuntime, "no relaunch");
}

class TestApp : public rex::test::ObjectApp<rex::app::App> {
 public:
  TestApp() : ObjectApp("app_test") {}

  std::string lock_name;
  bool fail_configure = false;
  bool fail_init = false;
  bool poll = false;
  rex::i32 code = 0;
  rex::test::TempFolder folder{"rex_app_test_"};
  std::vector<rex::IModule*> extra_providers;

 protected:
  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.insert(providers.end(), extra_providers.begin(), extra_providers.end());
  }

  [[nodiscard]] rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    g_events.push_back("app:configure");
    (void)settings;
    REX_TRY_VOID(root.SetText(rex::SettingLayer::kTitle, folder.path.string()));
    REX_TRY_VOID(single_instance.SetText(rex::SettingLayer::kTitle, lock_name));
    if (fail_configure) {
      return rex::Err<void>(rex::ErrorCategory::kConfig, "configure failed");
    }
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> OnInit() override {
    g_events.push_back("app:init");
    if (fail_init) {
      return rex::Err<void>(rex::ErrorCategory::kConfig, "init failed");
    }
    return rex::Ok();
  }

  void OnRun() override {
    g_events.push_back("app:run");
    if (code != 0) {
      rex::app::RequestShutdown(code);
    }
    while (poll && !rex::app::IsShutdownRequested()) {
      std::this_thread::yield();
    }
  }

  void OnShutdown() override { g_events.push_back("app:shutdown"); }
};

class DepthOwner final : public rex::SettingOwner {
 public:
  DepthOwner() : SettingOwner("feedlayers") {}
  REX_SETTING(rex::i32, depth, 0, "Depth");
  REX_SETTING(rex::i32, startup, 0, "Startup", rex::kStartupOnly);
};

class LayerApp : public TestApp {
 public:
  rex::i32 title_value = 0;
  rex::i32 depth_at_init = -1;
  rex::SettingLayer layer_at_init = rex::SettingLayer::kDefault;
  bool startup_refused = false;
  DepthOwner owner;

 protected:
  [[nodiscard]] rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    REX_TRY_VOID(settings.Register(owner));
    if (title_value != 0) {
      REX_TRY_VOID(owner.depth.SetText(rex::SettingLayer::kTitle, std::to_string(title_value)));
    }
    return TestApp::OnConfigure(settings);
  }

  [[nodiscard]] rex::Result<void> OnInit() override {
    depth_at_init = owner.depth.Get();
    layer_at_init = owner.depth.GetLayer();
    startup_refused = !owner.startup.Set(5);
    return TestApp::OnInit();
  }
};

class OrderProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "order"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override {
    g_events.push_back("provider:connect");
    return rex::Ok();
  }
  void Disconnect() override { g_events.push_back("provider:disconnect"); }
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return {}; }
};

class EarlyFailApp : public TestApp {
 public:
  [[nodiscard]] rex::Result<void> FailBeforeInit() {
    return Fail("paths", rex::Error(rex::ErrorCategory::kConfig, "no user folder"));
  }
};

class ActionApp : public TestApp {
 public:
  std::vector<std::string> actions_seen;
  std::string config_value_at_init;
  bool fail_action = false;

 protected:
  std::span<const rex::CLIAction> GetActions() const override { return kActions; }

  [[nodiscard]] rex::Result<void> OnAction(const rex::CLIActionHit& hit) override {
    g_events.push_back("app:action:" + std::string(hit.action.name));
    actions_seen.push_back(std::string(hit.action.name) + "=" + hit.option.value.value_or(""));
    if (fail_action) {
      return rex::Err<void>(rex::ErrorCategory::kConfig, "action failed");
    }
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> OnInit() override {
    config_value_at_init = REX_SETTINGS().Find("clock.no_scaling").value()->GetText();
    return TestApp::OnInit();
  }

 private:
  static constexpr std::array<rex::CLIAction, 1> kActions = {
      REX_CLI_ACTION(repair, "Opens the repair mode.")};
};

class ClashApp : public TestApp {
 protected:
  std::span<const rex::CLIAction> GetActions() const override { return kClash; }

 private:
  static constexpr std::array<rex::CLIAction, 1> kClash = {
      rex::CLIAction{"config_file", "Clashes."}};
};

class InitProvider final : public rex::IModule {
 public:
  std::string_view GetName() const override { return "init_event"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return rex::Ok(); }
  void Disconnect() override {}
  rex::Result<void> Init() override {
    g_events.push_back("provider:init");
    return rex::Ok();
  }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view, rex::u32) override { return nullptr; }
};

enum class RequestIn : rex::u8 { kConfigure, kAction, kInit };

class RestartApp final : public ActionApp {
 public:
  explicit RestartApp(RequestIn where) : where_(where) { extra_providers.push_back(&provider_); }

 protected:
  [[nodiscard]] rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    REX_TRY_VOID(ActionApp::OnConfigure(settings));
    REXLOG_INFO("update installed in OnConfigure");
    Request(RequestIn::kConfigure);
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> OnAction(const rex::CLIActionHit& hit) override {
    REX_TRY_VOID(ActionApp::OnAction(hit));
    Request(RequestIn::kAction);
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> OnInit() override {
    REX_TRY_VOID(ActionApp::OnInit());
    Request(RequestIn::kInit);
    return rex::Ok();
  }

 private:
  void Request(RequestIn at) {
    if (at == where_) {
      rex::app::RequestRestart({"--updated", "--profile", "b"});
    }
  }

  RequestIn where_;
  InitProvider provider_;
};

std::filesystem::path LogFile(const std::filesystem::path& root) {
  for (const auto& entry : std::filesystem::directory_iterator(root / "logs")) {
    return entry.path();
  }
  return {};
}

rex::CLIArgList Args(std::vector<std::string> args = {}) {
  return rex::CLIArgList::Parse(args);
}

struct Fixture {
  Fixture() {
    g_events.clear();
    g_relaunched.clear();
    rex::app::detail::SetRelaunchForTest(&FakeRelaunch, true);
  }
  ~Fixture() { rex::app::detail::SetRelaunchForTest(nullptr, true); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "App runs its steps in lifecycle order", "[app][app]") {
  TestApp app;
  app.code = 3;
  REQUIRE(app.Init(Args()));
  app.Run();
  REQUIRE(app.Shutdown() == 3);
  REQUIRE(g_events ==
          std::vector<std::string>{"app:configure", "app:init", "app:run", "app:shutdown"});
}

REX_TEST_CASE_METHOD(Fixture, "App Shutdown stops the timer queue", "[app][app]") {
  TestApp app;
  REQUIRE(app.Init(Args()));
  rex::platform::QueueTimerOnce(
      [](void*) {}, nullptr,
      rex::platform::TimerQueueWaitItem::clock::now() + std::chrono::hours(1));
  CHECK(rex::platform::detail::IsTimerQueueRunning());
  app.Run();
  app.Shutdown();
  CHECK_FALSE(rex::platform::detail::IsTimerQueueRunning());
}

REX_TEST_CASE_METHOD(Fixture,
                     "A failed Init and an App that never started still stop the timer queue",
                     "[app][app]") {
  const auto queue_timer = [] {
    rex::platform::QueueTimerOnce(
        [](void*) {}, nullptr,
        rex::platform::TimerQueueWaitItem::clock::now() + std::chrono::hours(1));
  };
  {
    TestApp app;
    app.fail_init = true;
    queue_timer();
    REQUIRE_FALSE(app.Init(Args()));
    REQUIRE(app.Shutdown() == 1);
    CHECK_FALSE(rex::platform::detail::IsTimerQueueRunning());
  }
  {
    TestApp app;
    app.fail_configure = true;
    queue_timer();
    REQUIRE_FALSE(app.Init(Args()));
    REQUIRE(app.Shutdown() == 1);
    CHECK_FALSE(rex::platform::detail::IsTimerQueueRunning());
  }
}

REX_TEST_CASE_METHOD(Fixture, "A failing OnConfigure loads nothing", "[app][app]") {
  TestApp app;
  app.fail_configure = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure"});
}

REX_TEST_CASE_METHOD(Fixture, "A failing OnInit skips OnShutdown", "[app][app]") {
  TestApp app;
  app.fail_init = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(std::ranges::find(g_events, "app:shutdown") == g_events.end());
}

REX_TEST_CASE_METHOD(Fixture, "a structural flag overrides OnConfigure", "[app][app]") {
  TestApp app;
  REQUIRE(app.Init(Args({"--user_data_root=from_command_line"})));
  CHECK(REX_PATHS().user_root.Get() == std::filesystem::path("from_command_line"));
  CHECK(REX_PATHS().user_root.GetLayer() == rex::SettingLayer::kCommandLine);
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "An unknown option fails Init and names it", "[app][app]") {
  TestApp app;
  auto result = app.Init(Args({"--nope=1"}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().message.find("--nope=1") != std::string::npos);
  REQUIRE(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown after a failed module start leaves the systems alone",
                     "[app][app]") {
  TestApp app;
  REQUIRE_FALSE(app.Init(Args({"--nope=1"})));
  g_events.clear();
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(std::ranges::find(g_events, "app:init") == g_events.end());
  REQUIRE(std::ranges::find(g_events, "app:shutdown") == g_events.end());
  REQUIRE(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "A declared action reaches OnAction before the providers connect",
                     "[app][app][action]") {
  ActionApp app;
  REQUIRE(app.Init(Args({"--repair=now"})));
  REQUIRE(app.actions_seen == std::vector<std::string>{"repair=now"});
  REQUIRE(g_events.size() >= 2);
  REQUIRE(g_events[0] == "app:configure");
  REQUIRE(g_events[1] == "app:action:repair");
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "An OnAction error fails Init before the providers connect",
                     "[app][app][action]") {
  ActionApp app;
  app.fail_action = true;
  REQUIRE_FALSE(app.Init(Args({"--repair"})));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure", "app:action:repair"});
}

REX_TEST_CASE_METHOD(Fixture, "An action named like a structural flag fails Init",
                     "[app][app][action]") {
  ClashApp app;
  auto result = app.Init(Args());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().message.find("config_file") != std::string::npos);
  REQUIRE(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "An app can defer its action past the config file load",
                     "[app][app][action]") {
  rex::test::TempFolder folder("rex_app_action_");
  const auto config_path = folder.path / "app.toml";
  {
    std::ofstream out(config_path, std::ios::binary);
    out << "[clock]\nno_scaling = true\n";
  }
  ActionApp app;
  const std::string config_arg = "--config_file=" + config_path.string();
  REQUIRE(app.Init(Args({"--repair", config_arg})));
  REQUIRE(app.actions_seen == std::vector<std::string>{"repair="});
  REQUIRE(app.config_value_at_init == "true");
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "args returns the positionals", "[app][app]") {
  TestApp app;
  REQUIRE(app.Init(Args({"pos", "--ui.fullscreen=false"})));
  REQUIRE(app.GetArgs().size() == 1);
  REQUIRE(app.GetArgs()[0] == "pos");
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "RequestShutdown from another thread sets the exit code",
                     "[app][app]") {
  TestApp app;
  app.poll = true;
  REQUIRE(app.Init(Args()));
  std::jthread requester([] { rex::app::RequestShutdown(7); });
  app.Run();
  REQUIRE(app.Shutdown() == 7);
}

REX_TEST_CASE_METHOD(Fixture,
                     "single_instance held elsewhere fails Init before the providers connect",
                     "[app][app]") {
  auto held = rex::platform::InstanceLock::Acquire("rex-app-test");
  REQUIRE(held);
  TestApp app;
  app.lock_name = "rex-app-test";
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure"});
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown is idempotent after any failure", "[app][app]") {
  TestApp app;
  app.fail_configure = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  const auto events = g_events;
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == events);
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown is idempotent after a run and after a failed OnInit",
                     "[app][app]") {
  TestApp ran;
  ran.code = 5;
  REQUIRE(ran.Init(Args()));
  ran.Run();
  REQUIRE(ran.Shutdown() == 5);
  auto events = g_events;
  REQUIRE(ran.Shutdown() == 5);
  REQUIRE(g_events == events);

  TestApp failed;
  failed.fail_init = true;
  REQUIRE_FALSE(failed.Init(Args()));
  REQUIRE(failed.Shutdown() == 1);
  events = g_events;
  REQUIRE(failed.Shutdown() == 1);
  REQUIRE(g_events == events);
}

REX_TEST_CASE_METHOD(Fixture, "Init names the log after the app", "[app][app]") {
  TestApp app;
  REQUIRE(app.Init(Args()));
  const std::filesystem::path file = LogFile(app.folder.path);
  REQUIRE(file.parent_path() == app.folder.path / "logs");
  REQUIRE(file.filename().string().starts_with("app_test_"));
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "A failed relaunch reaches the log before logging shuts down",
                     "[app][app]") {
  rex::app::detail::SetRelaunchForTest(&FailRelaunch, true);
  TestApp app;
  REQUIRE(app.Init(Args()));
  rex::Logger* logger = &REX_LOGGER();
  rex::log::LogCaptureSink sink;
  auto sink_guard = logger->AddSink(sink);
  rex::app::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  sink_guard.Reset();
  REQUIRE(std::ranges::any_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.text.find("relaunch failed") != std::string::npos;
  }));
}

REX_TEST_CASE_METHOD(Fixture,
                     "A restart requested in OnConfigure ends startup before the modules init",
                     "[app][app][restart]") {
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  RestartApp app(RequestIn::kConfigure);
  REQUIRE(app.Init(Args()));
  CHECK(rex::app::IsShutdownRequested());
  CHECK(g_events == std::vector<std::string>{"app:configure"});
  REQUIRE(app.Shutdown() == 0);
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  CHECK(std::ranges::none_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.level >= rex::log::Level::kWarn;
  }));
  std::ifstream in(LogFile(app.folder.path));
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  CHECK(text.find("update installed in OnConfigure") != std::string::npos);
  CHECK(g_events == std::vector<std::string>{"app:configure", "relaunch"});
  CHECK(g_relaunched == std::vector<std::string>{"--updated", "--profile", "b"});
}

REX_TEST_CASE_METHOD(Fixture,
                     "A restart requested in OnAction ends startup before the modules init",
                     "[app][app][restart]") {
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  RestartApp app(RequestIn::kAction);
  REQUIRE(app.Init(Args({"--repair"})));
  CHECK(rex::app::IsShutdownRequested());
  CHECK(g_events == std::vector<std::string>{"app:configure", "app:action:repair"});
  REQUIRE(app.Shutdown() == 0);
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  CHECK(std::ranges::none_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.level >= rex::log::Level::kWarn;
  }));
  std::ifstream in(LogFile(app.folder.path));
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  CHECK(text.find("update installed in OnConfigure") != std::string::npos);
  CHECK(g_relaunched == std::vector<std::string>{"--updated", "--profile", "b"});
}

REX_TEST_CASE_METHOD(Fixture, "A restart requested in OnInit does not end startup",
                     "[app][app][restart]") {
  RestartApp app(RequestIn::kInit);
  REQUIRE(app.Init(Args({"--repair"})));
  CHECK(rex::app::IsShutdownRequested());
  CHECK(g_events == std::vector<std::string>{"app:configure", "app:action:repair", "provider:init",
                                             "app:init"});
  REQUIRE(app.Shutdown() == 0);
  CHECK(g_relaunched == std::vector<std::string>{"--updated", "--profile", "b"});
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown without Init returns 0", "[app][app]") {
  TestApp app;
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_events.empty());
}

REX_TEST_CASE_METHOD(Fixture, "RequestRestart relaunches with the given arguments", "[app][app]") {
  TestApp app;
  REQUIRE(app.Init(Args({"--ui.fullscreen=false"})));
  rex::app::RequestRestart({"--profile", "b"});
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_relaunched == std::vector<std::string>{"--profile", "b"});
}

REX_TEST_CASE_METHOD(Fixture, "RequestRestart without relaunch support starts nothing",
                     "[app][app]") {
  rex::app::detail::SetRelaunchForTest(&FakeRelaunch, false);
  TestApp app;
  REQUIRE(app.Init(Args()));
  rex::app::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(std::ranges::find(g_events, "relaunch") == g_events.end());
}

REX_TEST_CASE("A category keeps writing after the logger stops and starts", "[app][app]") {
  rex::Logger* logger = &REX_LOGGER();
  {
    rex::test::RunInInit([] {});
  }
  std::vector<rex::log::CapturedMessage> entries;
  rex::test::RunInInit([&] {
    rex::log::LogCaptureSink sink;
    auto sink_guard = logger->AddSink(sink);
    REXLOG_WARN("after logging shut down");
    sink.CopySince(0, entries);
    sink_guard.Reset();
  });
  REQUIRE(std::ranges::any_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.text.find("after logging shut down") != std::string::npos;
  }));
}

REX_TEST_CASE_METHOD(Fixture, "The logger outlives the timer queue and the relaunch log",
                     "[app][app]") {
  rex::app::detail::SetRelaunchForTest(&FailRelaunch, true);
  TestApp app;
  REQUIRE(app.Init(Args()));
  const std::filesystem::path file = LogFile(app.folder.path);
  rex::app::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  std::ifstream in(file);
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  REQUIRE(text.find("relaunch failed") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "An app left alive does not end the session of the next app",
                     "[app][app]") {
  auto first = std::make_unique<TestApp>();
  REQUIRE(first->Init(Args()));
  REQUIRE(first->Shutdown() == 0);
  TestApp second;
  REQUIRE(second.Init(Args()));
  first.reset();
  CHECK_FALSE(second.GetUI().HasDialogs());
  REXLOG_INFO("second still logs");
  REQUIRE(second.Shutdown() == 0);
  const std::filesystem::path file = LogFile(second.folder.path);
  std::ifstream in(file);
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  CHECK(text.find("second still logs") != std::string::npos);
}

REX_TEST_CASE("RunGuarded returns the exit code of the entry body", "[app][app][entry]") {
  CHECK(rex::app::detail::RunGuarded([] { return 7; }) == 7);
}

REX_TEST_CASE("RunGuarded turns an escaping exception into exit code 1", "[app][app][entry]") {
  CHECK(rex::app::detail::RunGuarded([]() -> int { throw std::runtime_error("entry failed"); }) ==
        1);
  CHECK(rex::app::detail::RunGuarded([]() -> int { throw 5; }) == 1);
}

REX_TEST_CASE_METHOD(Fixture, "The first shutdown request keeps its exit code", "[app][app]") {
  rex::app::detail::ResetAppState();
  REQUIRE_FALSE(rex::app::detail::GetRequestedExitCode().has_value());
  rex::app::RequestShutdown(7);
  rex::app::RequestShutdown(0);
  REQUIRE(rex::app::IsShutdownRequested());
  REQUIRE(rex::app::detail::GetRequestedExitCode() == 7);
  rex::app::RequestShutdown(-3);
  REQUIRE(rex::app::detail::GetRequestedExitCode() == 7);
  rex::app::detail::ResetAppState();
  rex::app::RequestShutdown(-3);
  REQUIRE(rex::app::detail::GetRequestedExitCode() == -3);
  rex::app::detail::ResetAppState();
}

REX_TEST_CASE_METHOD(Fixture, "A failure before Init makes Shutdown return 1", "[app][app]") {
  EarlyFailApp app;
  CHECK_FALSE(app.FailBeforeInit());
  CHECK(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "A second Init fails the app and Shutdown returns 1", "[app][app]") {
  TestApp app;
  REQUIRE(app.Init(Args()));
  CHECK_FALSE(app.Init(Args()));
  CHECK(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "init applies layers in order", "[app][app]") {
  const auto run = [](rex::i32 title, const std::vector<std::string>& args) {
    LayerApp app;
    app.title_value = title;
    REQUIRE(app.Init(Args(args)));
    const auto result = std::pair{app.depth_at_init, app.layer_at_init};
    app.Shutdown();
    return result;
  };
  CHECK(run(1, {}) == std::pair{1, rex::SettingLayer::kTitle});

  rex::test::TempFolder folder("rex_app_layers_");
  const auto file = folder.path / "layers.toml";
  std::ofstream(file, std::ios::binary) << "[feedlayers]\ndepth = 2\n";
  const std::string file_arg = "--config_file=" + file.string();
  CHECK(run(1, {file_arg}) == std::pair{2, rex::SettingLayer::kFile});

  REQUIRE(rex::platform::SetEnvironmentValue("REX_FEEDLAYERS_DEPTH", "3"));
  CHECK(run(1, {file_arg}) == std::pair{3, rex::SettingLayer::kEnvironment});
  CHECK(run(1, {file_arg, "--feedlayers.depth=4"}) ==
        std::pair{4, rex::SettingLayer::kCommandLine});
  REQUIRE(rex::platform::UnsetEnvironmentValue("REX_FEEDLAYERS_DEPTH"));
}

REX_TEST_CASE_METHOD(Fixture, "startup only freezes after providers", "[app][app]") {
  LayerApp app;
  REQUIRE(app.Init(Args()));
  CHECK(app.startup_refused);
  CHECK(app.owner.startup.Get() == 0);
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "failed init still shuts down", "[app][app]") {
  OrderProvider provider;
  TestApp app;
  app.extra_providers = {&provider};
  app.fail_init = true;
  REQUIRE_FALSE(app.Init(Args()));
  CHECK(app.Shutdown() == 1);
  CHECK(std::ranges::count(g_events, "provider:connect") == 1);
  CHECK(std::ranges::count(g_events, "provider:disconnect") == 1);
}

REX_TEST_CASE_METHOD(Fixture, "API reaches the modules only while the app runs", "[app][app]") {
  OrderProvider module;
  TestApp app;
  app.extra_providers = {&module};
  CHECK(REX_INTERFACE(rex::IDriverModule) == nullptr);
  REQUIRE(app.Init(Args()));
  CHECK(REX_INTERFACE(rex::IDriverModule) == static_cast<rex::IDriverModule*>(&module));
  CHECK(app.Shutdown() == 0);
  CHECK(REX_INTERFACE(rex::IDriverModule) == nullptr);
}

REX_TEST_CASE_METHOD(Fixture, "two apps in one process", "[app][app]") {
  std::vector<std::string> events;
  {
    TestApp first;
    REQUIRE(first.Init(Args({"--log.level=error"})));
    CHECK(REX_LOGGER().level.Get() == "error");
    first.Run();
    CHECK(first.Shutdown() == 0);
    events = g_events;
    g_events.clear();
  }
  TestApp second;
  REQUIRE(second.Init(Args()));
  CHECK(REX_LOGGER().level.Get() == "info");
  CHECK(second.GetPhase() == rex::app::AppPhase::kRunning);
  second.Run();
  CHECK(second.Shutdown() == 0);
  CHECK(g_events == events);
  CHECK(second.GetPhase() == rex::app::AppPhase::kShutdown);
}

REX_TEST_CASE_METHOD(Fixture, "an unknown setting on the command line fails Init and names it",
                     "[app][app]") {
  TestApp app;
  auto result = app.Init(Args({"--nope.thing=1"}));
  REQUIRE_FALSE(result);
  CHECK(result.error().message == "unknown setting 'nope.thing' from CommandLine");
  CHECK(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "an unknown setting from the config file is only a warning",
                     "[app][app]") {
  rex::test::TempFolder folder("rex_app_unknown_");
  const auto file = folder.path / "unknown.toml";
  std::ofstream(file, std::ios::binary) << "[nope]\nthing = 1\n";
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  TestApp app;
  REQUIRE(app.Init(Args({"--config_file=" + file.string()})));
  app.Shutdown();
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  CHECK(std::ranges::any_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.level == rex::log::Level::kWarn && entry.text.contains("nope.thing");
  }));
}

REX_TEST_CASE_METHOD(Fixture, "a setting an owner registers during OnInit is not unknown",
                     "[app][app]") {
  class LateApp final : public TestApp {
   public:
    DepthOwner owner;

   protected:
    [[nodiscard]] rex::Result<void> OnInit() override {
      REX_TRY_VOID(REX_SETTINGS().Register(owner));
      return TestApp::OnInit();
    }
  };
  LateApp app;
  REQUIRE(app.Init(Args({"--feedlayers.depth=4"})));
  CHECK(app.owner.depth.Get() == 4);
  app.Shutdown();
}

namespace system_modules_tests {

namespace {

std::vector<std::string> g_events;

class OrderInput final : public rex::hid::IInputDriver {
 public:
  std::string_view GetName() const override { return "order_input"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_events.emplace_back("init input");
    return {};
  }
  void Shutdown() override { g_events.emplace_back("shutdown input"); }
  rex::u64 GetDeviceRevision() const override { return 0; }
  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override { out.clear(); }
  rex::Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID) override {
    return rex::Err<rex::hid::PadState>(rex::ErrorCategory::kNotFound, "none");
  }
  rex::Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID) override {
    return rex::hid::PadCapabilities{};
  }
  rex::Result<void> SetDeviceVibration(rex::hid::PadDeviceID,
                                       const rex::hid::PadVibration&) override {
    return {};
  }
};

class OrderAudio final : public rex::apu::IAudioDriver {
 public:
  std::string_view GetName() const override { return "order_audio"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_events.emplace_back("init audio");
    return {};
  }
  void Shutdown() override { g_events.emplace_back("shutdown audio"); }
  rex::Result<rex::u32> OpenDevice(rex::u32 wanted, rex::apu::AudioFill) override { return wanted; }
  void CloseDevice() override {}
  bool IsDeviceLost() const override { return false; }
};

class OrderUI final : public rex::test_ui::FakeUIDriver {
 public:
  std::string_view GetName() const override { return "order_ui"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_events.emplace_back("init ui");
    return {};
  }
  void Shutdown() override { g_events.emplace_back("shutdown ui"); }
};

std::unique_ptr<rex::IDriver> CreateInput() {
  return std::make_unique<OrderInput>();
}

std::unique_ptr<rex::IDriver> CreateAudio() {
  return std::make_unique<OrderAudio>();
}

std::unique_ptr<rex::IDriver> CreateUI() {
  return std::make_unique<OrderUI>();
}

class OrderProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "order"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 3> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kInput, "order_input", {}, &CreateInput},
      rex::DriverRegistration{rex::DriverType::kAudio, "order_audio", {}, &CreateAudio},
      rex::DriverRegistration{rex::DriverType::kUI, "order_ui", {}, &CreateUI}};
};

class OrderApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  OrderApp() : ObjectApp("order_test") {
    static_cast<rex::test_ui::FakeUIData&>(rex::test_ui::g_ui) = rex::test_ui::FakeUIData{};
  }

  bool needs_ui = false;
  bool dirty = false;

  rex::Result<void> Start() {
    auto started = Init(rex::CLIArgList::Parse(std::vector<std::string>{}));
    if (started && dirty) {
      auto stream = GetAudio().OpenStream(rex::MoveOnlyFunction<void()>([] {}));
      if (stream) {
        stream_ = *stream;
      }
    }
    return started;
  }

  int Finish() { return Shutdown(); }

  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs;
    needs.ui = needs_ui;
    return needs;
  }

 protected:
  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
  }

  rex::Result<void> OnConfigure(rex::SettingsRegistry&) override {
    REX_TRY_VOID(root.SetText(rex::SettingLayer::kTitle, folder_.path.string()));
    REX_TRY_VOID(
        GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "order_input"));
    REX_TRY_VOID(GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "order_audio"));
    if (dirty) {
      REX_TRY_VOID(GetAudio().GetSettings().mute.SetText(rex::SettingLayer::kTitle, "true"));
    }
    REX_TRY_VOID(GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "order_ui"));
    return rex::Ok();
  }

 private:
  rex::test::TempFolder folder_{"rex_order_"};
  rex::apu::AudioStreamID stream_{};
  OrderProvider provider_;
};

}  // namespace

REX_TEST_CASE("systems stop in reverse start order", "[app][app][modules]") {
  g_events.clear();
  OrderApp app;
  REQUIRE(app.Start());
  app.Finish();
  const std::vector<std::string> expected = {"init input",  "init audio",     "init ui",
                                             "shutdown ui", "shutdown audio", "shutdown input"};
  CHECK(g_events == expected);
}

REX_TEST_CASE("a system that fails to init is stopped once and the earlier ones are stopped",
              "[app][app][modules]") {
  g_events.clear();
  OrderApp app;
  app.needs_ui = true;
  rex::test_ui::g_ui.open_fails = true;
  CHECK_FALSE(app.Start());
  app.Finish();
  const std::vector<std::string> expected = {"init input",  "init audio",     "init ui",
                                             "shutdown ui", "shutdown audio", "shutdown input"};
  CHECK(g_events == expected);
}

REX_TEST_CASE("a second app in one process starts from default systems", "[app][app][modules]") {
  {
    OrderApp first;
    first.dirty = true;
    REQUIRE(first.Start());
    CHECK(first.GetAudio().GetSettings().mute.Get());
    CHECK(first.GetAudio().GetStats().open_streams == 1);
    first.Finish();
  }
  OrderApp second;
  REQUIRE(second.Start());
  CHECK_FALSE(second.GetAudio().GetSettings().mute.Get());
  CHECK(second.GetAudio().GetStats().open_streams == 0);
  second.Finish();
}

}  // namespace system_modules_tests
