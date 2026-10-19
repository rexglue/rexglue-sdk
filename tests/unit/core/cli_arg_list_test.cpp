/**
 * @file        tests/unit/core/cli_arg_list_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>
#include <vector>

#include <rex/api.h>
#include <rex/cli_arg_list.h>

namespace {

rex::CLIArgList Args(std::vector<std::string> args) {
  return rex::CLIArgList::Parse(args);
}

}  // namespace

REX_TEST_CASE("CLIArgList splits options, values and positionals", "[core][cli]") {
  auto args = Args({"first", "--a=1", "--b", "2", "--c", "--d", "last"});
  REQUIRE(args.Get("a") == "1");
  REQUIRE(args.Get("b") == "2");
  REQUIRE(args.Has("c"));
  REQUIRE_FALSE(args.Get("c").has_value());
  REQUIRE(args.Get("d") == "last");
  REQUIRE(args.GetPositionals().size() == 1);
  REQUIRE(args.GetPositionals()[0] == "first");
  REQUIRE(args.GetAll().size() == 7);
  REQUIRE(args.GetOptions().size() == 4);
  REQUIRE(args.GetOptions()[1].name == "b");
}

REX_TEST_CASE("CLIArgList skips argv[0]", "[core][cli]") {
  char program[] = "foo.exe";
  char option[] = "--x=1";
  char* argv[] = {program, option};
  auto args = rex::CLIArgList::Parse(2, argv);
  REQUIRE(args.GetAll().size() == 1);
  REQUIRE(args.Get("x") == "1");
}

REX_TEST_CASE("A trailing option without a value has none", "[core][cli]") {
  auto args = Args({"--last"});
  REQUIRE(args.Has("last"));
  REQUIRE_FALSE(args.Get("last").has_value());
}

REX_TEST_CASE("ParseCommandLine fills fields and cvars and returns unknown options",
              "[core][cli]") {
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

REX_TEST_CASE("A structural option without a value is unknown", "[core][cli]") {
  rex::Config config;
  auto unknown = rex::ParseCommandLine(config, Args({"--game_data_root"}));
  REQUIRE(unknown == std::vector<std::string>{"--game_data_root"});
}
