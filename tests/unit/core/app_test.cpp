/**
 * @file        tests/unit/core/app_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/app.h>
#include <rex/platform/file.h>
#include <rex/logging.h>
#include <rex/logging/sink.h>
#include <rex/platform/dynlib.h>
#include <rex/platform/process.h>

#include "core/app_state.h"
#include "core/module_loader.h"
#include "modules/echo.h"
#include "modules/test_module_common.h"

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
  return rex::Err<void>(rex::ErrorCategory::Runtime, "no relaunch");
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
      return rex::Err<void>(rex::ErrorCategory::Config, "configure failed");
    }
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> OnInit(rex::Config&, rex::API& api) override {
    g_events.push_back("app:init");
    saw_echo = api.Get<rex::test::IEcho>() != nullptr;
    if (fail_init) {
      return rex::Err<void>(rex::ErrorCategory::Config, "init failed");
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

TEST_CASE_METHOD(Fixture, "App runs its steps in lifecycle order", "[app]") {
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

TEST_CASE_METHOD(Fixture, "A failing OnConfigure loads nothing", "[app]") {
  TestApp app;
  app.fail_configure = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure"});
}

TEST_CASE_METHOD(Fixture, "A failing OnInit disconnects every module", "[app]") {
  Pinned pin("rexmod-test-good");
  TestApp app;
  app.fail_init = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events.back() == "rexmod-test-good:disconnect");
  REQUIRE(std::ranges::find(g_events, "rexmod-test-good:init") == g_events.end());
}

TEST_CASE_METHOD(Fixture, "command line overrides OnConfigure", "[app]") {
  Pinned pin("rexmod-test-good");
  TestApp app;
  app.game_root = "C:/from_configure";
  REQUIRE(app.Init(Args({"--game_data_root=C:/from_command_line"})));
  REQUIRE(app.api().config().paths.game_data_root == "C:/from_command_line");
  app.Shutdown();
}

TEST_CASE_METHOD(Fixture, "An unknown option fails Init and names it", "[app]") {
  TestApp app;
  app.modules = {};
  auto result = app.Init(Args({"--nope=1"}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().message.find("--nope=1") != std::string::npos);
  REQUIRE(app.Shutdown() == 1);
}

TEST_CASE_METHOD(Fixture, "args returns the command line", "[app]") {
  TestApp app;
  app.modules = {"rex-filesystem", "rex-runtime", "rex-ui-sdl3"};
  REQUIRE(app.Init(Args({"pos", "--ui.fullscreen=false"})));
  REQUIRE(app.args().Positionals()[0] == "pos");
  REQUIRE(app.args().Get("ui.fullscreen") == "false");
  app.Shutdown();
}

TEST_CASE_METHOD(Fixture, "RequestShutdown from another thread sets the exit code", "[app]") {
  TestApp app;
  app.modules = {};
  app.poll = true;
  REQUIRE(app.Init(Args()));
  std::jthread requester([] { rex::RequestShutdown(7); });
  app.Run();
  REQUIRE(app.Shutdown() == 7);
}

TEST_CASE_METHOD(Fixture, "single_instance held elsewhere fails Init before any module", "[app]") {
  auto held = rex::platform::InstanceLock::Acquire("rex-app-test");
  REQUIRE(held);
  TestApp app;
  app.lock_name = "rex-app-test";
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == std::vector<std::string>{"app:configure"});
}

TEST_CASE_METHOD(Fixture, "Shutdown is idempotent after any failure", "[app]") {
  TestApp app;
  app.fail_configure = true;
  REQUIRE_FALSE(app.Init(Args()));
  REQUIRE(app.Shutdown() == 1);
  const auto events = g_events;
  REQUIRE(app.Shutdown() == 1);
  REQUIRE(g_events == events);
}

TEST_CASE_METHOD(Fixture, "Shutdown is idempotent after a run and after a failed OnInit", "[app]") {
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

TEST_CASE_METHOD(Fixture, "Init names the log after the executable", "[app]") {
  TestApp app;
  app.modules = {};
  REQUIRE(app.Init(Args()));
  const std::filesystem::path file = app.api().Get<rex::ILogger>()->FilePath();
  REQUIRE(file.parent_path() == rex::platform::GetExecutableFolder() / "logs");
  REQUIRE(file.filename().string().starts_with(rex::platform::GetExecutablePath().stem().string() +
                                               "_"));
  app.Shutdown();
}

TEST_CASE_METHOD(Fixture, "A failed relaunch reaches the log before logging shuts down", "[app]") {
  rex::detail::SetRelaunchForTest(&FailRelaunch, true);
  TestApp app;
  app.modules = {};
  REQUIRE(app.Init(Args()));
  auto* logger = app.api().Get<rex::ILogger>();
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  rex::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  std::vector<rex::log::CapturedRecord> entries;
  sink.CopySince(0, entries);
  logger->RemoveSink(sink);
  REQUIRE(std::ranges::any_of(entries, [](const rex::log::CapturedRecord& entry) {
    return entry.text.find("relaunch failed") != std::string::npos;
  }));
}

TEST_CASE_METHOD(Fixture, "Shutdown without Init returns 0", "[app]") {
  TestApp app;
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_events.empty());
}

TEST_CASE_METHOD(Fixture, "RequestRestart relaunches with the given arguments", "[app]") {
  TestApp app;
  app.modules = {"rex-filesystem", "rex-runtime", "rex-ui-sdl3"};
  REQUIRE(app.Init(Args({"--ui.fullscreen=false"})));
  rex::RequestRestart({"--profile", "b"});
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(g_relaunched == std::vector<std::string>{"--profile", "b"});
}

TEST_CASE_METHOD(Fixture, "RequestRestart without relaunch support starts nothing", "[app]") {
  rex::detail::SetRelaunchForTest(&FakeRelaunch, false);
  TestApp app;
  app.modules = {};
  REQUIRE(app.Init(Args()));
  rex::RequestRestart();
  REQUIRE(app.Shutdown() == 0);
  REQUIRE(std::ranges::find(g_events, "relaunch") == g_events.end());
}

TEST_CASE("A category keeps writing after the logger stops and starts", "[app]") {
  auto first = rex::Init(rex::Config{});
  REQUIRE(first);
  auto* logger = first.value()->Get<rex::ILogger>();
  rex::Shutdown();
  REQUIRE(rex::Init(rex::Config{}));
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  REXLOG_WARN(core, "after logging shut down");
  std::vector<rex::log::CapturedRecord> entries;
  sink.CopySince(0, entries);
  logger->RemoveSink(sink);
  rex::Shutdown();
  REQUIRE(std::ranges::any_of(entries, [](const rex::log::CapturedRecord& entry) {
    return entry.text.find("after logging shut down") != std::string::npos;
  }));
}
