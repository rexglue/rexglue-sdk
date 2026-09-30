/**
 * @file        tests/unit/core/module_loader_test.cpp
 * @brief       Unit tests for rex::Init, rex::Shutdown, the interface registry and ParseCommandLine
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/platform/file.h>
#include <rex/logging.h>
#include <rex/logging/sink.h>
#include <rex/platform/dynlib.h>

#include "core/module_loader.h"
#include "modules/echo.h"
#include "modules/test_module_common.h"

namespace {

std::vector<std::string> g_events;

void Sink(const char* module, const char* event) {
  g_events.push_back(std::string(module) + ":" + event);
}

// Holds a second reference to a test module so its statics outlive rex::Shutdown, and
// installs the event sink before the loader constructs the module instance.
class Pinned {
 public:
  explicit Pinned(const char* name) {
    auto path = rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName(name);
    REQUIRE(library_.Load(path, rex::platform::SymbolResolution::kImmediate));
    auto set_sink = library_.GetSymbol<void (*)(RexModTestSink)>("rexmod_test_set_sink");
    REQUIRE(set_sink != nullptr);
    set_sink(&Sink);
  }
  rex::platform::DynamicLibrary& Library() { return library_; }

 private:
  rex::platform::DynamicLibrary library_;
};

struct Fixture {
  Fixture() {
    g_events.clear();
    rex::Shutdown();
  }
  ~Fixture() { rex::Shutdown(); }
};

rex::Config With(std::vector<std::string> modules) {
  rex::Config cfg;
  cfg.modules = std::move(modules);
  return cfg;
}

}  // namespace

TEST_CASE_METHOD(Fixture, "Init with no modules succeeds and can repeat", "[module_loader]") {
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  REQUIRE_FALSE(api.value()->IsLoaded("anything"));
  rex::Shutdown();
  REQUIRE(rex::Init(rex::Config{}));
}

TEST_CASE_METHOD(Fixture, "Shutdown without Init is a no-op", "[module_loader]") {
  rex::Shutdown();
  rex::Shutdown();
  REQUIRE(rex::Init(rex::Config{}));
}

TEST_CASE_METHOD(Fixture, "Init twice fails and keeps the first", "[module_loader]") {
  Pinned pin("rexmod-test-good");
  REQUIRE(rex::Init(With({"rexmod-test-good"})));
  auto second = rex::Init(With({"rexmod-test-good"}));
  REQUIRE_FALSE(second);
  REQUIRE(second.error().message == "rex::Init called while initialized");
  REQUIRE(rex::detail::ModuleLoader::Instance().IsLoaded("rexmod-test-good"));
}

TEST_CASE_METHOD(Fixture, "Good module loads, serves echo and unwinds in order",
                 "[module_loader]") {
  Pinned pin("rexmod-test-good");
  auto api = rex::Init(With({"rexmod-test-good"}));
  REQUIRE(api);
  REQUIRE(api.value()->IsLoaded("rexmod-test-good"));
  auto* echo = api.value()->Get<rex::test::IEcho>();
  REQUIRE(echo != nullptr);
  REQUIRE(echo->Echo(41) == 41);
  REQUIRE(g_events == std::vector<std::string>{"rexmod-test-good:constructor",
                                               "rexmod-test-good:connect",
                                               "rexmod-test-good:init"});
  rex::Shutdown();
  REQUIRE(g_events.size() == 5);
  REQUIRE(g_events[3] == "rexmod-test-good:shutdown");
  REQUIRE(g_events[4] == "rexmod-test-good:disconnect");
}

TEST_CASE_METHOD(Fixture, "The only interface request before Init is the rex.cvar fetch at Connect",
                 "[module_loader]") {
  Pinned pin("rexmod-test-good");
  REQUIRE(rex::Init(With({"rexmod-test-good"})));
  REQUIRE(rex::detail::InterfaceRequestCount() == 1);
  REQUIRE(g_events.front() == "rexmod-test-good:constructor");
  REQUIRE(g_events[1] == "rexmod-test-good:connect");
}

TEST_CASE_METHOD(Fixture, "rex_create_interface with a null name reports NotFound",
                 "[module_loader]") {
  Pinned pin("rexmod-test-good");
  auto create = pin.Library().GetSymbol<rex::CreateInterfaceFn>(rex::kCreateInterfaceSymbol);
  REQUIRE(create != nullptr);
  rex::i32 status = -1;
  REQUIRE(create(nullptr, 1, &status) == nullptr);
  REQUIRE(status == static_cast<rex::i32>(rex::InterfaceStatus::NotFound));
  status = -1;
  REQUIRE(create(rex::IModule::kInterfaceName, 99, &status) == nullptr);
  REQUIRE(status == static_cast<rex::i32>(rex::InterfaceStatus::VersionMismatch));
}

TEST_CASE_METHOD(Fixture, "Missing requirement fails before any Connect", "[module_loader]") {
  Pinned good("rexmod-test-good");
  Pinned missing("rexmod-test-missing");
  auto api = rex::Init(With({"rexmod-test-good", "rexmod-test-missing"}));
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message ==
          "module 'rexmod-test-missing' requires rex.test.absent version 1, which no module before "
          "it serves");
  for (const auto& event : g_events) {
    REQUIRE(event.find(":connect") == std::string::npos);
  }
}

TEST_CASE_METHOD(Fixture, "Bad module version is rejected", "[module_loader]") {
  auto api = rex::Init(With({"rexmod-test-badversion"}));
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message ==
          "module 'rexmod-test-badversion' does not serve rex.module version 1 (status 2)");
}

TEST_CASE_METHOD(Fixture, "Unknown module reports the composed path", "[module_loader]") {
  auto api = rex::Init(With({"rexmod-test-nowhere"}));
  REQUIRE_FALSE(api);
  auto expected =
      (rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexmod-test-nowhere"))
          .string();
  REQUIRE(api.error().message == "module 'rexmod-test-nowhere' not found at '" + expected + "'");
}

TEST_CASE_METHOD(Fixture, "Init failure unwinds the modules before it", "[module_loader]") {
  Pinned good("rexmod-test-good");
  Pinned failinit("rexmod-test-failinit");
  auto api = rex::Init(With({"rexmod-test-good", "rexmod-test-failinit"}));
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message == "module 'rexmod-test-failinit' Init failed: failinit refuses");
  REQUIRE(g_events == std::vector<std::string>{
                          "rexmod-test-good:constructor", "rexmod-test-failinit:constructor",
                          "rexmod-test-good:connect", "rexmod-test-failinit:connect",
                          "rexmod-test-good:init", "rexmod-test-failinit:init",
                          "rexmod-test-good:shutdown", "rexmod-test-failinit:disconnect",
                          "rexmod-test-good:disconnect"});
  REQUIRE(rex::Init(rex::Config{}));
}

TEST_CASE_METHOD(Fixture, "Duplicate name and invalid name are rejected before load",
                 "[module_loader]") {
  auto dup = rex::Init(With({"rexmod-test-good", "rexmod-test-good"}));
  REQUIRE_FALSE(dup);
  REQUIRE(dup.error().message == "module 'rexmod-test-good' listed twice");
  REQUIRE(g_events.empty());
  for (const char* bad : {"../evil", "sub/evil", "sub\\evil", "c:evil", ""}) {
    auto api = rex::Init(With({bad}));
    REQUIRE_FALSE(api);
    REQUIRE(api.error().message == std::string("module '") + bad + "' has an invalid name");
  }
  REQUIRE_FALSE(rex::detail::IsValidModuleName(".."));
  REQUIRE(rex::detail::IsValidModuleName("rexgpu-xenos"));
}

TEST_CASE_METHOD(Fixture, "An interface served by 2 modules is rejected", "[module_loader]") {
  Pinned good("rexmod-test-good");
  Pinned dup("rexmod-test-dupecho");
  auto api = rex::Init(With({"rexmod-test-good", "rexmod-test-dupecho"}));
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message ==
          "interface rex.test.echo is served by both 'rexmod-test-good' and 'rexmod-test-dupecho'");
  for (const auto& event : g_events) {
    REQUIRE(event.find(":connect") == std::string::npos);
  }
}

TEST_CASE("ParseCommandLine reads --modules in both forms", "[module_loader]") {
  rex::Config cfg;
  char a0[] = "app";
  char a1[] = "--modules=alpha,beta";
  char a2[] = "--foo";
  char a3[] = "--modules";
  char a4[] = "gamma";
  char a5[] = "--modules";
  char* argv[] = {a0, a1, a2, a3, a4, a5};
  auto rest = rex::ParseCommandLine(cfg, rex::CLIArgList::Parse(6, argv));
  REQUIRE(cfg.modules == std::vector<std::string>{"alpha", "beta", "gamma"});
  REQUIRE(rest == std::vector<std::string>{"--foo", "--modules"});
}

namespace app_cvars {
REXCVAR_BOOL(app, debug, false, "Consumer flag");
REXCVAR_INT32(app, count, 1, "Consumer count").range(0, 10);
REXCVAR_TABLE(kAppCvars, debug, count);
}  // namespace app_cvars

namespace {

bool HasScope(const std::vector<std::string_view>& scopes, std::string_view scope) {
  return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

bool Ran(const char* event) {
  return std::find(g_events.begin(), g_events.end(), event) != g_events.end();
}

}  // namespace

TEST_CASE_METHOD(Fixture, "Init registers consumer and module tables and serves rex.cvar",
                 "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.tables.push_back({"app", app_cvars::kAppCvars});
  cfg.cvars.Set("echo.value", "5");
  cfg.cvars.values.push_back({"app.count", "7", rex::cvar::CvarSource::Config});
  auto api = rex::Init(cfg);
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE(registry != nullptr);
  REQUIRE(registry->IsFinalized());
  REQUIRE(HasScope(registry->Scopes(), "app"));
  REQUIRE(HasScope(registry->Scopes(), "echo"));
  REQUIRE(registry->Get("echo.value").value() == "5");
  REQUIRE(api.value()->Get<rex::test::IEcho>()->Value() == 5);
  REQUIRE(app_cvars::count.get() == 7);
  REQUIRE(registry->Source("app.count") == rex::cvar::CvarSource::Config);
  REQUIRE(api.value()->config().modules == std::vector<std::string>{"rexmod-test-good"});
  rex::Shutdown();
  REQUIRE(app_cvars::debug.registry() == nullptr);
  auto again = rex::Init(rex::Config{});
  REQUIRE(again);
  auto fresh = again.value()->Get<rex::cvar::ICvarRegistry>()->Scopes();
  REQUIRE_FALSE(HasScope(fresh, "app"));
  REQUIRE_FALSE(HasScope(fresh, "echo"));
  REQUIRE(app_cvars::count.Parse("1"));
}

TEST_CASE_METHOD(Fixture, "Unknown CommandLine override fails Init before any module Init",
                 "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.Set("nope.value", "1");
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("unknown cvar 'nope.value' from CommandLine") !=
          std::string::npos);
  REQUIRE_FALSE(Ran("rexmod-test-good:init"));
}

TEST_CASE_METHOD(Fixture, "unknown Config override warns and Init succeeds",
                 "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.values.push_back({"vsync", "false", rex::cvar::CvarSource::Config});
  auto sink = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(sink);
  auto api = rex::Init(cfg);
  REQUIRE(api);
  std::vector<rex::LogEntry> entries;
  sink->CopyEntries(entries);
  bool warned = false;
  for (const auto& entry : entries) {
    if (entry.text.find("unknown cvar 'vsync' from Config") != std::string::npos)
      warned = true;
  }
  REQUIRE(warned);
  rex::RemoveSink(sink);
}

TEST_CASE_METHOD(Fixture, "Init rejects an unparsable command line value",
                 "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.Set("echo.value", "abc");
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("cvar 'echo.value' rejected value 'abc'") != std::string::npos);
  REQUIRE(api.error().message.find("from CommandLine") != std::string::npos);
  REQUIRE_FALSE(Ran("rexmod-test-good:init"));
}

TEST_CASE_METHOD(Fixture, "Init rejects an unparsable environment value", "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.values.push_back({"echo.ready", "off-ish", rex::cvar::CvarSource::Environment});
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("rejected value 'off-ish' from Environment") !=
          std::string::npos);
}

TEST_CASE_METHOD(Fixture, "A consumer table with a bad name fails before any load",
                 "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  static const rex::cvar::IVar* wrong_scope[] = {&app_cvars::debug};
  cfg.cvars.tables.push_back({"other", wrong_scope});
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("consumer table 'other'") != std::string::npos);
  REQUIRE(g_events.empty());
}

TEST_CASE_METHOD(Fixture, "Slice validation fails before any module connects",
                 "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexfilesystem", "rexruntime", "rexmod-test-good"});
  cfg.runtime.heap_size_mb = 0;
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("runtime.heap_size_mb") != std::string::npos);
  REQUIRE_FALSE(Ran("rexmod-test-good:connect"));
  cfg.runtime.heap_size_mb = 256;
  cfg.graphics.resolution_scale_x = 9;
  api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("graphics.resolution_scale_x") != std::string::npos);
}

TEST_CASE_METHOD(Fixture, "Locks apply after values and unknown locks fail",
                 "[module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.Set("echo.value", "3");
  cfg.cvars.Lock("echo.value");
  auto api = rex::Init(cfg);
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE(registry->Get("echo.value").value() == "3");
  auto set = registry->Set("echo.value", "4");
  REQUIRE_FALSE(set);
  REQUIRE(set.error().code == static_cast<int>(rex::cvar::Error::Locked));
  rex::Shutdown();
  cfg.cvars.locked = {"nope.value"};
  api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("unknown cvar 'nope.value'") != std::string::npos);
}

TEST_CASE_METHOD(Fixture, "Every SDK cvar name is scope.leaf in a charter scope",
                 "[module_loader][cvar]") {
  static const std::vector<std::string_view> kScopes = {"log",   "clock",  "perf",    "runtime",
                                                        "audio", "input",  "ui",      "gpu",
                                                        "d3d12", "vulkan", "console", "bind"};
  auto cfg = With({"rexfilesystem", "rexruntime"});
  auto xenos = rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexgpu-xenos");
  if (std::filesystem::exists(xenos))
    cfg.modules.push_back("rexgpu-xenos");
  auto api = rex::Init(cfg);
  if (!api && cfg.modules.size() > 2) {
    cfg.modules.pop_back();
    api = rex::Init(cfg);
  }
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  size_t count = 0;
  for (const auto* var : registry->Enumerate()) {
    const auto& desc = var->desc();
    ++count;
    auto dot = desc.name.find('.');
    REQUIRE(dot != std::string_view::npos);
    REQUIRE(desc.name.find('.', dot + 1) == std::string_view::npos);
    REQUIRE(std::find(kScopes.begin(), kScopes.end(), desc.scope) != kScopes.end());
    for (char c : desc.name) {
      REQUIRE((std::islower(static_cast<unsigned char>(c)) ||
               std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '.'));
    }
  }
  REQUIRE(count >= 95);
  if (cfg.modules.size() > 2)
    REQUIRE(count == 166);
}

TEST_CASE_METHOD(Fixture, "rexruntime registers the module scopes at Connect",
                 "[module_loader][cvar]") {
  auto api = rex::Init(With({"rexfilesystem", "rexruntime"}));
  REQUIRE(api);
  REQUIRE(api.value()->IsLoaded("rexruntime"));
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE(registry->Find("runtime.protect_zero") != nullptr);
  REQUIRE(registry->Find("ui.window_width") != nullptr);
  auto scopes = registry->Scopes();
  REQUIRE(std::find(scopes.begin(), scopes.end(), "bind") != scopes.end());
}

TEST_CASE_METHOD(Fixture, "Without rexruntime no module scope exists", "[module_loader][cvar]") {
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE(registry->Find("runtime.protect_zero") == nullptr);
  REQUIRE(registry->Find("ui.window_width") == nullptr);
  auto scopes = registry->Scopes();
  REQUIRE(std::find(scopes.begin(), scopes.end(), "bind") == scopes.end());
  REQUIRE(registry->Find("log.level") != nullptr);
}
