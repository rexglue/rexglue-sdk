/**
 * @file        tests/unit/base/cli_arg_list_test.cpp
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

#include <rex/cli.h>

#include "app/command_line.h"

namespace {

rex::CLIArgList Args(std::vector<std::string> args) {
  return rex::CLIArgList::Parse(args);
}

}  // namespace

REX_TEST_CASE("CLIArgList splits options, values and positionals", "[base][cli]") {
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

REX_TEST_CASE("CLIArgList skips argv[0]", "[base][cli]") {
  char program[] = "foo.exe";
  char option[] = "--x=1";
  char* argv[] = {program, option};
  auto args = rex::CLIArgList::Parse(2, argv);
  REQUIRE(args.GetAll().size() == 1);
  REQUIRE(args.Get("x") == "1");
}

REX_TEST_CASE("A trailing option without a value has none", "[base][cli]") {
  auto args = Args({"--last"});
  REQUIRE(args.Has("last"));
  REQUIRE_FALSE(args.Get("last").has_value());
}

REX_TEST_CASE("A structural option without a value is unknown", "[base][cli]") {
  auto parsed = rex::app::detail::ParseCommandLine(Args({"--game_data_root"}));
  REQUIRE_FALSE(parsed);
  REQUIRE(parsed.error().message == "unknown option '--game_data_root'");
}
