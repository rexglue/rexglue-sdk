/**
 * @file        tests/unit/core/cvar_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/category.h>

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
REXCVAR_INT32(test, count, 42, "An int32").SetRange(1, 100);
REXCVAR_DOUBLE(test, ratio, 0.5, "A double").SetRange(0.0, 1.0);
REXCVAR_STRING(test, mode, "low", "A string").SetAllowed(REXCVAR_ALLOWED("low", "medium", "high"));
REXCVAR_STRING(test, word, "abc", "Validated").SetValidator(&AtLeast3);
REXCVAR_INT32(test, once, 1, "Init only").SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_INT32(test, restart, 1, "Requires restart")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_UINT64(test, big, 5, "A uint64").SetRange(0, 1.8e19);
REXCVAR_BOOL(test, hidden, false, "Debug only").SetDebugOnly();
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

constinit rex::cvar::Var<i32> g_constinit_proof =
    rex::cvar::Var<i32>("test.proof", 7, "constant initialized").SetRange(0, 10);
static_assert(rex::cvar::detail::RenderDefault(-42).GetView() == "-42");
static_assert(rex::cvar::detail::RenderDefault(true).GetView() == "true");
static_assert(rex::cvar::detail::RenderDefault(0.5).GetView() == "0.5");
static_assert(rex::cvar::detail::RenderDefault(3.0).GetView() == "3");
static_assert(rex::cvar::detail::RenderDefault(60.0).GetView() == "60");

namespace {

struct Fixture {
  std::unique_ptr<rex::detail::CvarRegistryImpl> registry = rex::detail::CreateCvarRegistry();
  rex::detail::CvarRegistryImpl& GetReg() const { return *registry; }
  const rex::cvar::VarBase& Var(std::string_view name) const {
    const auto found = GetReg().Find(name);
    if (!found) {
      throw std::logic_error("cvar not registered");
    }
    return found->get();
  }
};

}  // namespace

REX_TEST_CASE("A cvar name is a lowercase scope and leaf joined by 1 dot", "[core][cvar]") {
  CHECK(rex::cvar::IsName("gpu.backend"));
  CHECK(rex::cvar::IsName("ui.window_2"));
  CHECK_FALSE(rex::cvar::IsName("backend"));
  CHECK_FALSE(rex::cvar::IsName("gpu.xenos.backend"));
  CHECK_FALSE(rex::cvar::IsName("Gpu.backend"));
  CHECK_FALSE(rex::cvar::IsName("gpu.2fast"));
  CHECK_FALSE(rex::cvar::IsName(".backend"));
  CHECK_FALSE(rex::cvar::IsName("gpu."));
  CHECK(rex::cvar::IsNamePart("a_1"));
  CHECK_FALSE(rex::cvar::IsNamePart("_a"));
  CHECK_FALSE(rex::cvar::IsNamePart(""));
}

REX_TEST_CASE_METHOD(Fixture, "Var descriptors are constant initialized", "[core][cvar]") {
  REQUIRE(g_constinit_proof.GetDesc().name == "test.proof");
  REQUIRE(g_constinit_proof.GetDesc().scope == "test");
  REQUIRE(g_constinit_proof.GetDefaultText() == "7");
  REQUIRE(test_cvars::mode.GetDefaultText() == "low");
  auto chained_copy = rex::cvar::Var<std::string>("test.tmp", "abc", "d")
                          .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
  REQUIRE(chained_copy.GetDefaultText() == "abc");
  REQUIRE(g_constinit_proof.GetDesc().constraints.min == 0.0);
  REQUIRE(g_constinit_proof.Get() == 7);
  REQUIRE(test_cvars::mode.GetDesc().constraints.allowed.size() == 3);
  REQUIRE(test_cvars::mode.GetDesc().constraints.allowed[1] == "medium");
  REQUIRE(test_cvars::kTestCvars.size() == 10);
  REQUIRE(test_cvars::run.GetDesc().type == rex::cvar::Type::kCommand);
}

REX_TEST_CASE_METHOD(Fixture, "Var builders chain Range, Lifecycle and NoSave", "[core][cvar]") {
  const auto chained = rex::cvar::Var<i32>("test.chain", 3, "chained")
                           .SetRange(0, 5)
                           .SetLifecycle(rex::cvar::Lifecycle::kInitOnly)
                           .SetNoSave();
  REQUIRE(chained.GetDesc().constraints.max == 5.0);
  REQUIRE(chained.GetDesc().lifecycle == rex::cvar::Lifecycle::kInitOnly);
  REQUIRE(chained.GetDesc().no_save);
}

REX_TEST_CASE_METHOD(Fixture, "Var parses and renders each type", "[core][cvar]") {
  REQUIRE(test_cvars::flag.Parse("no"));
  REQUIRE_FALSE(test_cvars::flag.Get());
  REQUIRE(test_cvars::flag.Render() == "false");
  REQUIRE_FALSE(test_cvars::flag.Parse("maybe"));
  REQUIRE(test_cvars::count.Parse("77"));
  REQUIRE(test_cvars::count.Get() == 77);
  REQUIRE_FALSE(test_cvars::count.Parse("77x"));
  REQUIRE(test_cvars::ratio.Parse("0.25"));
  REQUIRE(test_cvars::ratio.Get() == 0.25);
  REQUIRE(test_cvars::mode.Parse("high"));
  REQUIRE(test_cvars::mode.Get() == "high");
  REQUIRE_FALSE(test_cvars::mode.Parse(std::string(300, 'x')));
  REQUIRE(test_cvars::big.Parse("18000000000000000000"));
  REQUIRE(test_cvars::big.Get() == 18000000000000000000ull);
  REQUIRE(test_cvars::flag.Parse("true"));
  REQUIRE(test_cvars::count.Parse("42"));
  REQUIRE(test_cvars::ratio.Parse("0.5"));
  REQUIRE(test_cvars::mode.Parse("low"));
  REQUIRE(test_cvars::big.Parse("5"));
}

REX_TEST_CASE_METHOD(Fixture, "set on an unregistered var fails NotFound", "[core][cvar]") {
  auto status = test_cvars::count.Set(5);
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::CvarError::kNotFound));
  REQUIRE(test_cvars::count.Get() == 42);
}

REX_TEST_CASE_METHOD(Fixture, "Register exposes descriptors, scopes and order", "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  const rex::cvar::VarBase& d = Var("test.count");
  REQUIRE(d.GetDesc().type == rex::cvar::Type::kInt32);
  REQUIRE(d.GetDefaultText() == "42");
  REQUIRE(d.GetDesc().lifecycle == rex::cvar::Lifecycle::kHotReload);
  REQUIRE(Var("test.once").GetDesc().lifecycle == rex::cvar::Lifecycle::kInitOnly);
  auto all = GetReg().Enumerate("test");
  REQUIRE(all.size() == 10);
  REQUIRE(all[0]->GetDesc().name == "test.flag");
  REQUIRE(all[9]->GetDesc().name == "test.run");
  REQUIRE(GetReg().GetScopes() == std::vector<std::string_view>{"test"});
  REQUIRE(test_cvars::flag.GetRegistry() == &GetReg());
}

REX_TEST_CASE_METHOD(Fixture, "Register rejects duplicates and bad names and registers nothing",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  auto twice = GetReg().Register("test", test_cvars::kTestCvars);
  REQUIRE_FALSE(twice);
  REQUIRE(twice.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateScope));
  auto dupe = GetReg().Register("test2", dupe_cvars::kDupeCvars);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::CvarError::kBadName));
  auto wrong = GetReg().Register("test3", dupe_cvars::kWrongScope);
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().code == static_cast<int>(rex::cvar::CvarError::kBadName));
  REQUIRE(GetReg().Enumerate().size() == 10);
  REQUIRE(dupe_cvars::wrong_scope.GetRegistry() == nullptr);
}

REX_TEST_CASE_METHOD(Fixture, "A duplicate full name in a free scope fails DuplicateScope",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  GetReg().Unregister(test_cvars::kTestCvars);
  REQUIRE(GetReg().GetScopes().empty());
  REQUIRE(GetReg().Register("test", dupe_cvars::kDupeCvars));
  const rex::cvar::VarBase* again[] = {&test_cvars::flag};
  auto status = GetReg().Register("test", again);
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateScope));
  const rex::cvar::VarBase* other_scope_dupe[] = {&dupe_cvars::flag};
  GetReg().Unregister(dupe_cvars::kDupeCvars);
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  auto dupe = GetReg().Register("test", other_scope_dupe);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateScope));
  REQUIRE(&Var("test.flag") == &test_cvars::flag);
}

REX_TEST_CASE_METHOD(Fixture, "Unregister frees the scope and re registration works",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  GetReg().Unregister(test_cvars::kTestCvars);
  REQUIRE_FALSE(GetReg().Find("test.flag"));
  REQUIRE(GetReg().GetScopes().empty());
  REQUIRE(test_cvars::flag.GetRegistry() == nullptr);
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetReg().Enumerate().size() == 10);
}

REX_TEST_CASE_METHOD(Fixture, "Precedence is Default, Config, Environment, CommandLine, Runtime",
                     "[core][cvar]") {
  using S = rex::cvar::CvarSource;
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  const S order[] = {S::kDefault, S::kConfig, S::kEnvironment, S::kCommandLine};
  for (S high : order) {
    for (S low : order) {
      if (low >= high)
        continue;
      GetReg().Unregister(test_cvars::kTestCvars);
      REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
      std::vector<rex::CvarOverride> lows_first = {{"test.count", "10", low},
                                                   {"test.count", "20", high}};
      REQUIRE(GetReg().ApplyOverrides(lows_first));
      REQUIRE(test_cvars::count.Get() == 20);
      REQUIRE(GetReg().Source("test.count") == high);
      GetReg().Unregister(test_cvars::kTestCvars);
      REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
      std::vector<rex::CvarOverride> highs_first = {{"test.count", "20", high},
                                                    {"test.count", "10", low}};
      REQUIRE(GetReg().ApplyOverrides(highs_first));
      REQUIRE(test_cvars::count.Get() == 20);
    }
  }
  REQUIRE(GetReg().Set("test.count", "30"));
  REQUIRE(test_cvars::count.Get() == 30);
  REQUIRE(GetReg().Source("test.count") == S::kRuntime);
  REQUIRE(GetReg().Get("test.count").value() == "30");
  REQUIRE_FALSE(GetReg().Get("test.nope"));
  REQUIRE(GetReg().Source("test.nope") == S::kDefault);
  REQUIRE(GetReg().Set("test.count", "42"));
}

REX_TEST_CASE_METHOD(Fixture, "SetDefault applies only while the source is Default",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetReg().SetDefault("test.count", "5"));
  REQUIRE(test_cvars::count.Get() == 5);
  REQUIRE(GetReg().Source("test.count") == rex::cvar::CvarSource::kDefault);
  std::vector<rex::CvarOverride> cfg = {{"test.count", "9", rex::cvar::CvarSource::kConfig}};
  REQUIRE(GetReg().ApplyOverrides(cfg));
  REQUIRE(GetReg().SetDefault("test.count", "6"));
  REQUIRE(test_cvars::count.Get() == 9);
  REQUIRE(GetReg().ResetToDefault("test.count"));
  REQUIRE(test_cvars::count.Get() == 6);
  REQUIRE(GetReg().Source("test.count") == rex::cvar::CvarSource::kDefault);
  REQUIRE(Var("test.count").GetDefaultText() == "42");
  REQUIRE(GetReg().Get("test.count").value() == "6");
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "Lock rejects Set and keeps applied values", "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  std::vector<rex::CvarOverride> cli = {{"test.count", "12", rex::cvar::CvarSource::kCommandLine}};
  REQUIRE(GetReg().ApplyOverrides(cli));
  std::vector<std::string> locks = {"test.count"};
  REQUIRE(GetReg().ApplyLocks(locks));
  auto status = GetReg().Set("test.count", "13");
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::CvarError::kLocked));
  REQUIRE(test_cvars::count.Get() == 12);
  std::vector<std::string> bad = {"test.nope"};
  REQUIRE_FALSE(GetReg().ApplyLocks(bad));
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "Constraints reject with Invalid and keep the value",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  auto range = GetReg().Set("test.count", "101");
  REQUIRE_FALSE(range);
  REQUIRE(range.error().code == static_cast<int>(rex::cvar::CvarError::kInvalid));
  REQUIRE(test_cvars::count.Get() == 42);
  REQUIRE_FALSE(GetReg().Set("test.ratio", "1.5"));
  REQUIRE(test_cvars::ratio.Get() == 0.5);
  REQUIRE_FALSE(GetReg().Set("test.mode", "ultra"));
  REQUIRE(test_cvars::mode.Get() == "low");
  REQUIRE_FALSE(GetReg().Set("test.word", "ab"));
  REQUIRE(test_cvars::word.Get() == "abc");
  REQUIRE(GetReg().Set("test.big", "18000000000000000000"));
  REQUIRE(test_cvars::big.Get() == 18000000000000000000ull);
  REQUIRE_FALSE(GetReg().Set("test.count", "abc"));
  REQUIRE(GetReg().Set("test.big", "5"));
}

REX_TEST_CASE_METHOD(Fixture, "InitOnly and RequiresRestart follow Finalize", "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetReg().Set("test.once", "2"));
  std::vector<rex::CvarOverride> cfg = {{"test.restart", "3", rex::cvar::CvarSource::kConfig}};
  REQUIRE(GetReg().ApplyOverrides(cfg));
  REQUIRE(GetReg().GetPendingRestart().empty());
  REQUIRE_FALSE(GetReg().IsFinalized());
  GetReg().Finalize();
  REQUIRE(GetReg().IsFinalized());
  auto status = GetReg().Set("test.once", "3");
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::CvarError::kInitOnly));
  REQUIRE(test_cvars::once.Get() == 2);
  REQUIRE(GetReg().Set("test.restart", "4"));
  REQUIRE(GetReg().GetPendingRestart() == std::vector<std::string>{"test.restart"});
  REQUIRE(test_cvars::once.Parse("1"));
  REQUIRE(test_cvars::restart.Parse("1"));
}

REX_TEST_CASE_METHOD(Fixture, "Callbacks fire on Set, apply and reset, and stop with the handle",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  std::vector<std::string> seen;
  auto handle = GetReg().OnChange("test.count", [&](std::string_view n, std::string_view v) {
    seen.push_back(std::string(n) + "=" + std::string(v));
  });
  REQUIRE(handle);
  REQUIRE(GetReg().Set("test.count", "1"));
  std::vector<rex::CvarOverride> cfg = {{"test.count", "2", rex::cvar::CvarSource::kConfig}};
  REQUIRE(GetReg().ApplyOverrides(cfg));
  REQUIRE(GetReg().SetDefault("test.count", "42"));
  REQUIRE(GetReg().ResetToDefault("test.count"));
  REQUIRE(seen == std::vector<std::string>{"test.count=1", "test.count=2", "test.count=42"});
  handle.value().Reset();
  REQUIRE(GetReg().Set("test.count", "3"));
  REQUIRE(seen.size() == 3);
  REQUIRE_FALSE(GetReg().OnChange("test.nope", [](std::string_view, std::string_view) {}));
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "a callback may call the registry and release its own handle",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  rex::cvar::CallbackHandle self;
  int fired = 0;
  auto handle = GetReg().OnChange("test.count", [&](std::string_view, std::string_view) {
    ++fired;
    REQUIRE(GetReg().Get("test.count").value() == "1");
    self.Reset();
  });
  self = std::move(handle.value());
  REQUIRE(GetReg().Set("test.count", "1"));
  REQUIRE(GetReg().Set("test.count", "2"));
  REQUIRE(fired == 1);
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "Commands invoke and refuse Set", "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetReg().Invoke("test.run", "a b"));
  REQUIRE(g_last_command_args == "a b");
  auto on_var = GetReg().Invoke("test.count", "");
  REQUIRE_FALSE(on_var);
  REQUIRE(on_var.error().code == static_cast<int>(rex::cvar::CvarError::kNotACommand));
  auto set_cmd = GetReg().Set("test.run", "x");
  REQUIRE_FALSE(set_cmd);
  REQUIRE(set_cmd.error().code == static_cast<int>(rex::cvar::CvarError::kNotAVar));
}

REX_TEST_CASE_METHOD(Fixture, "Serialize writes scoped tables of non default values",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetReg().Register("other", test_cvars::kOtherCvars));
  REQUIRE(GetReg().Set("test.count", "7"));
  REQUIRE(GetReg().Set("test.word", "a \"quoted\" back\\slash"));
  REQUIRE(GetReg().Set("other.flag2", "true"));
  auto toml = GetReg().Serialize();
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

REX_TEST_CASE_METHOD(Fixture, "Add extends a reserved scope and pinned scopes survive",
                     "[core][cvar]") {
  REQUIRE(GetReg().Register("bind", {}));
  REQUIRE(GetReg().GetScopes() == std::vector<std::string_view>{"bind"});
  auto dynamic = std::make_unique<rex::cvar::Var<std::string>>("bind.console", "Backtick", "d");
  const rex::cvar::VarBase* one[] = {dynamic.get()};
  REQUIRE(GetReg().Add(one));
  REQUIRE(GetReg().Get("bind.console").value() == "Backtick");
  auto dupe = GetReg().Add(one);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateName));
  const rex::cvar::VarBase* unreserved[] = {&test_cvars::flag};
  auto bad = GetReg().Add(unreserved);
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().code == static_cast<int>(rex::cvar::CvarError::kBadName));
  GetReg().Unregister(one);
  REQUIRE_FALSE(GetReg().Find("bind.console"));
  REQUIRE(GetReg().GetScopes() == std::vector<std::string_view>{"bind"});
  REQUIRE(GetReg().Add(one));
  GetReg().Unregister(one);
  dynamic = std::make_unique<rex::cvar::Var<std::string>>("bind.other", "F1", "d");
  const rex::cvar::VarBase* two[] = {dynamic.get()};
  REQUIRE(GetReg().Add(two));
  GetReg().Unregister(two);
}

REX_TEST_CASE_METHOD(Fixture, "Values for a reserved scope wait for Add", "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetReg().Register("bind", {}));
  std::vector<rex::CvarOverride> cfg = {{"bind.console", "F9", rex::cvar::CvarSource::kConfig},
                                        {"bind.console", "F2", rex::cvar::CvarSource::kDefault},
                                        {"bind.other", "Home", rex::cvar::CvarSource::kCommandLine},
                                        {"nope.thing", "1", rex::cvar::CvarSource::kConfig}};
  auto unknown = GetReg().ApplyOverrides(cfg);
  REQUIRE(unknown);
  REQUIRE(unknown->size() == 1);
  REQUIRE(unknown->front().name == "nope.thing");
  GetReg().Finalize();
  std::vector<std::string> seen;
  auto console = std::make_unique<rex::cvar::Var<std::string>>("bind.console", "Backtick", "d");
  auto other = std::make_unique<rex::cvar::Var<std::string>>("bind.other", "F1", "d");
  const rex::cvar::VarBase* both[] = {console.get(), other.get()};
  REQUIRE(GetReg().Add(both));
  REQUIRE(GetReg().Get("bind.console").value() == "F9");
  REQUIRE(GetReg().Source("bind.console") == rex::cvar::CvarSource::kConfig);
  REQUIRE(console->GetDefaultText() == "Backtick");
  REQUIRE(GetReg().ResetToDefault("bind.console"));
  REQUIRE(GetReg().Get("bind.console").value() == "F2");
  REQUIRE(GetReg().Get("bind.other").value() == "Home");
  REQUIRE(GetReg().Source("bind.other") == rex::cvar::CvarSource::kCommandLine);
  GetReg().Unregister(both);
  REQUIRE(GetReg().Add(both));
  REQUIRE(GetReg().Source("bind.console") == rex::cvar::CvarSource::kDefault);
  GetReg().Unregister(both);
}

REX_TEST_CASE_METHOD(Fixture, "Reset clears registrations and the finalized flag", "[core][cvar]") {
  REQUIRE(GetReg().Register("test", test_cvars::kTestCvars));
  GetReg().Finalize();
  REQUIRE(GetReg().GetRegisteredNames().size() == 10);
  GetReg().Reset();
  REQUIRE(GetReg().Enumerate().empty());
  REQUIRE_FALSE(GetReg().IsFinalized());
  REQUIRE(test_cvars::flag.GetRegistry() == nullptr);
}

namespace {
REXCVAR_STRING(test6a, kept, "a", "Written to the config file");
REXCVAR_STRING(test6a, transient, "a", "Never written").SetNoSave();
REXCVAR_TABLE(kTest6aCvars, kept, transient);
}  // namespace

REX_TEST_CASE("A no_save var is absent from Serialize", "[core][cvar]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("test6a", kTest6aCvars));
  REQUIRE(registry->Set("test6a.kept", "b"));
  REQUIRE(registry->Set("test6a.transient", "b"));
  const std::string text = registry->Serialize();
  REQUIRE(text.find("kept") != std::string::npos);
  REQUIRE(text.find("transient") == std::string::npos);
  registry->Unregister(kTest6aCvars);
}

REX_TEST_CASE("cvar and log category descriptors construct without throwing", "[core][cvar]") {
  using namespace std::string_view_literals;
  STATIC_REQUIRE(std::is_nothrow_copy_constructible_v<rex::cvar::Var<bool>>);
  STATIC_REQUIRE(noexcept(rex::cvar::Var<bool>("a.b"sv, true, "d"sv)));
  STATIC_REQUIRE(noexcept(rex::cvar::Var<bool>("a.b"sv, true, "d"sv).SetRange(0, 1)));
  STATIC_REQUIRE(noexcept(rex::log::Category("a"sv, "d"sv)));
}

namespace {

rex::cvar::CvarDesc IntegerDesc(rex::cvar::Type type) {
  rex::cvar::CvarDesc desc;
  desc.name = "test.value";
  desc.type = type;
  return desc;
}

}  // namespace

REX_TEST_CASE("an Int64 cvar beyond 32 bits survives an edit round trip", "[core][cvar]") {
  const std::string text = std::to_string(i64{1} << 40);
  CHECK(rex::cvar::ClampIntegerCvarText(IntegerDesc(rex::cvar::Type::kInt64), text) == text);
}

REX_TEST_CASE("a UInt64 cvar beyond 32 bits survives an edit round trip", "[core][cvar]") {
  const std::string text = std::to_string(u64{1} << 40);
  CHECK(rex::cvar::ClampIntegerCvarText(IntegerDesc(rex::cvar::Type::kUInt64), text) == text);
}

REX_TEST_CASE("an integer cvar clamps to its constraints", "[core][cvar]") {
  rex::cvar::CvarDesc desc = IntegerDesc(rex::cvar::Type::kInt64);
  desc.constraints.min = 2.0;
  desc.constraints.max = 8.0;
  CHECK(rex::cvar::ClampIntegerCvarText(desc, "1") == "2");
  CHECK(rex::cvar::ClampIntegerCvarText(desc, "9") == "8");
  CHECK(rex::cvar::ClampIntegerCvarText(desc, "5") == "5");
}
