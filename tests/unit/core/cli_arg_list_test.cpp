/**
 * @file        tests/unit/core/cli_arg_list_test.cpp
 * @brief       Unit tests for CLIArgList and ParseCommandLine
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
#include <rex/cli_arg_list.h>

namespace {

rex::CLIArgList Args(std::vector<std::string> args) {
  return rex::CLIArgList::Parse(args);
}

}  // namespace

TEST_CASE("CLIArgList splits options, values and positionals", "[cli]") {
  auto args = Args({"first", "--a=1", "--b", "2", "--c", "--d", "last"});
  REQUIRE(args.Get("a") == "1");
  REQUIRE(args.Get("b") == "2");
  REQUIRE(args.Has("c"));
  REQUIRE_FALSE(args.Get("c").has_value());
  REQUIRE(args.Get("d") == "last");
  REQUIRE(args.Positionals().size() == 1);
  REQUIRE(args.Positionals()[0] == "first");
  REQUIRE(args.All().size() == 7);
  REQUIRE(args.Options().size() == 4);
  REQUIRE(args.Options()[1].name == "b");
}

TEST_CASE("CLIArgList skips argv[0]", "[cli]") {
  char program[] = "foo.exe";
  char option[] = "--x=1";
  char* argv[] = {program, option};
  auto args = rex::CLIArgList::Parse(2, argv);
  REQUIRE(args.All().size() == 1);
  REQUIRE(args.Get("x") == "1");
}

TEST_CASE("A trailing option without a value has none", "[cli]") {
  auto args = Args({"--last"});
  REQUIRE(args.Has("last"));
  REQUIRE_FALSE(args.Get("last").has_value());
}

TEST_CASE("ParseCommandLine fills fields and cvars and returns unknown options", "[cli]") {
  rex::Config config;
  auto unknown = rex::ParseCommandLine(
      config, Args({"--game_data_root=C:/g", "--gpu.vsync", "--no-ui.fullscreen", "--foo",
                    "--Bad.Name=1", "--modules", "a,b", "pos"}));
  REQUIRE(config.paths.game_data_root == "C:/g");
  REQUIRE(config.modules == std::vector<std::string>{"a", "b"});
  REQUIRE(config.cvars.Get("gpu.vsync") == "true");
  REQUIRE(config.cvars.Get("ui.fullscreen") == "false");
  REQUIRE(unknown == std::vector<std::string>{"--foo", "--Bad.Name=1"});
}

TEST_CASE("A structural option without a value is unknown", "[cli]") {
  rex::Config config;
  auto unknown = rex::ParseCommandLine(config, Args({"--game_data_root"}));
  REQUIRE(unknown == std::vector<std::string>{"--game_data_root"});
}
