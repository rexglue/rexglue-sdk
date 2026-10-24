/**
 * @file        tests/unit/core/module_loader_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/platform/file.h>
#include <rex/logging/log.h>
#include <rex/logging/sink.h>
#include <rex/module.h>
#include <rex/platform/dynlib.h>

#include "core/module_loader.h"
#include "modules/echo.h"
#include "modules/test_module_sink.h"

namespace {

std::vector<std::string> g_events;

void Sink(const char* module, const char* event) {
  g_events.push_back(std::string(module) + ":" + event);
}

/**
 * Holds a second reference to a test module so its statics outlive rex::Shutdown.
 * Installs the event sink before the loader constructs the module instance.
 */
class Pinned {
 public:
  explicit Pinned(const char* name) {
    auto path = rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName(name);
    REQUIRE(library_.Load(path, rex::platform::SymbolResolution::kImmediate));
    auto set_sink = library_.GetSymbol<void (*)(ReXModTestSink)>("rexmod_test_set_sink");
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

REX_TEST_CASE_METHOD(Fixture, "Init with no modules succeeds and can repeat",
                     "[core][module_loader]") {
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  REQUIRE(api.value()->GetConfig().modules.empty());
  REQUIRE_FALSE(api.value()->IsLoaded("anything"));
  rex::Shutdown();
  REQUIRE(rex::Init(rex::Config{}));
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown without Init is a no-op", "[core][module_loader]") {
  rex::Shutdown();
  rex::Shutdown();
  REQUIRE(rex::Init(rex::Config{}));
}

REX_TEST_CASE_METHOD(Fixture, "Init twice fails and keeps the first", "[core][module_loader]") {
  Pinned pin("rexmod-test-good");
  REQUIRE(rex::Init(With({"rexmod-test-good"})));
  auto second = rex::Init(With({"rexmod-test-good"}));
  REQUIRE_FALSE(second);
  REQUIRE(second.error().message == "rex::Init called while initialized");
  REQUIRE(rex::detail::ModuleLoader::GetInstance().IsLoaded("rexmod-test-good"));
}

REX_TEST_CASE_METHOD(Fixture, "Good module loads, serves echo and unwinds in order",
                     "[core][module_loader]") {
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

REX_TEST_CASE_METHOD(Fixture,
                     "The only interface request before Init is the cvar registry fetch at Connect",
                     "[core][module_loader]") {
  Pinned pin("rexmod-test-good");
  REQUIRE(rex::Init(With({"rexmod-test-good"})));
  REQUIRE(rex::detail::GetInterfaceRequestCount() == 1);
  REQUIRE(g_events.front() == "rexmod-test-good:constructor");
  REQUIRE(g_events[1] == "rexmod-test-good:connect");
}

REX_TEST_CASE_METHOD(Fixture, "The interface factory reports a missing or mismatched interface",
                     "[core][module_loader]") {
  REQUIRE(rex::Init(rex::Config{}));
  rex::IInterfaceFactory& factory = rex::detail::ModuleLoader::GetInstance();
  using Registry = rex::cvar::ICvarRegistry;
  auto typed = factory.Get<Registry>();
  REQUIRE(typed);
  CHECK(*typed != nullptr);
  auto missing = factory.Get("rex.nothing", 1);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == static_cast<int>(rex::InterfaceStatus::kNotFound));
  auto wrong = factory.Get(Registry::kInterfaceName, Registry::kInterfaceVersion + 1);
  REQUIRE_FALSE(wrong);
  CHECK(wrong.error().code == static_cast<int>(rex::InterfaceStatus::kVersionMismatch));
}

REX_TEST_CASE_METHOD(Fixture, "ReXCreateInterface with a null name reports NotFound",
                     "[core][module_loader]") {
  Pinned pin("rexmod-test-good");
  auto create = pin.Library().GetSymbol<rex::CreateInterfaceFn>(rex::kCreateInterfaceSymbol);
  REQUIRE(create != nullptr);
  rex::i32 status = -1;
  REQUIRE(create(nullptr, 1, &status) == nullptr);
  REQUIRE(status == std::to_underlying(rex::InterfaceStatus::kNotFound));
  status = -1;
  REQUIRE(create(rex::IModule::kInterfaceName, 99, &status) == nullptr);
  REQUIRE(status == std::to_underlying(rex::InterfaceStatus::kVersionMismatch));
}

REX_TEST_CASE_METHOD(Fixture, "Missing requirement fails before any Connect",
                     "[core][module_loader]") {
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

REX_TEST_CASE_METHOD(Fixture, "Bad module version is rejected", "[core][module_loader]") {
  auto api = rex::Init(With({"rexmod-test-badversion"}));
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message ==
          "module 'rexmod-test-badversion' does not serve rex.module version 1 (status 2)");
}

REX_TEST_CASE_METHOD(Fixture, "A module that exports no build identity is rejected",
                     "[core][module_loader]") {
  auto api = rex::Init(With({"rexmod-test-noidentity"}));
  REQUIRE_FALSE(api);
  const auto path =
      rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexmod-test-noidentity");
  REQUIRE(api.error().message ==
          std::format("module 'rexmod-test-noidentity' at '{}' exports no ReXBuildIdentity, it "
                      "was not built against this SDK build '{}'",
                      path.string(), rex::kBuildIdentity));
}

REX_TEST_CASE_METHOD(Fixture, "A module built against another SDK build is rejected",
                     "[core][module_loader]") {
  auto api = rex::Init(With({"rexmod-test-otheridentity"}));
  REQUIRE_FALSE(api);
  const auto path = rex::platform::GetExecutableFolder() /
                    rex::detail::ModuleFileName("rexmod-test-otheridentity");
  REQUIRE(api.error().message ==
          std::format("module 'rexmod-test-otheridentity' at '{}' was built against SDK build "
                      "'0.0.0-000000000000', this SDK build is '{}'",
                      path.string(), rex::kBuildIdentity));
}

REX_TEST_CASE_METHOD(Fixture, "Unknown module reports the composed path", "[core][module_loader]") {
  auto api = rex::Init(With({"rexmod-test-nowhere"}));
  REQUIRE_FALSE(api);
  auto expected =
      (rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexmod-test-nowhere"))
          .string();
  REQUIRE(api.error().message.starts_with("module 'rexmod-test-nowhere' not found at '" + expected +
                                          "'"));
}

REX_TEST_CASE_METHOD(Fixture, "Init failure unwinds the modules before it",
                     "[core][module_loader]") {
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

REX_TEST_CASE_METHOD(Fixture, "Duplicate name and invalid name are rejected before load",
                     "[core][module_loader]") {
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

REX_TEST_CASE_METHOD(Fixture, "An interface served by 2 modules is rejected",
                     "[core][module_loader]") {
  Pinned good("rexmod-test-good");
  Pinned dup("rexmod-test-duplicate-echo");
  auto api = rex::Init(With({"rexmod-test-good", "rexmod-test-duplicate-echo"}));
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message ==
          "interface rex.test.echo is served by both 'rexmod-test-good' and "
          "'rexmod-test-duplicate-echo'");
  for (const auto& event : g_events) {
    REQUIRE(event.find(":connect") == std::string::npos);
  }
}

REX_TEST_CASE("ParseCommandLine reads --modules in both forms", "[core][module_loader]") {
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
REXCVAR_INT32(app, count, 1, "Consumer count").SetRange(0, 10);
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

REX_TEST_CASE_METHOD(Fixture,
                     "Init registers consumer and module tables and serves the cvar registry",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.tables.push_back({"app", app_cvars::kAppCvars});
  cfg.cvars.Set("echo.value", "5");
  cfg.cvars.values.push_back({"app.count", "7", rex::cvar::CvarSource::kConfig});
  auto api = rex::Init(cfg);
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE(registry != nullptr);
  REQUIRE(registry->IsFinalized());
  REQUIRE(HasScope(registry->GetScopes(), "app"));
  REQUIRE(HasScope(registry->GetScopes(), "echo"));
  REQUIRE(registry->Get("echo.value").value() == "5");
  REQUIRE(api.value()->Get<rex::test::IEcho>()->GetValue() == 5);
  REQUIRE(app_cvars::count.Get() == 7);
  REQUIRE(registry->GetSource("app.count") == rex::cvar::CvarSource::kConfig);
  REQUIRE(api.value()->GetConfig().modules == std::vector<std::string>{"rexmod-test-good"});
  rex::Shutdown();
  REQUIRE(app_cvars::debug.GetRegistry() == nullptr);
  auto again = rex::Init(rex::Config{});
  REQUIRE(again);
  auto fresh = again.value()->Get<rex::cvar::ICvarRegistry>()->GetScopes();
  REQUIRE_FALSE(HasScope(fresh, "app"));
  REQUIRE_FALSE(HasScope(fresh, "echo"));
  REQUIRE(app_cvars::count.Parse("1"));
}

REX_TEST_CASE_METHOD(Fixture, "Unknown CommandLine override fails Init before any module Init",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.Set("nope.value", "1");
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("unknown cvar 'nope.value' from CommandLine") !=
          std::string::npos);
  REQUIRE_FALSE(Ran("rexmod-test-good:init"));
}

REX_TEST_CASE_METHOD(Fixture, "unknown Config override warns and Init succeeds",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.values.push_back({"vsync", "false", rex::cvar::CvarSource::kConfig});
  rex::log::CaptureSink sink;
  cfg.log.sinks.push_back(&sink);
  auto api = rex::Init(cfg);
  REQUIRE(api);
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  bool warned = false;
  for (const auto& entry : entries) {
    if (entry.text.find("unknown cvar 'vsync' from Config") != std::string::npos)
      warned = true;
  }
  rex::Shutdown();
  REQUIRE(warned);
}

REX_TEST_CASE_METHOD(Fixture, "Init rejects an unparsable command line value",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.Set("echo.value", "abc");
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("cvar 'echo.value' rejected value 'abc'") != std::string::npos);
  REQUIRE(api.error().message.find("from CommandLine") != std::string::npos);
  REQUIRE_FALSE(Ran("rexmod-test-good:init"));
}

REX_TEST_CASE_METHOD(Fixture, "Init rejects --no- on a cvar that is not a bool",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.SetNegated("echo.value");
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("cvar 'echo.value' is not a bool, so --no-echo.value cannot "
                                   "set it") != std::string::npos);
  REQUIRE_FALSE(Ran("rexmod-test-good:init"));
}

REX_TEST_CASE_METHOD(Fixture, "Init accepts --no- on a bool cvar", "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.SetNegated("echo.ready");
  auto api = rex::Init(cfg);
  REQUIRE(api);
  rex::Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "Init rejects an unparsable environment value",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  cfg.cvars.values.push_back({"echo.ready", "off-ish", rex::cvar::CvarSource::kEnvironment});
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("rejected value 'off-ish' from Environment") !=
          std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "A consumer table with a bad name fails before any load",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexmod-test-good"});
  static const rex::cvar::VarBase* wrong_scope[] = {&app_cvars::debug};
  cfg.cvars.tables.push_back({"other", wrong_scope});
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("consumer table 'other'") != std::string::npos);
  REQUIRE(g_events.empty());
}

REX_TEST_CASE_METHOD(Fixture, "Config validation fails before any module connects",
                     "[core][module_loader][cvar]") {
  Pinned pin("rexmod-test-good");
  auto cfg = With({"rexfilesystem", "rexruntime", "rexmod-test-good"});
  cfg.runtime.heap_size_mb = 0;
  auto api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("runtime.heap_size_mb") != std::string::npos);
  REQUIRE_FALSE(Ran("rexmod-test-good:connect"));
  cfg.runtime.heap_size_mb = 256;
  cfg.modules.push_back("rexgpu-xenos");
  cfg.gpu.resolution_scale_x = 9;
  api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("gpu.resolution_scale_x") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "Locks apply after values and unknown locks fail",
                     "[core][module_loader][cvar]") {
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
  REQUIRE(set.error().code == static_cast<int>(rex::cvar::CvarError::kLocked));
  rex::Shutdown();
  cfg.cvars.locked = {"nope.value"};
  api = rex::Init(cfg);
  REQUIRE_FALSE(api);
  REQUIRE(api.error().message.find("unknown cvar 'nope.value'") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "Every SDK cvar name is scope.leaf in a known scope",
                     "[core][module_loader][cvar]") {
  auto cfg = With({"rexfilesystem", "rexruntime", "rexui-sdl3"});
  auto xenos = rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexgpu-xenos");
  auto vulkan =
      rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexgpu-xenos-vulkan");
  if (std::filesystem::exists(xenos) && std::filesystem::exists(vulkan)) {
    cfg.gpu.backend = "vulkan";
    cfg.modules.push_back("rexgpu-xenos");
    cfg.modules.push_back("rexgpu-xenos-vulkan");
  }
  auto api = rex::Init(cfg);
  if (!api && cfg.modules.size() > 3) {
    cfg.modules.resize(3);
    cfg.gpu.backend = "null";
    api = rex::Init(cfg);
  }
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  const auto scopes = registry->GetScopes();
  size_t count = 0;
  for (const auto* var : registry->Enumerate()) {
    const auto& desc = var->GetDesc();
    ++count;
    auto dot = desc.name.find('.');
    REQUIRE(dot != std::string_view::npos);
    REQUIRE(desc.name.find('.', dot + 1) == std::string_view::npos);
    REQUIRE(desc.name.substr(0, dot) == desc.scope);
    REQUIRE(std::find(scopes.begin(), scopes.end(), desc.scope) != scopes.end());
    for (char c : desc.name) {
      REQUIRE((std::islower(static_cast<unsigned char>(c)) ||
               std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '.'));
    }
  }
  REQUIRE(count > 0);
}

REX_TEST_CASE_METHOD(Fixture, "rexruntime registers the module scopes at Connect",
                     "[core][module_loader][cvar]") {
  auto api = rex::Init(With({"rexfilesystem", "rexruntime"}));
  REQUIRE(api);
  REQUIRE(api.value()->IsLoaded("rexruntime"));
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE(registry->Find("runtime.protect_zero"));
  REQUIRE_FALSE(registry->Find("ui.window_width"));
  auto scopes = registry->GetScopes();
  REQUIRE(std::find(scopes.begin(), scopes.end(), "bind") == scopes.end());
}

REX_TEST_CASE_METHOD(Fixture, "Runtime config sets the code segment and zero page defaults",
                     "[core][module_loader][cvar]") {
  auto cfg = With({"rexfilesystem", "rexruntime"});
  cfg.runtime.writable_code_segments = true;
  cfg.runtime.protect_zero = false;
  auto api = rex::Init(cfg);
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE(registry->Get("runtime.writable_code_segments").value() == "true");
  REQUIRE(registry->Get("runtime.protect_zero").value() == "false");
}

REX_TEST_CASE_METHOD(Fixture, "Without rexruntime no module scope exists",
                     "[core][module_loader][cvar]") {
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  auto* registry = api.value()->Get<rex::cvar::ICvarRegistry>();
  REQUIRE_FALSE(registry->Find("runtime.protect_zero"));
  REQUIRE_FALSE(registry->Find("ui.window_width"));
  auto scopes = registry->GetScopes();
  REQUIRE(std::find(scopes.begin(), scopes.end(), "bind") == scopes.end());
  REQUIRE(registry->Find("log.level"));
}

REX_TEST_CASE("LibraryFileName adds the platform prefix and extension", "[core][modules]") {
#if REX_PLATFORM_WIN32
  CHECK(rex::detail::LibraryFileName("demo_dll") == "demo_dll.dll");
#elif REX_PLATFORM_MAC
  CHECK(rex::detail::LibraryFileName("demo_dll") == "libdemo_dll.dylib");
#else
  CHECK(rex::detail::LibraryFileName("demo_dll") == "libdemo_dll.so");
#endif
}

REX_TEST_CASE("Attach opens a library only while the process has it mapped", "[core][modules]") {
  const auto path = rex::platform::GetExecutableFolder() /
                    rex::detail::ModuleFileName("rexmod-test-recompiled-b");
  rex::platform::DynamicLibrary attached;
  CHECK_FALSE(attached.Attach(path));
  CHECK_FALSE(attached);
  rex::platform::DynamicLibrary loaded;
  REQUIRE(loaded.Load(path));
  CHECK(attached.Attach(path));
  loaded.Close();
  attached.Close();
  CHECK_FALSE(attached.Attach(path));
}

REX_TEST_CASE("A module is searched in the executable folder, then the library folder",
              "[core][module_loader]") {
  const std::filesystem::path root = "install";
  const auto file = rex::detail::ModuleFileName("rexfilesystem");
  const auto paths = rex::detail::GetModuleSearchPaths(root / "bin", "rexfilesystem", "../lib64");
  REQUIRE(paths.size() == 2);
  CHECK(paths[0] == root / "bin" / file);
  CHECK(paths[1] == root / "lib64" / file);
}

REX_TEST_CASE("A module has one search path when no library folder is given",
              "[core][module_loader]") {
  const auto paths = rex::detail::GetModuleSearchPaths("bin", "rexfilesystem", "");
  REQUIRE(paths.size() == 1);
  CHECK(paths[0] == std::filesystem::path("bin") / rex::detail::ModuleFileName("rexfilesystem"));
}