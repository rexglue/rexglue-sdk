/**
 * @file        tests/unit/core/cli_action_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <string>
#include <vector>

#include <rex/api.h>
#include <rex/cli_action.h>
#include <rex/cli_arg_list.h>

namespace {

constexpr std::array<rex::CLIAction, 2> kActions = {
    REX_CLI_ACTION(repair, "Opens the repair mode."),
    REX_CLI_ACTION(dump_state, "Writes the state of the app."),
};

rex::CLIArgList Args(std::vector<std::string> args) {
  return rex::CLIArgList::Parse(args);
}

}  // namespace

REX_TEST_CASE("REX_CLI_ACTION keeps the name and the description", "[core][cli][action]") {
  REQUIRE(kActions[0].name == "repair");
  REQUIRE(kActions[0].description == "Opens the repair mode.");
  REQUIRE(kActions[1].name == "dump_state");
}

REX_TEST_CASE("An action name is lowercase with digits, underscores and hyphens",
              "[core][cli][action]") {
  REQUIRE(rex::detail::IsActionName("repair"));
  REQUIRE(rex::detail::IsActionName("dump-state_2"));
  REQUIRE_FALSE(rex::detail::IsActionName(""));
  REQUIRE_FALSE(rex::detail::IsActionName("Repair"));
  REQUIRE_FALSE(rex::detail::IsActionName("a.b"));
  REQUIRE_FALSE(rex::detail::IsActionName("2fast"));
  REQUIRE_FALSE(rex::detail::IsActionName("no-repair name"));
}

REX_TEST_CASE("A declared action is routed to the hits and never to the cvars",
              "[core][cli][action]") {
  rex::Config config;
  std::vector<rex::CLIActionHit> hits;
  auto unknown = rex::ParseCommandLine(config, Args({"--repair", "--gpu.vsync"}), kActions, &hits);
  REQUIRE(unknown.empty());
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].action.name == "repair");
  REQUIRE_FALSE(hits[0].option.value.has_value());
  REQUIRE(config.cvars.values.size() == 1);
  REQUIRE(config.cvars.Get("gpu.vsync") == "true");
}

REX_TEST_CASE("An action receives its value", "[core][cli][action]") {
  rex::Config config;
  std::vector<rex::CLIActionHit> hits;
  auto unknown = rex::ParseCommandLine(config, Args({"--dump_state=slot1"}), kActions, &hits);
  REQUIRE(unknown.empty());
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].option.value == "slot1");
}

REX_TEST_CASE("Actions are reported in command line order and a repeat reports twice",
              "[core][cli][action]") {
  rex::Config config;
  std::vector<rex::CLIActionHit> hits;
  rex::ParseCommandLine(config, Args({"--dump_state", "--repair", "--dump_state"}), kActions,
                        &hits);
  REQUIRE(hits.size() == 3);
  REQUIRE(hits[0].action.name == "dump_state");
  REQUIRE(hits[1].action.name == "repair");
  REQUIRE(hits[2].action.name == "dump_state");
}

REX_TEST_CASE("An action takes no negation", "[core][cli][action]") {
  rex::Config config;
  std::vector<rex::CLIActionHit> hits;
  auto unknown = rex::ParseCommandLine(config, Args({"--no-repair"}), kActions, &hits);
  REQUIRE(unknown == std::vector<std::string>{"--no-repair"});
  REQUIRE(hits.empty());
}

REX_TEST_CASE("An undeclared action name is unknown", "[core][cli][action]") {
  rex::Config config;
  auto unknown = rex::ParseCommandLine(config, Args({"--repair"}));
  REQUIRE(unknown == std::vector<std::string>{"--repair"});
}

REX_TEST_CASE("Declared actions validate when names are unique and not structural flags",
              "[core][cli][action]") {
  REQUIRE(rex::ValidateCLIActions(kActions));
}

REX_TEST_CASE("A repeated action name fails validation", "[core][cli][action]") {
  constexpr std::array<rex::CLIAction, 2> kRepeated = {
      rex::CLIAction{"repair", "First."},
      rex::CLIAction{"repair", "Second."},
  };
  auto checked = rex::ValidateCLIActions(kRepeated);
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().message.find("repair") != std::string::npos);
}

REX_TEST_CASE("An action named like a structural flag fails validation", "[core][cli][action]") {
  constexpr std::array<rex::CLIAction, 1> kClash = {rex::CLIAction{"backend", "Clashes."}};
  auto checked = rex::ValidateCLIActions(kClash);
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().message.find("backend") != std::string::npos);
}

REX_TEST_CASE("An action name with a dot fails validation", "[core][cli][action]") {
  constexpr std::array<rex::CLIAction, 1> kDotted = {rex::CLIAction{"gpu.vsync", "Clashes."}};
  REQUIRE_FALSE(rex::ValidateCLIActions(kDotted));
}
