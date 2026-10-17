/**
 * @file        tests/unit/core/app_test.cpp
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
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/app.h>
#include <rex/cli_action.h>
#include <rex/cvar/registry.h>
#include <rex/logging/logger.h>
#include <rex/platform/file.h>
#include <rex/logging/log.h>
#include <rex/logging/sink.h>
#include <rex/platform/dynlib.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/platform/timer_queue.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "core/app_state.h"
#include "core/module_loader.h"
#include "modules/echo.h"
#include "modules/test_module_sink.h"
#include "temp_folder.h"

namespace {

std::vector<std::string> g_events;
std::vector<std::string> g_relaunched;

void Sink(const char* module, const char* event) {
  g_events.push_back(std::string(module) + ":" + event);
}

class Pinned {
 public:
  explicit Pinned(const char* name) {
    auto path = rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName(name);
    REQUIRE(library_.Load(path, rex::platform::SymbolResolution::kImmediate));
    auto set_sink = library_.GetSymbol<void (*)(RexModTestSink)>("rexmod_test_set_sink");
    REQUIRE(set_sink != nullptr);
    set_sink(&Sink);
  }

 private:
  rex::platform::DynamicLibrary library_;
};

[[nodiscard]] rex::Result<void> FakeRelaunch(std::span<const std::string> args) {
  g_relaunched.assign(args.begin(), args.end());
  g_events.push_back("relaunch");
  return rex::Ok();
}

[[nodiscard]] rex::Result<void> FailRelaunch(std::span<const std::string>) {
  return rex::Err<void>(rex::ErrorCategory::kRuntime, "no relaunch");
}

bool AddressMapped(const void* address) {
#if REX_PLATFORM_WIN32
  HMODULE module = nullptr;
  return GetModuleHandleExA(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             static_cast<LPCSTR>(address), &module) != 0;
#else
  Dl_info info{};
  return dladdr(address, &info) != 0;
#endif
}

class TestApp : public rex::App {
 public:
  std::vector<std::string> modules{"rexmod-test-good"};
  std::string game_root;
  std::string lock_name;
  bool fail_configure = false;
  bool fail_init = false;
  bool poll = false;
  bool saw_echo = false;
  rex::i32 code = 0;

 protected:
  [[nodiscard]] rex::Result<void> OnConfigure(rex::Config& config) override {
    g_events.push_back("app:configure");
    config.modules = modules;
    config.paths.game_data_root = game_root;
    config.single_instance = lock_name;
    if (fail_configure) {
      return rex::Err<void>(rex::ErrorCategory::kConfig, "configure failed");
    }
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> OnInit(rex::Config&, rex::API& api) override {
    g_events.push_back("app:init");
    saw_echo = api.Get<rex::test::IEcho>() != nullptr;
    if (fail_init) {
      return rex::Err<void>(rex::ErrorCategory::kConfig, "init failed");
    }
    return rex::Ok();
  }

  rex::i32 OnRun(rex::API&) override {
    g_events.push_back("app:run");
    while (poll && !rex::ShutdownRequested()) {
      std::this_thread::yield();
    }
    return code;
  }

  void OnShutdown() override { g_events.push_back("app:shutdown"); }
};

class ActionApp : public TestApp {
 public:
  std::vector<std::string> actions_seen;
  std::string flush_interval_at_init;
  bool fail_action = false;

 protected:
  std::span<const rex::CLIAction> GetActions() const override { return kActions; }

  [[nodiscard]] rex::Result<void> OnAction(const rex::CLIAction& action,
                                           const rex::CLIOption& option) override {
    g_events.push_back("app:action:" + std::string(action.name));
    actions_seen.push_back(std::string(action.name) + "=" + option.value.value_or(""));
    if (fail_action) {
      return rex::Err<void>(rex::ErrorCategory::kConfig, "action failed");
    }
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> OnInit(rex::Config& config, rex::API& api) override {
    flush_interval_at_init =
        api.Get<rex::cvar::ICvarRegistry>()->Get("log.flush_interval").value_or("");
    return TestApp::OnInit(config, api);
  }

 private:
  static constexpr std::array<rex::CLIAction, 1> kActions = {
      REX_CLI_ACTION(repair, "Opens the repair mode.")};
};

class ClashApp : public TestApp {
 protected:
  std::span<const rex::CLIAction> GetActions() const override { return kClash; }

 private:
  static constexpr std::array<rex::CLIAction, 1> kClash = {rex::CLIAction{"backend", "Clashes."}};
};

rex::CLIArgList Args(std::vector<std::string> args = {}) {
  return rex::CLIArgList::Parse(args);
}

struct Fixture {
  Fixture() {
    g_events.clear();
    g_relaunched.clear();
    rex::detail::SetRelaunchForTest(&FakeRelaunch, true);
  }
  ~Fixture() { rex::detail::SetRelaunchForTest(nullptr, true); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "App runs its steps in lifecycle order", "[core][app]") {
  Pinned pin("rexmod-test-good");
  TestApp app;
  app.code = 3;
  REQUIRE(app.Init(Args()));
  app.Run();
  REQUIRE(app.Shutdown() == 3);
  REQUIRE(app.saw_echo);
  REQUIRE(g_events == std::vector<std::string>{"app:configure", "rexmod-test-good:constructor",
                                               "rexmod-test-good:connect", "app:init",
                                               "rexmod-test-good:init", "app:run", "app:shutdown",
                                               "rexmod-test-good:shutdown",
                                               "rexmod-test-good:disconnect"});
}

REX_TEST_CASE_METHOD(Fixture, "A failing OnConfigure loads nothing", "[core][app]") {
  TestApp app;
  app.fail_configure = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure"});
}

REX_TEST_CASE_METHOD(Fixture, "A failing OnInit disconnects every module", "[core][app]") {
  Pinned pin("rexmod-test-good");
  TestApp app;
  app.fail_init = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events.back() == "rexmod-test-good:disconnect");
  REQUIRE(std::ranges::find(g_events, "rexmod-test-good:init") == g_events.end());
}

REX_TEST_CASE_METHOD(Fixture, "command line overrides OnConfigure", "[core][app]") {
  Pinned pin("rexmod-test-good");
  TestApp app;
  app.game_root = "C:/from_configure";
  REQUIRE(app.Init(Args({"--game_data_root=C:/from_command_line"})));
  REQUIRE(app.GetAPI().GetConfig().paths.game_data_root == "C:/from_command_line");
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "An unknown option fails Init and names it", "[core][app]") {
  TestApp app;
  app.modules = {};
  auto result = app.Init(Args({"--nope=1"}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().message.find("--nope=1") != std::string::npos);
  REQUIRE(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "A declared action reaches OnAction before the modules load",
                     "[core][app][action]") {
  ActionApp app;
  app.modules = {};
  REQUIRE(app.Init(Args({"--repair=now"})));
  REQUIRE(app.actions_seen == std::vector<std::string>{"repair=now"});
  REQUIRE(g_events.size() >= 2);
  REQUIRE(g_events[0] == "app:configure");
  REQUIRE(g_events[1] == "app:action:repair");
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "An OnAction error fails Init before any module",
                     "[core][app][action]") {
  Pinned pin("rexmod-test-good");
  ActionApp app;
  app.fail_action = true;
  REQUIRE_FALSE(app.Init(Args({"--repair"})));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure", "app:action:repair"});
}

REX_TEST_CASE_METHOD(Fixture, "An action named like a structural flag fails Init",
                     "[core][app][action]") {
  ClashApp app;
  app.modules = {};
  auto result = app.Init(Args());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().message.find("backend") != std::string::npos);
  REQUIRE(app.Shutdown() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "An app can defer its action past the config file load",
                     "[core][app][action]") {
  rex::test::TempFolder folder("rex_app_action_");
  const auto config_path = folder.path / "app.toml";
  {
    std::ofstream out(config_path, std::ios::binary);
    out << "[log]\nflush_interval = 45\n";
  }
  Pinned pin("rexmod-test-good");
  ActionApp app;
  const std::string config_arg = "--config_file=" + config_path.string();
  REQUIRE(app.Init(Args({"--repair", config_arg})));
  REQUIRE(app.actions_seen == std::vector<std::string>{"repair="});
  REQUIRE(app.flush_interval_at_init == "45");
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "args returns the command line", "[core][app]") {
  TestApp app;
  app.modules = {"rexfilesystem", "rexruntime", "rexui-sdl3"};
  REQUIRE(app.Init(Args({"pos", "--ui.fullscreen=false"})));
  REQUIRE(app.GetArgs().GetPositionals()[0] == "pos");
  REQUIRE(app.GetArgs().Get("ui.fullscreen") == "false");
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "RequestShutdown from another thread sets the exit code",
                     "[core][app]") {
  TestApp app;
  app.modules = {};
  app.poll = true;
  REQUIRE(app.Init(Args()));
  std::jthread requester([] { rex::RequestShutdown(7); });
  app.Run();
  REQUIRE(app.Shutdown() == 7);
}

REX_TEST_CASE_METHOD(Fixture, "single_instance held elsewhere fails Init before any module",
                     "[core][app]") {
  auto held = rex::platform::InstanceLock::Acquire("rex-app-test");
  REQUIRE(held);
  TestApp app;
  app.lock_name = "rex-app-test";
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure"});
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown is idempotent after any failure", "[core][app]") {
  TestApp app;
  app.fail_configure = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  const auto events = g_events;
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == events);
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown is idempotent after a run and after a failed OnInit",
                     "[core][app]") {
  Pinned pin("rexmod-test-good");
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

REX_TEST_CASE_METHOD(Fixture, "Init names the log after the executable", "[core][app]") {
  TestApp app;
  app.modules = {};
  REQUIRE(app.Init(Args()));
  const std::filesystem::path file = app.GetAPI().Get<rex::ILogger>()->GetFilePath();
  REQUIRE(file.parent_path() == rex::platform::GetExecutableFolder() / "logs");
  REQUIRE(file.filename().string().starts_with(rex::platform::GetExecutablePath().stem().string() +
                                               "_"));
  app.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "A failed relaunch reaches the log before logging shuts down",
                     "[core][app]") {
  rex::detail::SetRelaunchForTest(&FailRelaunch, true);
  TestApp app;
  app.modules = {};
  REQUIRE(app.Init(Args()));
  auto* logger = app.GetAPI().Get<rex::ILogger>();
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  rex::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  logger->RemoveSink(sink);
  REQUIRE(std::ranges::any_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.text.find("relaunch failed") != std::string::npos;
  }));
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown without Init returns 0", "[core][app]") {
  TestApp app;
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_events.empty());
}

REX_TEST_CASE_METHOD(Fixture, "RequestRestart relaunches with the given arguments", "[core][app]") {
  TestApp app;
  app.modules = {"rexfilesystem", "rexruntime", "rexui-sdl3"};
  REQUIRE(app.Init(Args({"--ui.fullscreen=false"})));
  rex::RequestRestart({"--profile", "b"});
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_relaunched == std::vector<std::string>{"--profile", "b"});
}

REX_TEST_CASE_METHOD(Fixture, "RequestRestart without relaunch support starts nothing",
                     "[core][app]") {
  rex::detail::SetRelaunchForTest(&FakeRelaunch, false);
  TestApp app;
  app.modules = {};
  REQUIRE(app.Init(Args()));
  rex::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(std::ranges::find(g_events, "relaunch") == g_events.end());
}

REX_TEST_CASE("A category keeps writing after the logger stops and starts", "[core][app]") {
  auto first = rex::Init(rex::Config{});
  REQUIRE(first);
  auto* logger = first.value()->Get<rex::ILogger>();
  rex::Shutdown();
  REQUIRE(rex::Init(rex::Config{}));
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  REXLOG_WARN(core, "after logging shut down");
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  logger->RemoveSink(sink);
  rex::Shutdown();
  REQUIRE(std::ranges::any_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.text.find("after logging shut down") != std::string::npos;
  }));
}

REX_TEST_CASE_METHOD(Fixture, "App teardown runs twice in 1 process", "[core][app]") {
  Pinned pin("rexmod-test-good");
  TestApp first;
  REQUIRE(first.Init(Args()));
  first.Run();
  REQUIRE(first.Shutdown() == 0);
  auto events = g_events;
  std::erase(events, std::string("rexmod-test-good:constructor"));
  g_events.clear();
  TestApp second;
  REQUIRE(second.Init(Args()));
  REQUIRE(second.saw_echo);
  second.Run();
  REQUIRE(second.Shutdown() == 0);
  REQUIRE(g_events == events);
}

REX_TEST_CASE_METHOD(Fixture, "The logger outlives the timer queue and the relaunch log",
                     "[core][app]") {
  rex::detail::SetRelaunchForTest(&FailRelaunch, true);
  TestApp app;
  app.modules = {};
  REQUIRE(app.Init(Args()));
  const std::filesystem::path file = app.GetAPI().Get<rex::ILogger>()->GetFilePath();
  rex::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  std::ifstream in(file);
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  REQUIRE(text.find("relaunch failed") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "Module libraries close after the timer queue stops", "[core][app]") {
  TestApp app;
  REQUIRE(app.Init(Args()));
  app.Run();
  const void* code = nullptr;
  {
    rex::platform::DynamicLibrary library;
    REQUIRE(library.Load(rex::platform::GetExecutableFolder() /
                         rex::detail::ModuleFileName("rexmod-test-good")));
    code = reinterpret_cast<const void*>(
        library.GetSymbol<void (*)(RexModTestSink)>("rexmod_test_set_sink"));
    REQUIRE(code != nullptr);
  }
  REQUIRE(AddressMapped(code));
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto mapped = std::make_shared<std::atomic<bool>>(false);
  (void)rex::platform::QueueTimerOnce(
      [started, mapped, code](void*) {
        started->store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        mapped->store(AddressMapped(code));
      },
      nullptr, rex::platform::TimerQueueWaitItem::clock::now());
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!started->load() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(started->load());
  REQUIRE(app.Shutdown() == 0);
  CHECK(mapped->load());
  CHECK_FALSE(AddressMapped(code));
}

REX_TEST_CASE("RunGuarded returns the exit code of the entry body", "[core][app][entry]") {
  CHECK(rex::detail::RunGuarded([] { return 7; }) == 7);
}

REX_TEST_CASE("RunGuarded turns an escaping exception into exit code 1", "[core][app][entry]") {
  CHECK(rex::detail::RunGuarded([]() -> int { throw std::runtime_error("entry failed"); }) == 1);
  CHECK(rex::detail::RunGuarded([]() -> int { throw 5; }) == 1);
}

REX_TEST_CASE_METHOD(Fixture, "The first shutdown request keeps its exit code", "[core][app]") {
  rex::detail::ResetAppState();
  REQUIRE_FALSE(rex::detail::GetRequestedExitCode().has_value());
  rex::RequestShutdown(7);
  rex::RequestShutdown(0);
  REQUIRE(rex::ShutdownRequested());
  REQUIRE(rex::detail::GetRequestedExitCode() == 7);
  rex::RequestShutdown(-3);
  REQUIRE(rex::detail::GetRequestedExitCode() == 7);
  rex::detail::ResetAppState();
  rex::RequestShutdown(-3);
  REQUIRE(rex::detail::GetRequestedExitCode() == -3);
  rex::detail::ResetAppState();
}
