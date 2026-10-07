/**
 * @file        tests/unit/ui/console_model_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/sink.h>
#include "ui/sdl3/console/model.h"

#include "core/cvar_registry.h"

namespace {

std::string g_console_args;

void RecordConsoleArgs(std::string_view args) {
  g_console_args = std::string(args);
}

}  // namespace

namespace console_test_cvars {
REXCVAR_STRING(alpha, mode, "low", "Quality mode").allowed(REXCVAR_ALLOWED("low", "high"));
REXCVAR_COMMAND_ARGS(alpha, run, RecordConsoleArgs, "Records its arguments");
REXCVAR_TABLE(kAlphaCvars, mode, run);
REXCVAR_INT32(zeta, count, 4, "A counted value").range(1, 8);
REXCVAR_TABLE(kZetaCvars, count);
}  // namespace console_test_cvars

namespace {

using rex::log::Level;
using rex::ui::ConsoleModel;

struct Harness {
  Harness() : registry(rex::detail::CreateCvarRegistry()) {
    REQUIRE(registry->Register("alpha", console_test_cvars::kAlphaCvars));
    REQUIRE(registry->Register("zeta", console_test_cvars::kZetaCvars));
  }

  ~Harness() {
    CHECK(registry->ResetToDefault("alpha.mode"));
    CHECK(registry->ResetToDefault("zeta.count"));
    registry->Unregister(console_test_cvars::kAlphaCvars);
    registry->Unregister(console_test_cvars::kZetaCvars);
  }

  void Write(Level level, std::string_view category, std::string_view text) {
    rex::log::Record record;
    record.level = level;
    record.category = category;
    record.text = text;
    record.sequence = ++sequence;
    capture.Write(record);
  }

  std::unique_ptr<rex::detail::CvarRegistryImpl> registry;
  rex::log::CaptureSink capture;
  rex::u64 sequence = 0;
};

class TempFolder {
 public:
  TempFolder() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("rex_console_test_" + std::to_string(stamp));
    std::filesystem::create_directories(path_);
  }
  ~TempFolder() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

const std::string& Last(const ConsoleModel& model) {
  return model.lines().back().text;
}

std::vector<std::string> VisibleText(const ConsoleModel& model) {
  std::vector<std::string> out;
  for (const rex::ui::ConsoleLine* line : model.Visible()) {
    out.push_back(line->text);
  }
  return out;
}

}  // namespace

TEST_CASE_METHOD(Harness, "Execute sets, prints, reports, runs and clears", "[console]") {
  ConsoleModel model(*registry, capture, {});
  model.Execute("zeta.count 6");
  REQUIRE(registry->Get("zeta.count").value() == "6");
  REQUIRE(Last(model) == "zeta.count = 6");
  model.Execute("  zeta.count  ");
  REQUIRE(Last(model) == "zeta.count = 6");
  model.Execute("zeta.nope");
  REQUIRE(Last(model) == "unknown cvar zeta.nope");
  model.Execute("zeta.count 99");
  REQUIRE(model.lines().back().level == Level::kWarn);
  REQUIRE(registry->Get("zeta.count").value() == "6");
  model.Execute("alpha.run first second");
  REQUIRE(g_console_args == "first second");
  model.Execute("clear");
  REQUIRE(model.lines().empty());
}

TEST_CASE_METHOD(Harness, "Complete sorts by scope then name with built ins first", "[console]") {
  ConsoleModel model(*registry, capture, {});
  REQUIRE(model.Complete("").empty());
  REQUIRE(model.Complete("alpha.mode x").empty());

  const auto alpha = model.Complete("alpha.");
  REQUIRE(alpha.size() == 2);
  REQUIRE(alpha[0].name == "alpha.mode");
  REQUIRE(alpha[0].scope == "alpha");
  REQUIRE(alpha[0].description == "Quality mode");
  REQUIRE_FALSE(alpha[0].command);
  REQUIRE(alpha[1].name == "alpha.run");
  REQUIRE(alpha[1].command);

  const auto help = model.Complete("he");
  REQUIRE(help.size() == 1);
  REQUIRE(help[0].name == "help");
  REQUIRE(help[0].scope.empty());

  std::deque<std::string> names;
  std::deque<rex::cvar::Var<bool>> vars;
  std::vector<const rex::cvar::IVar*> table;
  for (int index = 0; index < 70; ++index) {
    names.push_back(std::format("many.v{:02}", index));
    vars.emplace_back(names.back(), false, "Generated");
    table.push_back(&vars.back());
  }
  REQUIRE(registry->Register("many", table));
  const auto many = model.Complete("m");
  REQUIRE(many.size() == ConsoleModel::kMaxCompletions);
  REQUIRE(many.front().name == "many.v00");
  registry->Unregister(table);
}

TEST_CASE_METHOD(Harness, "HelpLine describes a cvar, a command and nothing else", "[console]") {
  ConsoleModel model(*registry, capture, {});
  REQUIRE(model.HelpLine("alpha.mode") ==
          "alpha.mode string = low, default low, allowed low high. Quality mode");
  REQUIRE(model.HelpLine("zeta.count 3") ==
          "zeta.count i32 = 4, default 4, range 1 to 8. A counted value");
  REQUIRE(model.HelpLine("alpha.run") == "alpha.run command. Records its arguments");
  REQUIRE(model.HelpLine("clear").starts_with("clear. "));
  REQUIRE(model.HelpLine("zeta.nope").empty());
}

TEST_CASE_METHOD(Harness, "History walks, skips repeats, caps and round trips", "[console]") {
  TempFolder temp;
  const auto path = temp.Path() / "history.txt";
  std::string newest;
  {
    ConsoleModel model(*registry, capture, path);
    model.Execute("zeta.count 2");
    model.Execute("zeta.count 2");
    model.Execute("zeta.count 3");
    REQUIRE(model.history().size() == 2);
    REQUIRE(model.HistoryPrevious() == "zeta.count 3");
    REQUIRE(model.HistoryPrevious() == "zeta.count 2");
    REQUIRE(model.HistoryPrevious() == "zeta.count 2");
    REQUIRE(model.HistoryNext() == "zeta.count 3");
    REQUIRE(model.HistoryNext().empty());
    for (int index = 0; index < 300; ++index) {
      model.Execute(std::format("zeta.count {}", 1 + index % 8));
    }
    REQUIRE(model.history().size() == ConsoleModel::kMaxHistory);
    newest = model.history().back();
  }
  ConsoleModel reloaded(*registry, capture, path);
  REQUIRE(reloaded.history().size() == ConsoleModel::kMaxHistory);
  REQUIRE(reloaded.history().back() == newest);
  REQUIRE(reloaded.HistoryPrevious() == newest);
}

TEST_CASE_METHOD(Harness, "Output interleaves by sequence, caps and filters", "[console]") {
  ConsoleModel model(*registry, capture, {});
  Write(Level::kInfo, "kernel", "first");
  model.Execute("zeta.count");
  Write(Level::kDebug, "gpu", "second\nthird");
  model.Poll();
  REQUIRE(VisibleText(model) ==
          std::vector<std::string>{"first", "> zeta.count", "zeta.count = 4", "second", "third"});
  REQUIRE(model.categories().contains("kernel"));
  REQUIRE(model.categories().contains("gpu"));

  model.set_min_level(Level::kInfo);
  REQUIRE(VisibleText(model) ==
          std::vector<std::string>{"first", "> zeta.count", "zeta.count = 4"});
  model.set_min_level(Level::kTrace);
  model.set_shown("kernel", false);
  REQUIRE_FALSE(model.shown("kernel"));
  REQUIRE(VisibleText(model) ==
          std::vector<std::string>{"> zeta.count", "zeta.count = 4", "second", "third"});

  for (int index = 0; index < 5000; ++index) {
    Write(Level::kInfo, "kernel", "filler");
  }
  model.Poll();
  REQUIRE(model.lines().size() == ConsoleModel::kMaxLines);
}
