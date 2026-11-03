/**
 * @file        tests/unit/app/cli_action_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"

#include <array>
#include <span>
#include <string>
#include <vector>

#include <rex/app/app.h>
#include <rex/cli.h>
#include <rex/setting.h>
#include <rex/settings.h>

#include "base/app_harness.h"
#include "app/command_line.h"

namespace {

constexpr std::array<rex::CLIAction, 2> kActions = {
    REX_CLI_ACTION(repair, "Opens the repair mode."),
    REX_CLI_ACTION(dump_state, "Writes the state of the app."),
};

rex::CLIArgList Args(std::vector<std::string> args) {
  return rex::CLIArgList::Parse(args);
}

class ActionApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  ActionApp() : ObjectApp("action_app"), folder_("rex_action_app_") {}

  rex::Result<void> Start(std::vector<std::string> args) { return Init(Args(std::move(args))); }

  std::vector<rex::CLIActionHit> hits;

 protected:
  rex::Result<void> OnConfigure(rex::Settings&) override {
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    return rex::Ok();
  }

  std::span<const rex::CLIAction> GetActions() const override { return kActions; }

  rex::Result<void> OnAction(const rex::CLIActionHit& hit) override {
    hits.push_back(hit);
    return rex::Ok();
  }

 private:
  rex::test::TempFolder folder_;
};

}  // namespace

REX_TEST_CASE("REX_CLI_ACTION keeps the name and the description", "[app][cli][action]") {
  REQUIRE(kActions[0].name == "repair");
  REQUIRE(kActions[0].description == "Opens the repair mode.");
  REQUIRE(kActions[1].name == "dump_state");
}

REX_TEST_CASE("An action name is lowercase with digits, underscores and hyphens",
              "[app][cli][action]") {
  REQUIRE(rex::detail::IsActionName("repair"));
  REQUIRE(rex::detail::IsActionName("dump-state_2"));
  REQUIRE_FALSE(rex::detail::IsActionName(""));
  REQUIRE_FALSE(rex::detail::IsActionName("Repair"));
  REQUIRE_FALSE(rex::detail::IsActionName("a.b"));
  REQUIRE_FALSE(rex::detail::IsActionName("2fast"));
  REQUIRE_FALSE(rex::detail::IsActionName("no-repair name"));
}

REX_TEST_CASE("A declared action is routed to the hits and never to the settings",
              "[app][cli][action]") {
  ActionApp app;
  REQUIRE(app.Start({"--repair", "--clock.no_scaling"}));
  REQUIRE(app.hits.size() == 1);
  CHECK(app.hits[0].action.name == "repair");
  CHECK_FALSE(app.hits[0].option.value.has_value());
  CHECK(REX_SETTINGS().Find("clock.no_scaling").value()->GetText() == "true");
  app.Shutdown();
}

REX_TEST_CASE("An action receives its value", "[app][cli][action]") {
  ActionApp app;
  REQUIRE(app.Start({"--dump_state=slot1"}));
  REQUIRE(app.hits.size() == 1);
  CHECK(app.hits[0].option.value == "slot1");
  app.Shutdown();
}

REX_TEST_CASE("Actions are reported in command line order and a repeat reports twice",
              "[app][cli][action]") {
  ActionApp app;
  REQUIRE(app.Start({"--dump_state", "--repair", "--dump_state"}));
  REQUIRE(app.hits.size() == 3);
  CHECK(app.hits[0].action.name == "dump_state");
  CHECK(app.hits[1].action.name == "repair");
  CHECK(app.hits[2].action.name == "dump_state");
  app.Shutdown();
}

REX_TEST_CASE("An action takes no negation", "[app][cli][action]") {
  ActionApp app;
  auto started = app.Start({"--no-repair"});
  REQUIRE_FALSE(started);
  CHECK(started.error().message == "unknown option '--no-repair'");
  CHECK(app.hits.empty());
  app.Shutdown();
}

REX_TEST_CASE("An undeclared action name is unknown", "[app][cli][action]") {
  auto parsed = rex::app::detail::ParseCommandLine(Args({"--repair"}));
  REQUIRE_FALSE(parsed);
  CHECK(parsed.error().message == "unknown option '--repair'");
}

REX_TEST_CASE("Declared actions validate when names are unique and not structural flags",
              "[app][cli][action]") {
  REQUIRE(rex::app::detail::ValidateCLIActions(kActions));
}

REX_TEST_CASE("A repeated action name fails validation", "[app][cli][action]") {
  constexpr std::array<rex::CLIAction, 2> kRepeated = {
      rex::CLIAction{"repair", "First."},
      rex::CLIAction{"repair", "Second."},
  };
  auto checked = rex::app::detail::ValidateCLIActions(kRepeated);
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().message.find("repair") != std::string::npos);
}

REX_TEST_CASE("An action named like a structural flag fails validation", "[app][cli][action]") {
  constexpr std::array<rex::CLIAction, 1> kClash = {rex::CLIAction{"config_file", "Clashes."}};
  auto checked = rex::app::detail::ValidateCLIActions(kClash);
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().message.find("config_file") != std::string::npos);
}

REX_TEST_CASE("An action name with a dot fails validation", "[app][cli][action]") {
  constexpr std::array<rex::CLIAction, 1> kDotted = {rex::CLIAction{"gpu.vsync", "Clashes."}};
  REQUIRE_FALSE(rex::app::detail::ValidateCLIActions(kDotted));
}
