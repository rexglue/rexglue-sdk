/**
 * @file        tests/unit/ui/console_model_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/command.h>
#include "ui/overlay/console/model.h"

namespace {

std::string g_console_args;

class AlphaOwner final : public rex::SettingOwner {
 public:
  AlphaOwner() : SettingOwner("alpha") {}

  REX_SETTING(std::string, mode, "low", "Quality mode", rex::Allowed("low", "high"));
  REX_COMMAND(run, "Records its arguments", [](std::string_view args) {
    g_console_args = std::string(args);
    return rex::Ok();
  });
};

class ZetaOwner final : public rex::SettingOwner {
 public:
  ZetaOwner() : SettingOwner("zeta") {}

  REX_SETTING(rex::i32, count, 4, "A counted value", rex::Range{1, 8});
  REX_SETTING(rex::i32, frozen, 1, "Locked by the title");
};

class ManyOwner final : public rex::SettingOwner {
 public:
  explicit ManyOwner(std::string_view name)
      : SettingOwner("many"), value(this, name, false, "Generated") {}

  rex::Setting<bool> value;
};

using rex::log::Level;
using rex::ui::ConsoleModel;

struct Harness {
  Harness() {
    REQUIRE(REX_SETTINGS().Register(alpha));
    REQUIRE(REX_SETTINGS().Register(zeta));
  }

  ~Harness() {
    REX_SETTINGS().Unregister(zeta);
    REX_SETTINGS().Unregister(alpha);
  }

  void Write(Level level, std::string_view category, std::string_view text) {
    rex::log::Message message;
    message.level = level;
    message.category = category;
    message.text = text;
    message.sequence = ++sequence;
    capture.Write(message);
  }

  AlphaOwner alpha;
  ZetaOwner zeta;
  rex::log::CaptureSink capture;
  rex::u64 sequence = 0;
};

struct TempFolder : rex::test::TempFolder {
  TempFolder() : rex::test::TempFolder("rex_console_test_") {}
};

const std::string& Last(const ConsoleModel& model) {
  return model.GetLines().back().text;
}

std::vector<std::string> VisibleText(const ConsoleModel& model) {
  std::vector<std::string> out;
  for (const rex::ui::ConsoleLine* line : model.GetVisibleLines()) {
    out.push_back(line->text);
  }
  return out;
}

}  // namespace

REX_TEST_CASE_METHOD(Harness, "Execute sets, prints, reports, runs and clears", "[ui][console]") {
  ConsoleModel model(REX_SETTINGS(), capture, {});
  model.Execute("zeta.count 6");
  REQUIRE(zeta.count.Get() == 6);
  REQUIRE(Last(model) == "zeta.count = 6");
  model.Execute("  zeta.count  ");
  REQUIRE(Last(model) == "zeta.count = 6");
  model.Execute("zeta.count 5\r\n");
  REQUIRE(zeta.count.Get() == 5);
  model.Execute("zeta.count 6");
  model.Execute("zeta.nope");
  REQUIRE(Last(model) == "unknown setting zeta.nope");
  model.Execute("zeta.count 99");
  REQUIRE(model.GetLines().back().level == Level::kWarn);
  REQUIRE(zeta.count.Get() == 6);
  model.Execute("alpha.run first second");
  REQUIRE(g_console_args == "first second");
  model.Execute("clear");
  REQUIRE(model.GetLines().empty());
}

REX_TEST_CASE_METHOD(Harness, "Execute refuses a frozen setting and keeps its value",
                     "[ui][console]") {
  ConsoleModel model(REX_SETTINGS(), capture, {});
  REQUIRE(zeta.frozen.Lock(1));
  model.Execute("zeta.frozen 5");
  REQUIRE(model.GetLines().back().level == Level::kWarn);
  REQUIRE(zeta.frozen.Get() == 1);
}

REX_TEST_CASE_METHOD(Harness, "Complete sorts by scope then name with built ins first",
                     "[ui][console]") {
  ConsoleModel model(REX_SETTINGS(), capture, {});
  REQUIRE(model.Complete("").empty());
  REQUIRE(model.Complete("alpha.mode x").empty());

  const auto alpha_matches = model.Complete("alpha.");
  REQUIRE(alpha_matches.size() == 2);
  REQUIRE(alpha_matches[0].name == "alpha.mode");
  REQUIRE(alpha_matches[0].scope == "alpha");
  REQUIRE(alpha_matches[0].description == "Quality mode");
  REQUIRE_FALSE(alpha_matches[0].command);
  REQUIRE(alpha_matches[1].name == "alpha.run");
  REQUIRE(alpha_matches[1].command);

  const auto help = model.Complete("he");
  REQUIRE(help.size() == 1);
  REQUIRE(help[0].name == "help");
  REQUIRE(help[0].scope.empty());

  std::deque<ManyOwner> owners;
  for (int index = 0; index < 70; ++index) {
    owners.emplace_back(std::format("v{:02}", index));
    REQUIRE(REX_SETTINGS().Register(owners.back()));
  }
  const auto many = model.Complete("m");
  REQUIRE(many.size() == ConsoleModel::kMaxCompletions);
  REQUIRE(many.front().name == "many.v00");
  for (ManyOwner& owner : owners) {
    REX_SETTINGS().Unregister(owner);
  }
}

REX_TEST_CASE_METHOD(Harness, "HelpLine describes a setting, a command and nothing else",
                     "[ui][console]") {
  ConsoleModel model(REX_SETTINGS(), capture, {});
  REQUIRE(model.HelpLine("alpha.mode") ==
          "alpha.mode string = low, default low, allowed low high. Quality mode");
  REQUIRE(model.HelpLine("zeta.count 3") ==
          "zeta.count i32 = 4, default 4, range 1 to 8. A counted value");
  REQUIRE(model.HelpLine("alpha.run") == "alpha.run command. Records its arguments");
  REQUIRE(model.HelpLine("clear").starts_with("clear. "));
  REQUIRE(model.HelpLine("zeta.nope").empty());
}

REX_TEST_CASE_METHOD(Harness, "History walks, skips repeats, caps and round trips",
                     "[ui][console]") {
  TempFolder temp;
  const auto path = temp.path / "history.txt";
  std::string newest;
  {
    ConsoleModel model(REX_SETTINGS(), capture, path);
    model.Execute("zeta.count 2");
    model.Execute("zeta.count 2");
    model.Execute("zeta.count 3");
    REQUIRE(model.GetHistory().size() == 2);
    REQUIRE(model.StepHistoryBack() == "zeta.count 3");
    REQUIRE(model.StepHistoryBack() == "zeta.count 2");
    REQUIRE(model.StepHistoryBack() == "zeta.count 2");
    REQUIRE(model.StepHistoryForward() == "zeta.count 3");
    REQUIRE(model.StepHistoryForward().empty());
    for (int index = 0; index < 300; ++index) {
      model.Execute(std::format("zeta.count {}", 1 + index % 8));
    }
    REQUIRE(model.GetHistory().size() == ConsoleModel::kMaxHistory);
    newest = model.GetHistory().back();
  }
  ConsoleModel reloaded(REX_SETTINGS(), capture, path);
  REQUIRE(reloaded.GetHistory().size() == ConsoleModel::kMaxHistory);
  REQUIRE(reloaded.GetHistory().back() == newest);
  REQUIRE(reloaded.StepHistoryBack() == newest);
}

REX_TEST_CASE_METHOD(Harness, "Output interleaves by sequence, caps and filters", "[ui][console]") {
  ConsoleModel model(REX_SETTINGS(), capture, {});
  Write(Level::kInfo, "kernel", "first");
  model.Execute("zeta.count");
  Write(Level::kDebug, "gpu", "second\nthird");
  model.Poll();
  REQUIRE(VisibleText(model) ==
          std::vector<std::string>{"first", "> zeta.count", "zeta.count = 4", "second", "third"});
  REQUIRE(model.GetCategories().contains("kernel"));
  REQUIRE(model.GetCategories().contains("gpu"));

  model.SetMinLevel(Level::kInfo);
  REQUIRE(VisibleText(model) ==
          std::vector<std::string>{"first", "> zeta.count", "zeta.count = 4"});
  model.SetMinLevel(Level::kTrace);
  model.SetShown("kernel", false);
  REQUIRE_FALSE(model.IsShown("kernel"));
  REQUIRE(VisibleText(model) ==
          std::vector<std::string>{"> zeta.count", "zeta.count = 4", "second", "third"});

  for (int index = 0; index < 5000; ++index) {
    Write(Level::kInfo, "kernel", "filler");
  }
  model.Poll();
  REQUIRE(model.GetLines().size() == ConsoleModel::kMaxLines);
}
