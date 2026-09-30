/**
 * @file        tests/unit/core/cvar_test.cpp
 * @brief       Unit tests for the cvar types and the registry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "core/cvar_registry.h"

namespace {

bool AtLeast3(std::string_view v) {
  return v.size() >= 3;
}

std::string g_last_command_args;

void RecordCommand(std::string_view args) {
  g_last_command_args = std::string(args);
}

}  // namespace

namespace test_cvars {
REXCVAR_BOOL(test, flag, true, "A bool");
REXCVAR_INT32(test, count, 42, "An int32").range(1, 100);
REXCVAR_DOUBLE(test, ratio, 0.5, "A double").range(0.0, 1.0);
REXCVAR_STRING(test, mode, "low", "A string").allowed(REXCVAR_ALLOWED("low", "medium", "high"));
REXCVAR_STRING(test, word, "abc", "Validated").validator(&AtLeast3);
REXCVAR_INT32(test, once, 1, "Init only").lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_INT32(test, restart, 1, "Requires restart")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_UINT64(test, big, 5, "A uint64").range(0, 1.8e19);
REXCVAR_BOOL(test, hidden, false, "Debug only").debug_only();
REXCVAR_COMMAND_ARGS(test, run, RecordCommand, "Records its arguments");
REXCVAR_TABLE(kTestCvars, flag, count, ratio, mode, word, once, restart, big, hidden, run);

REXCVAR_BOOL(other, flag2, false, "Another scope");
REXCVAR_TABLE(kOtherCvars, flag2);
}  // namespace test_cvars

namespace dupe_cvars {
REXCVAR_BOOL(test, flag, false, "Same name as test_cvars::flag");
REXCVAR_TABLE(kDupeCvars, flag);
REXCVAR_BOOL(other, wrong_scope, false, "Wrong scope for the test table");
REXCVAR_TABLE(kWrongScope, wrong_scope);
}  // namespace dupe_cvars

constinit rex::cvar::Var<int32_t> g_constinit_proof =
    rex::cvar::Var<int32_t>("test.proof", 7, "constant initialized").range(0, 10);
static_assert(rex::cvar::detail::RenderDefault(-42).view() == "-42");
static_assert(rex::cvar::detail::RenderDefault(true).view() == "true");
static_assert(rex::cvar::detail::RenderDefault(0.5).view() == "0.5");
static_assert(rex::cvar::detail::RenderDefault(3.0).view() == "3");
static_assert(rex::cvar::detail::RenderDefault(60.0).view() == "60");

namespace {

struct Fixture {
  std::unique_ptr<rex::detail::CvarRegistryImpl> registry = rex::detail::CreateCvarRegistry();
  rex::detail::CvarRegistryImpl& reg() { return *registry; }
};

}  // namespace

TEST_CASE_METHOD(Fixture, "Var descriptors are constant initialized", "[cvar]") {
  REQUIRE(g_constinit_proof.desc().name == "test.proof");
  REQUIRE(g_constinit_proof.desc().scope == "test");
  REQUIRE(g_constinit_proof.DefaultText() == "7");
  REQUIRE(test_cvars::mode.DefaultText() == "low");
  auto chained_copy =
      rex::cvar::Var<std::string>("test.tmp", "abc", "d").lifecycle(rex::cvar::Lifecycle::InitOnly);
  REQUIRE(chained_copy.DefaultText() == "abc");
  REQUIRE(g_constinit_proof.desc().constraints.min == 0.0);
  REQUIRE(g_constinit_proof.get() == 7);
  REQUIRE(test_cvars::mode.desc().constraints.allowed.size() == 3);
  REQUIRE(test_cvars::mode.desc().constraints.allowed[1] == "medium");
  REQUIRE(test_cvars::kTestCvars.size() == 10);
  REQUIRE(test_cvars::run.desc().type == rex::cvar::Type::Command);
}

TEST_CASE_METHOD(Fixture, "Var parses and renders each type", "[cvar]") {
  REQUIRE(test_cvars::flag.Parse("no"));
  REQUIRE_FALSE(test_cvars::flag.get());
  REQUIRE(test_cvars::flag.Render() == "false");
  REQUIRE_FALSE(test_cvars::flag.Parse("maybe"));
  REQUIRE(test_cvars::count.Parse("77"));
  REQUIRE(test_cvars::count.get() == 77);
  REQUIRE_FALSE(test_cvars::count.Parse("77x"));
  REQUIRE(test_cvars::ratio.Parse("0.25"));
  REQUIRE(test_cvars::ratio.get() == 0.25);
  REQUIRE(test_cvars::mode.Parse("high"));
  REQUIRE(test_cvars::mode.get() == "high");
  REQUIRE_FALSE(test_cvars::mode.Parse(std::string(300, 'x')));
  REQUIRE(test_cvars::big.Parse("18000000000000000000"));
  REQUIRE(test_cvars::big.get() == 18000000000000000000ull);
  REQUIRE(test_cvars::flag.Parse("true"));
  REQUIRE(test_cvars::count.Parse("42"));
  REQUIRE(test_cvars::ratio.Parse("0.5"));
  REQUIRE(test_cvars::mode.Parse("low"));
  REQUIRE(test_cvars::big.Parse("5"));
}

TEST_CASE_METHOD(Fixture, "set on an unregistered var fails NotFound", "[cvar]") {
  auto status = test_cvars::count.set(5);
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::Error::NotFound));
  REQUIRE(test_cvars::count.get() == 42);
}

TEST_CASE_METHOD(Fixture, "Register exposes descriptors, scopes and order", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  auto* d = reg().Find("test.count");
  REQUIRE(d != nullptr);
  REQUIRE(d->desc().type == rex::cvar::Type::Int32);
  REQUIRE(d->DefaultText() == "42");
  REQUIRE(d->desc().lifecycle == rex::cvar::Lifecycle::HotReload);
  REQUIRE(reg().Find("test.once")->desc().lifecycle == rex::cvar::Lifecycle::InitOnly);
  auto all = reg().Enumerate("test");
  REQUIRE(all.size() == 10);
  REQUIRE(all[0]->desc().name == "test.flag");
  REQUIRE(all[9]->desc().name == "test.run");
  REQUIRE(reg().Scopes() == std::vector<std::string_view>{"test"});
  REQUIRE(test_cvars::flag.registry() == &reg());
}

TEST_CASE_METHOD(Fixture, "Register rejects duplicates and bad names and registers nothing",
                 "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  auto twice = reg().Register("test", test_cvars::kTestCvars);
  REQUIRE_FALSE(twice);
  REQUIRE(twice.error().code == static_cast<int>(rex::cvar::Error::DuplicateScope));
  auto dupe = reg().Register("test2", dupe_cvars::kDupeCvars);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::Error::BadName));
  auto wrong = reg().Register("test3", dupe_cvars::kWrongScope);
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().code == static_cast<int>(rex::cvar::Error::BadName));
  REQUIRE(reg().Enumerate().size() == 10);
  REQUIRE(dupe_cvars::wrong_scope.registry() == nullptr);
}

TEST_CASE_METHOD(Fixture, "A duplicate full name in a free scope fails DuplicateName", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  reg().Unregister(test_cvars::kTestCvars);
  REQUIRE(reg().Scopes().empty());
  REQUIRE(reg().Register("test", dupe_cvars::kDupeCvars));
  const rex::cvar::IVar* again[] = {&test_cvars::flag};
  auto status = reg().Register("test", again);
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::Error::DuplicateScope));
  const rex::cvar::IVar* other_scope_dupe[] = {&dupe_cvars::flag};
  reg().Unregister(dupe_cvars::kDupeCvars);
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  auto dupe = reg().Register("test", other_scope_dupe);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::Error::DuplicateScope));
  REQUIRE(reg().Find("test.flag") == &test_cvars::flag);
}

TEST_CASE_METHOD(Fixture, "Unregister frees the scope and re registration works", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  reg().Unregister(test_cvars::kTestCvars);
  REQUIRE(reg().Find("test.flag") == nullptr);
  REQUIRE(reg().Scopes().empty());
  REQUIRE(test_cvars::flag.registry() == nullptr);
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  REQUIRE(reg().Enumerate().size() == 10);
}

TEST_CASE_METHOD(Fixture, "Precedence is Default, Config, Environment, CommandLine, Runtime",
                 "[cvar]") {
  using S = rex::cvar::CvarSource;
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  const S order[] = {S::Default, S::Config, S::Environment, S::CommandLine};
  for (S high : order) {
    for (S low : order) {
      if (low >= high)
        continue;
      reg().Unregister(test_cvars::kTestCvars);
      REQUIRE(reg().Register("test", test_cvars::kTestCvars));
      std::vector<rex::CvarOverride> lows_first = {{"test.count", "10", low},
                                                   {"test.count", "20", high}};
      REQUIRE(reg().ApplyOverrides(lows_first));
      REQUIRE(test_cvars::count.get() == 20);
      REQUIRE(reg().Source("test.count") == high);
      reg().Unregister(test_cvars::kTestCvars);
      REQUIRE(reg().Register("test", test_cvars::kTestCvars));
      std::vector<rex::CvarOverride> highs_first = {{"test.count", "20", high},
                                                    {"test.count", "10", low}};
      REQUIRE(reg().ApplyOverrides(highs_first));
      REQUIRE(test_cvars::count.get() == 20);
    }
  }
  REQUIRE(reg().Set("test.count", "30"));
  REQUIRE(test_cvars::count.get() == 30);
  REQUIRE(reg().Source("test.count") == S::Runtime);
  REQUIRE(reg().Get("test.count").value() == "30");
  REQUIRE_FALSE(reg().Get("test.nope"));
  REQUIRE(reg().Source("test.nope") == S::Default);
  REQUIRE(reg().Set("test.count", "42"));
}

TEST_CASE_METHOD(Fixture, "SetDefault applies only while the source is Default", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  REQUIRE(reg().SetDefault("test.count", "5"));
  REQUIRE(test_cvars::count.get() == 5);
  REQUIRE(reg().Source("test.count") == rex::cvar::CvarSource::Default);
  std::vector<rex::CvarOverride> cfg = {{"test.count", "9", rex::cvar::CvarSource::Config}};
  REQUIRE(reg().ApplyOverrides(cfg));
  REQUIRE(reg().SetDefault("test.count", "6"));
  REQUIRE(test_cvars::count.get() == 9);
  REQUIRE(reg().ResetToDefault("test.count"));
  REQUIRE(test_cvars::count.get() == 6);
  REQUIRE(reg().Source("test.count") == rex::cvar::CvarSource::Default);
  REQUIRE(reg().Find("test.count")->DefaultText() == "42");
  REQUIRE(reg().Get("test.count").value() == "6");
  REQUIRE(test_cvars::count.Parse("42"));
}

TEST_CASE_METHOD(Fixture, "Lock rejects Set and keeps applied values", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  std::vector<rex::CvarOverride> cli = {{"test.count", "12", rex::cvar::CvarSource::CommandLine}};
  REQUIRE(reg().ApplyOverrides(cli));
  std::vector<std::string> locks = {"test.count"};
  REQUIRE(reg().ApplyLocks(locks));
  auto status = reg().Set("test.count", "13");
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::Error::Locked));
  REQUIRE(test_cvars::count.get() == 12);
  std::vector<std::string> bad = {"test.nope"};
  REQUIRE_FALSE(reg().ApplyLocks(bad));
  REQUIRE(test_cvars::count.Parse("42"));
}

TEST_CASE_METHOD(Fixture, "Constraints reject with Invalid and keep the value", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  auto range = reg().Set("test.count", "101");
  REQUIRE_FALSE(range);
  REQUIRE(range.error().code == static_cast<int>(rex::cvar::Error::Invalid));
  REQUIRE(test_cvars::count.get() == 42);
  REQUIRE_FALSE(reg().Set("test.ratio", "1.5"));
  REQUIRE(test_cvars::ratio.get() == 0.5);
  REQUIRE_FALSE(reg().Set("test.mode", "ultra"));
  REQUIRE(test_cvars::mode.get() == "low");
  REQUIRE_FALSE(reg().Set("test.word", "ab"));
  REQUIRE(test_cvars::word.get() == "abc");
  REQUIRE(reg().Set("test.big", "18000000000000000000"));
  REQUIRE(test_cvars::big.get() == 18000000000000000000ull);
  REQUIRE_FALSE(reg().Set("test.count", "abc"));
  REQUIRE(reg().Set("test.big", "5"));
}

TEST_CASE_METHOD(Fixture, "InitOnly and RequiresRestart follow Finalize", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  REQUIRE(reg().Set("test.once", "2"));
  std::vector<rex::CvarOverride> cfg = {{"test.restart", "3", rex::cvar::CvarSource::Config}};
  REQUIRE(reg().ApplyOverrides(cfg));
  REQUIRE(reg().PendingRestart().empty());
  REQUIRE_FALSE(reg().IsFinalized());
  reg().Finalize();
  REQUIRE(reg().IsFinalized());
  auto status = reg().Set("test.once", "3");
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::Error::InitOnly));
  REQUIRE(test_cvars::once.get() == 2);
  REQUIRE(reg().Set("test.restart", "4"));
  REQUIRE(reg().PendingRestart() == std::vector<std::string>{"test.restart"});
  REQUIRE(test_cvars::once.Parse("1"));
  REQUIRE(test_cvars::restart.Parse("1"));
}

TEST_CASE_METHOD(Fixture, "Callbacks fire on Set, apply and reset, and stop with the handle",
                 "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  std::vector<std::string> seen;
  auto handle = reg().OnChange("test.count", [&](std::string_view n, std::string_view v) {
    seen.push_back(std::string(n) + "=" + std::string(v));
  });
  REQUIRE(handle);
  REQUIRE(reg().Set("test.count", "1"));
  std::vector<rex::CvarOverride> cfg = {{"test.count", "2", rex::cvar::CvarSource::Config}};
  REQUIRE(reg().ApplyOverrides(cfg));
  REQUIRE(reg().SetDefault("test.count", "42"));
  REQUIRE(reg().ResetToDefault("test.count"));
  REQUIRE(seen == std::vector<std::string>{"test.count=1", "test.count=2", "test.count=42"});
  handle.value().Reset();
  REQUIRE(reg().Set("test.count", "3"));
  REQUIRE(seen.size() == 3);
  REQUIRE_FALSE(reg().OnChange("test.nope", [](std::string_view, std::string_view) {}));
  REQUIRE(test_cvars::count.Parse("42"));
}

TEST_CASE_METHOD(Fixture, "a callback may call the registry and release its own handle", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  rex::cvar::CallbackHandle self;
  int fired = 0;
  auto handle = reg().OnChange("test.count", [&](std::string_view, std::string_view) {
    ++fired;
    REQUIRE(reg().Get("test.count").value() == "1");
    self.Reset();
  });
  self = std::move(handle.value());
  REQUIRE(reg().Set("test.count", "1"));
  REQUIRE(reg().Set("test.count", "2"));
  REQUIRE(fired == 1);
  REQUIRE(test_cvars::count.Parse("42"));
}

TEST_CASE_METHOD(Fixture, "Commands invoke and refuse Set", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  REQUIRE(reg().Invoke("test.run", "a b"));
  REQUIRE(g_last_command_args == "a b");
  auto on_var = reg().Invoke("test.count", "");
  REQUIRE_FALSE(on_var);
  REQUIRE(on_var.error().code == static_cast<int>(rex::cvar::Error::NotACommand));
  auto set_cmd = reg().Set("test.run", "x");
  REQUIRE_FALSE(set_cmd);
  REQUIRE(set_cmd.error().code == static_cast<int>(rex::cvar::Error::NotAVar));
}

TEST_CASE_METHOD(Fixture, "Serialize writes scoped tables of non default values", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  REQUIRE(reg().Register("other", test_cvars::kOtherCvars));
  REQUIRE(reg().Set("test.count", "7"));
  REQUIRE(reg().Set("test.word", "a \"quoted\" back\\slash"));
  REQUIRE(reg().Set("other.flag2", "true"));
  auto toml = reg().Serialize();
  REQUIRE(toml.find("[test]\n") != std::string::npos);
  REQUIRE(toml.find("count = 7\n") != std::string::npos);
  REQUIRE(toml.find("word = \"a \\\"quoted\\\" back\\\\slash\"\n") != std::string::npos);
  REQUIRE(toml.find("[other]\n") != std::string::npos);
  REQUIRE(toml.find("flag2 = true\n") != std::string::npos);
  REQUIRE(toml.find("flag =") == std::string::npos);
  REQUIRE(toml.find("run") == std::string::npos);
  REQUIRE(test_cvars::count.Parse("42"));
  REQUIRE(test_cvars::word.Parse("abc"));
  REQUIRE(test_cvars::flag2.Parse("false"));
}

TEST_CASE_METHOD(Fixture, "Add extends a reserved scope and pinned scopes survive", "[cvar]") {
  REQUIRE(reg().Register("bind", {}));
  REQUIRE(reg().Scopes() == std::vector<std::string_view>{"bind"});
  auto dynamic = std::make_unique<rex::cvar::Var<std::string>>("bind.console", "Backtick", "d");
  const rex::cvar::IVar* one[] = {dynamic.get()};
  REQUIRE(reg().Add(one));
  REQUIRE(reg().Get("bind.console").value() == "Backtick");
  auto dupe = reg().Add(one);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::Error::DuplicateName));
  const rex::cvar::IVar* unreserved[] = {&test_cvars::flag};
  auto bad = reg().Add(unreserved);
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().code == static_cast<int>(rex::cvar::Error::BadName));
  reg().Unregister(one);
  REQUIRE(reg().Find("bind.console") == nullptr);
  REQUIRE(reg().Scopes() == std::vector<std::string_view>{"bind"});
  REQUIRE(reg().Add(one));
  reg().Unregister(one);
  dynamic.reset(new rex::cvar::Var<std::string>("bind.other", "F1", "d"));
  const rex::cvar::IVar* two[] = {dynamic.get()};
  REQUIRE(reg().Add(two));
  reg().Unregister(two);
}

TEST_CASE_METHOD(Fixture, "Values for a reserved scope wait for Add", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  REQUIRE(reg().Register("bind", {}));
  std::vector<rex::CvarOverride> cfg = {{"bind.console", "F9", rex::cvar::CvarSource::Config},
                                        {"bind.console", "F2", rex::cvar::CvarSource::Default},
                                        {"bind.other", "Home", rex::cvar::CvarSource::CommandLine},
                                        {"nope.thing", "1", rex::cvar::CvarSource::Config}};
  auto unknown = reg().ApplyOverrides(cfg);
  REQUIRE(unknown);
  REQUIRE(unknown->size() == 1);
  REQUIRE(unknown->front().name == "nope.thing");
  reg().Finalize();
  std::vector<std::string> seen;
  auto console = std::make_unique<rex::cvar::Var<std::string>>("bind.console", "Backtick", "d");
  auto other = std::make_unique<rex::cvar::Var<std::string>>("bind.other", "F1", "d");
  const rex::cvar::IVar* both[] = {console.get(), other.get()};
  REQUIRE(reg().Add(both));
  REQUIRE(reg().Get("bind.console").value() == "F9");
  REQUIRE(reg().Source("bind.console") == rex::cvar::CvarSource::Config);
  REQUIRE(console->DefaultText() == "Backtick");
  REQUIRE(reg().ResetToDefault("bind.console"));
  REQUIRE(reg().Get("bind.console").value() == "F2");
  REQUIRE(reg().Get("bind.other").value() == "Home");
  REQUIRE(reg().Source("bind.other") == rex::cvar::CvarSource::CommandLine);
  reg().Unregister(both);
  REQUIRE(reg().Add(both));
  REQUIRE(reg().Source("bind.console") == rex::cvar::CvarSource::Default);
  reg().Unregister(both);
}

TEST_CASE_METHOD(Fixture, "Reset clears registrations and the finalized flag", "[cvar]") {
  REQUIRE(reg().Register("test", test_cvars::kTestCvars));
  reg().Finalize();
  REQUIRE(reg().RegisteredNames().size() == 10);
  reg().Reset();
  REQUIRE(reg().Enumerate().empty());
  REQUIRE_FALSE(reg().IsFinalized());
  REQUIRE(test_cvars::flag.registry() == nullptr);
}

namespace {
REXCVAR_STRING(test6a, kept, "a", "Written to the config file");
REXCVAR_STRING(test6a, transient, "a", "Never written").no_save();
REXCVAR_TABLE(kTest6aCvars, kept, transient);
}  // namespace

TEST_CASE("A no_save var is absent from Serialize", "[cvar]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("test6a", kTest6aCvars));
  REQUIRE(registry->Set("test6a.kept", "b"));
  REQUIRE(registry->Set("test6a.transient", "b"));
  const std::string text = registry->Serialize();
  REQUIRE(text.find("kept") != std::string::npos);
  REQUIRE(text.find("transient") == std::string::npos);
  registry->Unregister(kTest6aCvars);
}
