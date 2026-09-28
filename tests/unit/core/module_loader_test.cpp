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

#include <string>
#include <vector>

#include <rex/api.h>
#include <rex/filesystem.h>
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
    auto path = rex::filesystem::GetExecutableFolder() / rex::detail::ModuleFileName(name);
    REQUIRE(library_.Load(path, rex::platform::SymbolResolution::kImmediate));
    auto set_sink = library_.GetSymbol<void (*)(RexModTestSink)>("rexmod_test_set_sink");
    REQUIRE(set_sink != nullptr);
    set_sink(&Sink);
  }
  rex::platform::DynamicLibrary& library() { return library_; }

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

TEST_CASE_METHOD(Fixture, "No interface requests happen before Connect", "[module_loader]") {
  Pinned pin("rexmod-test-good");
  REQUIRE(rex::Init(With({"rexmod-test-good"})));
  REQUIRE(rex::detail::InterfaceRequestCount() == 0);
  REQUIRE(g_events.front() == "rexmod-test-good:constructor");
  REQUIRE(g_events[1] == "rexmod-test-good:connect");
}

TEST_CASE_METHOD(Fixture, "rex_create_interface with a null name reports NotFound",
                 "[module_loader]") {
  Pinned pin("rexmod-test-good");
  auto create = pin.library().GetSymbol<rex::CreateInterfaceFn>(rex::kCreateInterfaceSymbol);
  REQUIRE(create != nullptr);
  int32_t status = -1;
  REQUIRE(create(nullptr, 1, &status) == nullptr);
  REQUIRE(status == static_cast<int32_t>(rex::InterfaceStatus::NotFound));
  status = -1;
  REQUIRE(create(rex::IModule::kInterfaceName, 99, &status) == nullptr);
  REQUIRE(status == static_cast<int32_t>(rex::InterfaceStatus::VersionMismatch));
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
      (rex::filesystem::GetExecutableFolder() / rex::detail::ModuleFileName("rexmod-test-nowhere"))
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
  auto rest = rex::ParseCommandLine(cfg, 6, argv);
  REQUIRE(cfg.modules == std::vector<std::string>{"alpha", "beta", "gamma"});
  REQUIRE(rest == std::vector<std::string>{"--foo", "--modules"});
}
