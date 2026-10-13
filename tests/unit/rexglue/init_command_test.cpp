/**
 * @file        tests/unit/rexglue/init_command_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/matchers/catch_matchers_string.hpp>
#include <CLI/CLI.hpp>

#include "rexglue/cli_context.h"
#include "rexglue/commands/init_command.h"
#include "rexglue/ui/ui.h"

namespace {

void WriteText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Parse(CLI::App& app, std::vector<std::string> args) {
  std::ranges::reverse(args);
  app.parse(args);
}

}  // namespace

REX_TEST_CASE("init module appends a module to an existing manifest", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_module_");
  const std::filesystem::path manifest = root.path / "demo_manifest.toml";
  WriteText(manifest,
            "[project]\nname = \"demo\"\n[entrypoint]\nfile_path = \"default.xex\"\n"
            "out_directory_path = \"generated\"\n");
  WriteText(root.path / "default.xex", "");
  WriteText(root.path / "game" / "a.xex", "");

  CLI::App app;
  app.require_subcommand(1);
  const rexglue::cli::CLIContext ctx;
  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterInit(app, ctx, pending);
  REQUIRE_NOTHROW(
      Parse(app, {"init", "module", "--project-root", root.path.string(), "--xex-path",
                  (root.path / "game" / "a.xex").string(), "--guest-path", "game/a.xex"}));
  REQUIRE(pending);
  rexglue::ui::Init({.tty = false, .color = false});
  const rex::Result<void> result = pending();
  rexglue::ui::Shutdown();
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const std::string text = ReadText(manifest);
  CHECK(text.find("[[modules]]") != std::string::npos);
  CHECK(text.find("a.xex") != std::string::npos);
}

REX_TEST_CASE("init without a project name still reports it as required", "[rexglue][init]") {
  CLI::App app;
  app.require_subcommand(1);
  const rexglue::cli::CLIContext ctx;
  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterInit(app, ctx, pending);
  CHECK_THROWS_WITH(Parse(app, {"init", "--xex-path", "default.xex"}),
                    Catch::Matchers::ContainsSubstring("--project-name is required"));
}
