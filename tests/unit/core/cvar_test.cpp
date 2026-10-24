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

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/logging/category.h>
#include <rex/logging/logger.h>
#include <rex/logging/sink.h>

#include "core/cvar_registry.h"

namespace {

bool AtLeast3(std::string_view v) {
  return v.size() >= 3;
}

std::string g_last_command_args;

void CaptureCommandArgs(std::string_view args) {
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
REXCVAR_COMMAND_ARGS(test, run, CaptureCommandArgs, "Records its arguments");
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
  rex::detail::CvarRegistryImpl& GetRegistry() const { return *registry; }
  const rex::cvar::VarBase& Var(std::string_view name) const {
    const auto found = GetRegistry().Find(name);
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
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  const rex::cvar::VarBase& d = Var("test.count");
  REQUIRE(d.GetDesc().type == rex::cvar::Type::kInt32);
  REQUIRE(d.GetDefaultText() == "42");
  REQUIRE(d.GetDesc().lifecycle == rex::cvar::Lifecycle::kHotReload);
  REQUIRE(Var("test.once").GetDesc().lifecycle == rex::cvar::Lifecycle::kInitOnly);
  auto all = GetRegistry().Enumerate("test");
  REQUIRE(all.size() == 10);
  REQUIRE(all[0]->GetDesc().name == "test.flag");
  REQUIRE(all[9]->GetDesc().name == "test.run");
  REQUIRE(GetRegistry().GetScopes() == std::vector<std::string_view>{"test"});
  REQUIRE(test_cvars::flag.GetRegistry() == &GetRegistry());
}

REX_TEST_CASE_METHOD(Fixture, "Register rejects duplicates and bad names and registers nothing",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  auto twice = GetRegistry().Register("test", test_cvars::kTestCvars);
  REQUIRE_FALSE(twice);
  REQUIRE(twice.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateScope));
  auto dupe = GetRegistry().Register("test2", dupe_cvars::kDupeCvars);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::CvarError::kBadName));
  auto wrong = GetRegistry().Register("test3", dupe_cvars::kWrongScope);
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().code == static_cast<int>(rex::cvar::CvarError::kBadName));
  REQUIRE(GetRegistry().Enumerate().size() == 10);
  REQUIRE(dupe_cvars::wrong_scope.GetRegistry() == nullptr);
}

REX_TEST_CASE_METHOD(Fixture, "A duplicate full name in a free scope fails DuplicateScope",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  GetRegistry().Unregister(test_cvars::kTestCvars);
  REQUIRE(GetRegistry().GetScopes().empty());
  REQUIRE(GetRegistry().Register("test", dupe_cvars::kDupeCvars));
  const rex::cvar::VarBase* again[] = {&test_cvars::flag};
  auto status = GetRegistry().Register("test", again);
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateScope));
  const rex::cvar::VarBase* other_scope_dupe[] = {&dupe_cvars::flag};
  GetRegistry().Unregister(dupe_cvars::kDupeCvars);
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  auto dupe = GetRegistry().Register("test", other_scope_dupe);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateScope));
  REQUIRE(&Var("test.flag") == &test_cvars::flag);
}

REX_TEST_CASE_METHOD(Fixture, "Unregister frees the scope and re registration works",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  GetRegistry().Unregister(test_cvars::kTestCvars);
  REQUIRE_FALSE(GetRegistry().Find("test.flag"));
  REQUIRE(GetRegistry().GetScopes().empty());
  REQUIRE(test_cvars::flag.GetRegistry() == nullptr);
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetRegistry().Enumerate().size() == 10);
}

REX_TEST_CASE_METHOD(Fixture, "Precedence is Default, Config, Environment, CommandLine, Runtime",
                     "[core][cvar]") {
  using S = rex::cvar::CvarSource;
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  const S order[] = {S::kDefault, S::kConfig, S::kEnvironment, S::kCommandLine};
  for (S high : order) {
    for (S low : order) {
      if (low >= high)
        continue;
      GetRegistry().Unregister(test_cvars::kTestCvars);
      REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
      std::vector<rex::CvarOverride> lows_first = {{"test.count", "10", low},
                                                   {"test.count", "20", high}};
      REQUIRE(GetRegistry().ApplyOverrides(lows_first));
      REQUIRE(test_cvars::count.Get() == 20);
      REQUIRE(GetRegistry().GetSource("test.count") == high);
      GetRegistry().Unregister(test_cvars::kTestCvars);
      REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
      std::vector<rex::CvarOverride> highs_first = {{"test.count", "20", high},
                                                    {"test.count", "10", low}};
      REQUIRE(GetRegistry().ApplyOverrides(highs_first));
      REQUIRE(test_cvars::count.Get() == 20);
    }
  }
  REQUIRE(GetRegistry().Set("test.count", "30"));
  REQUIRE(test_cvars::count.Get() == 30);
  REQUIRE(GetRegistry().GetSource("test.count") == S::kRuntime);
  REQUIRE(GetRegistry().Get("test.count").value() == "30");
  REQUIRE_FALSE(GetRegistry().Get("test.nope"));
  REQUIRE(GetRegistry().GetSource("test.nope") == S::kDefault);
  REQUIRE(GetRegistry().Set("test.count", "42"));
}

REX_TEST_CASE_METHOD(Fixture, "SetDefault applies only while the source is Default",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetRegistry().SetDefault("test.count", "5"));
  REQUIRE(test_cvars::count.Get() == 5);
  REQUIRE(GetRegistry().GetSource("test.count") == rex::cvar::CvarSource::kDefault);
  std::vector<rex::CvarOverride> cfg = {{"test.count", "9", rex::cvar::CvarSource::kConfig}};
  REQUIRE(GetRegistry().ApplyOverrides(cfg));
  REQUIRE(GetRegistry().SetDefault("test.count", "6"));
  REQUIRE(test_cvars::count.Get() == 9);
  REQUIRE(GetRegistry().ResetToDefault("test.count"));
  REQUIRE(test_cvars::count.Get() == 6);
  REQUIRE(GetRegistry().GetSource("test.count") == rex::cvar::CvarSource::kDefault);
  REQUIRE(Var("test.count").GetDefaultText() == "42");
  REQUIRE(GetRegistry().Get("test.count").value() == "6");
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "Lock rejects Set and keeps applied values", "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  std::vector<rex::CvarOverride> cli = {{"test.count", "12", rex::cvar::CvarSource::kCommandLine}};
  REQUIRE(GetRegistry().ApplyOverrides(cli));
  std::vector<std::string> locks = {"test.count"};
  REQUIRE(GetRegistry().ApplyLocks(locks));
  auto status = GetRegistry().Set("test.count", "13");
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::CvarError::kLocked));
  REQUIRE(test_cvars::count.Get() == 12);
  std::vector<std::string> bad = {"test.nope"};
  REQUIRE_FALSE(GetRegistry().ApplyLocks(bad));
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "Constraints reject with Invalid and keep the value",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  auto range = GetRegistry().Set("test.count", "101");
  REQUIRE_FALSE(range);
  REQUIRE(range.error().code == static_cast<int>(rex::cvar::CvarError::kInvalid));
  REQUIRE(test_cvars::count.Get() == 42);
  REQUIRE_FALSE(GetRegistry().Set("test.ratio", "1.5"));
  REQUIRE(test_cvars::ratio.Get() == 0.5);
  REQUIRE_FALSE(GetRegistry().Set("test.mode", "ultra"));
  REQUIRE(test_cvars::mode.Get() == "low");
  REQUIRE_FALSE(GetRegistry().Set("test.word", "ab"));
  REQUIRE(test_cvars::word.Get() == "abc");
  REQUIRE(GetRegistry().Set("test.big", "18000000000000000000"));
  REQUIRE(test_cvars::big.Get() == 18000000000000000000ull);
  REQUIRE_FALSE(GetRegistry().Set("test.count", "abc"));
  REQUIRE(GetRegistry().Set("test.big", "5"));
}

REX_TEST_CASE_METHOD(Fixture, "InitOnly and RequiresRestart follow Finalize", "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetRegistry().Set("test.once", "2"));
  std::vector<rex::CvarOverride> cfg = {{"test.restart", "3", rex::cvar::CvarSource::kConfig}};
  REQUIRE(GetRegistry().ApplyOverrides(cfg));
  REQUIRE(GetRegistry().GetPendingRestart().empty());
  REQUIRE_FALSE(GetRegistry().IsFinalized());
  GetRegistry().Finalize();
  REQUIRE(GetRegistry().IsFinalized());
  auto status = GetRegistry().Set("test.once", "3");
  REQUIRE_FALSE(status);
  REQUIRE(status.error().code == static_cast<int>(rex::cvar::CvarError::kInitOnly));
  REQUIRE(test_cvars::once.Get() == 2);
  REQUIRE(GetRegistry().Set("test.restart", "4"));
  REQUIRE(GetRegistry().GetPendingRestart() == std::vector<std::string>{"test.restart"});
  REQUIRE(test_cvars::once.Parse("1"));
  REQUIRE(test_cvars::restart.Parse("1"));
}

REX_TEST_CASE_METHOD(Fixture, "Callbacks fire on Set, apply and reset, and stop with the handle",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  std::vector<std::string> seen;
  auto handle = GetRegistry().OnChange("test.count", [&](std::string_view n, std::string_view v) {
    seen.push_back(std::string(n) + "=" + std::string(v));
  });
  REQUIRE(handle);
  REQUIRE(GetRegistry().Set("test.count", "1"));
  std::vector<rex::CvarOverride> cfg = {{"test.count", "2", rex::cvar::CvarSource::kConfig}};
  REQUIRE(GetRegistry().ApplyOverrides(cfg));
  REQUIRE(GetRegistry().SetDefault("test.count", "42"));
  REQUIRE(GetRegistry().ResetToDefault("test.count"));
  REQUIRE(seen == std::vector<std::string>{"test.count=1", "test.count=2", "test.count=42"});
  handle.value().Reset();
  REQUIRE(GetRegistry().Set("test.count", "3"));
  REQUIRE(seen.size() == 3);
  REQUIRE_FALSE(GetRegistry().OnChange("test.nope", [](std::string_view, std::string_view) {}));
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "a callback may call the registry and release its own handle",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  rex::cvar::CallbackHandle self;
  int fired = 0;
  auto handle = GetRegistry().OnChange("test.count", [&](std::string_view, std::string_view) {
    ++fired;
    REQUIRE(GetRegistry().Get("test.count").value() == "1");
    self.Reset();
  });
  self = std::move(handle.value());
  REQUIRE(GetRegistry().Set("test.count", "1"));
  REQUIRE(GetRegistry().Set("test.count", "2"));
  REQUIRE(fired == 1);
  REQUIRE(test_cvars::count.Parse("42"));
}

REX_TEST_CASE_METHOD(Fixture, "Commands invoke and refuse Set", "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetRegistry().Invoke("test.run", "a b"));
  REQUIRE(g_last_command_args == "a b");
  auto on_var = GetRegistry().Invoke("test.count", "");
  REQUIRE_FALSE(on_var);
  REQUIRE(on_var.error().code == static_cast<int>(rex::cvar::CvarError::kNotACommand));
  auto set_cmd = GetRegistry().Set("test.run", "x");
  REQUIRE_FALSE(set_cmd);
  REQUIRE(set_cmd.error().code == static_cast<int>(rex::cvar::CvarError::kNotAVar));
}

REX_TEST_CASE_METHOD(Fixture, "Serialize escapes control characters so the text parses back",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  const std::string value = "a\r\nb\x01 \b\f\x7f end";
  REQUIRE(GetRegistry().Set("test.word", value));
  const std::string text = GetRegistry().Serialize();
  const toml::table parsed = toml::parse(text);
  CHECK(parsed["test"]["word"].value<std::string>().value_or("") == value);
  REQUIRE(test_cvars::word.Parse("abc"));
}

REX_TEST_CASE_METHOD(Fixture, "Serialize writes scoped tables of non default values",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetRegistry().Register("other", test_cvars::kOtherCvars));
  REQUIRE(GetRegistry().Set("test.count", "7"));
  REQUIRE(GetRegistry().Set("test.word", "a \"quoted\" back\\slash"));
  REQUIRE(GetRegistry().Set("other.flag2", "true"));
  auto toml = GetRegistry().Serialize();
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
  REQUIRE(GetRegistry().Register("bind", {}));
  REQUIRE(GetRegistry().GetScopes() == std::vector<std::string_view>{"bind"});
  auto dynamic = std::make_unique<rex::cvar::Var<std::string>>("bind.console", "Backtick", "d");
  const rex::cvar::VarBase* one[] = {dynamic.get()};
  REQUIRE(GetRegistry().Add(one));
  REQUIRE(GetRegistry().Get("bind.console").value() == "Backtick");
  auto dupe = GetRegistry().Add(one);
  REQUIRE_FALSE(dupe);
  REQUIRE(dupe.error().code == static_cast<int>(rex::cvar::CvarError::kDuplicateName));
  const rex::cvar::VarBase* unreserved[] = {&test_cvars::flag};
  auto bad = GetRegistry().Add(unreserved);
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().code == static_cast<int>(rex::cvar::CvarError::kBadName));
  GetRegistry().Unregister(one);
  REQUIRE_FALSE(GetRegistry().Find("bind.console"));
  REQUIRE(GetRegistry().GetScopes() == std::vector<std::string_view>{"bind"});
  REQUIRE(GetRegistry().Add(one));
  GetRegistry().Unregister(one);
  dynamic = std::make_unique<rex::cvar::Var<std::string>>("bind.other", "F1", "d");
  const rex::cvar::VarBase* two[] = {dynamic.get()};
  REQUIRE(GetRegistry().Add(two));
  GetRegistry().Unregister(two);
}

REX_TEST_CASE_METHOD(Fixture, "Values for a reserved scope wait for Add", "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  REQUIRE(GetRegistry().Register("bind", {}));
  std::vector<rex::CvarOverride> cfg = {{"bind.console", "F9", rex::cvar::CvarSource::kConfig},
                                        {"bind.console", "F2", rex::cvar::CvarSource::kDefault},
                                        {"bind.other", "Home", rex::cvar::CvarSource::kCommandLine},
                                        {"nope.thing", "1", rex::cvar::CvarSource::kConfig}};
  auto unknown = GetRegistry().ApplyOverrides(cfg);
  REQUIRE(unknown);
  REQUIRE(unknown->size() == 1);
  REQUIRE(unknown->front().name == "nope.thing");
  GetRegistry().Finalize();
  std::vector<std::string> seen;
  auto console = std::make_unique<rex::cvar::Var<std::string>>("bind.console", "Backtick", "d");
  auto other = std::make_unique<rex::cvar::Var<std::string>>("bind.other", "F1", "d");
  const rex::cvar::VarBase* both[] = {console.get(), other.get()};
  REQUIRE(GetRegistry().Add(both));
  REQUIRE(GetRegistry().Get("bind.console").value() == "F9");
  REQUIRE(GetRegistry().GetSource("bind.console") == rex::cvar::CvarSource::kConfig);
  REQUIRE(console->GetDefaultText() == "Backtick");
  REQUIRE(GetRegistry().ResetToDefault("bind.console"));
  REQUIRE(GetRegistry().Get("bind.console").value() == "F2");
  REQUIRE(GetRegistry().Get("bind.other").value() == "Home");
  REQUIRE(GetRegistry().GetSource("bind.other") == rex::cvar::CvarSource::kCommandLine);
  GetRegistry().Unregister(both);
  REQUIRE(GetRegistry().Add(both));
  REQUIRE(GetRegistry().GetSource("bind.console") == rex::cvar::CvarSource::kDefault);
  GetRegistry().Unregister(both);
}

REX_TEST_CASE_METHOD(Fixture, "TakePending removes the waiting values except the kept scopes",
                     "[core][cvar]") {
  REQUIRE(GetRegistry().Register("bind", {}));
  REQUIRE(GetRegistry().Register("hid", {}));
  std::vector<rex::CvarOverride> cfg = {{"bind.late", "F9", rex::cvar::CvarSource::kConfig},
                                        {"hid.typo", "1", rex::cvar::CvarSource::kConfig}};
  REQUIRE(GetRegistry().ApplyOverrides(cfg));
  const std::array<std::string_view, 1> keep = {"bind"};
  auto taken = GetRegistry().TakePending(keep);
  REQUIRE(taken.size() == 1);
  CHECK(taken.front().name == "hid.typo");
  CHECK(GetRegistry().TakePending(keep).empty());
  taken = GetRegistry().TakePending();
  REQUIRE(taken.size() == 1);
  CHECK(taken.front().name == "bind.late");
}

REX_TEST_CASE_METHOD(Fixture, "Reset clears registrations and the finalized flag", "[core][cvar]") {
  REQUIRE(GetRegistry().Register("test", test_cvars::kTestCvars));
  GetRegistry().Finalize();
  REQUIRE(GetRegistry().GetRegisteredNames().size() == 10);
  GetRegistry().Reset();
  REQUIRE(GetRegistry().Enumerate().empty());
  REQUIRE_FALSE(GetRegistry().IsFinalized());
  REQUIRE(test_cvars::flag.GetRegistry() == nullptr);
}

namespace {
REXCVAR_STRING(test_save, kept, "a", "Written to the config file");
REXCVAR_STRING(test_save, transient, "a", "Never written").SetNoSave();
REXCVAR_TABLE(kSaveTestCvars, kept, transient);
}  // namespace

REX_TEST_CASE("A no_save var is absent from Serialize", "[core][cvar]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("test_save", kSaveTestCvars));
  REQUIRE(registry->Set("test_save.kept", "b"));
  REQUIRE(registry->Set("test_save.transient", "b"));
  const std::string text = registry->Serialize();
  REQUIRE(text.find("kept") != std::string::npos);
  REQUIRE(text.find("transient") == std::string::npos);
  registry->Unregister(kSaveTestCvars);
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

#ifndef NDEBUG
namespace report_cvars {
REXCVAR_INT32(report, unregistered, 5, "A cvar no table holds");
REXCVAR_INT32(report, silenced, 5, "A second cvar no table holds");
}  // namespace report_cvars

namespace {

class LineSink final : public rex::log::ISink {
 public:
  void Write(const rex::log::Message& message) override {
    levels.push_back(message.level);
    lines.emplace_back(message.text);
  }
  void Flush() override {}

  std::vector<rex::log::Level> levels;
  std::vector<std::string> lines;
};

struct InitScope {
  InitScope() {
    rex::Shutdown();
    auto started = rex::Init({});
    REQUIRE(started);
    logger = (*started)->Get<rex::ILogger>();
    REQUIRE(logger);
  }
  ~InitScope() { rex::Shutdown(); }

  rex::ILogger* logger = nullptr;
};

}  // namespace

REX_TEST_CASE("An unregistered cvar read logs one error once the modules connect", "[core][cvar]") {
  rex::Shutdown();
  CHECK(report_cvars::unregistered.Get() == 5);
  InitScope scope;
  LineSink sink;
  scope.logger->AddSink(sink);
  CHECK(report_cvars::unregistered.Get() == 5);
  CHECK(report_cvars::unregistered.Get() == 5);
  scope.logger->RemoveSink(sink);
  REQUIRE(sink.lines.size() == 1);
  CHECK(sink.levels[0] == rex::log::Level::kError);
  CHECK(sink.lines[0].find("report.unregistered") != std::string::npos);
}

REX_TEST_CASE("An unregistered cvar read logs nothing while the report is off", "[core][cvar]") {
  InitScope scope;
  LineSink sink;
  scope.logger->AddSink(sink);
  rex::detail::SetUnregisteredReadReport(false);
  CHECK(report_cvars::silenced.Get() == 5);
  scope.logger->RemoveSink(sink);
  CHECK(sink.lines.empty());
}
#endif