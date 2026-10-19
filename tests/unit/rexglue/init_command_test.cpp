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
#include "temp_folder.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/matchers/catch_matchers_string.hpp>
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "rexglue/cli_context.h"
#include "rexglue/commands/init_command.h"
#include "rexglue/ui/ui.h"
#include "file_text.h"

namespace {

using rex::test::ReadText;
using rex::test::WriteText;

void Parse(CLI::App& app, std::vector<std::string> args) {
  std::ranges::reverse(args);
  app.parse(args);
}

rex::Result<void> RunInit(const std::filesystem::path& root, bool force,
                          const std::string& name = "demo") {
  CLI::App app;
  app.require_subcommand(1);
  rexglue::cli::CLIContext ctx;
  ctx.overwrite_existing = force;
  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterInit(app, ctx, pending);
  Parse(app, {"init", "--project-name", name, "--project-root", root.string(), "--xex-path",
              (root / "default.xex").string()});
  rexglue::ui::Init({.tty = false, .color = false});
  rex::Result<void> result = pending();
  rexglue::ui::Shutdown();
  return result;
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

REX_TEST_CASE("init keeps the case of the host file paths in the manifest", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_case_");
  WriteText(root.path / "Game" / "Default.XEX", "");
  WriteText(root.path / "Game" / "Mods" / "Plugin.DLL", "");

  CLI::App app;
  app.require_subcommand(1);
  const rexglue::cli::CLIContext ctx;
  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterInit(app, ctx, pending);
  REQUIRE_NOTHROW(
      Parse(app, {"init", "--project-name", "demo", "--project-root", root.path.string(),
                  "--xex-path", (root.path / "Game" / "Default.XEX").string(), "--scan-dll"}));
  REQUIRE(pending);
  rexglue::ui::Init({.tty = false, .color = false});
  const rex::Result<void> result = pending();
  rexglue::ui::Shutdown();
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const std::string text = ReadText(root.path / "demo_manifest.toml");
  CHECK(text.find("Game/Default.XEX") != std::string::npos);
  CHECK(text.find("Game/Mods/Plugin.DLL") != std::string::npos);
  CHECK(text.find("generated/default") != std::string::npos);
  CHECK(text.find("generated/plugin") != std::string::npos);
}

REX_TEST_CASE("init without a project name reports it as required", "[rexglue][init]") {
  CLI::App app;
  app.require_subcommand(1);
  const rexglue::cli::CLIContext ctx;
  rexglue::cli::DeferredAction pending;
  rexglue::cli::RegisterInit(app, ctx, pending);
  CHECK_THROWS_WITH(Parse(app, {"init", "--xex-path", "default.xex"}),
                    Catch::Matchers::ContainsSubstring("--project-name is required"));
}

REX_TEST_CASE("init writes the host hooks source once", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_hooks_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::filesystem::path hooks = root.path / "src" / "demo_hooks.cpp";
  REQUIRE(std::filesystem::exists(hooks));
  CHECK(ReadText(hooks).find("demo_funcs.h") != std::string::npos);
  WriteText(hooks, "edited");
  REQUIRE(RunInit(root.path, true));
  CHECK(ReadText(hooks) == "edited");
}

REX_TEST_CASE("init splits the host code into an object library", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_split_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::string text = ReadText(root.path / "CMakeLists.txt");
  CHECK(text.find("add_library(demo_common OBJECT") != std::string::npos);
  CHECK(text.find("src/demo_hooks.cpp") != std::string::npos);
  CHECK(text.find("add_executable(demo WIN32 src/main.cpp)") != std::string::npos);
  CHECK(text.find("add_executable(demo src/main.cpp)") != std::string::npos);
  CHECK(text.find("rexglue_setup_host_library(demo_common)") != std::string::npos);
  CHECK(text.find("target_link_libraries(demo PRIVATE demo_common)") != std::string::npos);
  CHECK(text.find("cmake_policy(SET CMP0184 NEW)") != std::string::npos);
  CHECK(text.find("cmake_policy(SET CMP0184 NEW)") < text.find("project(demo"));
}

REX_TEST_CASE("init writes the recompiled library macros", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_macros_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::string text = ReadText(root.path / "generated" / "rexglue.cmake");
  CHECK(text.find("macro (rexglue_setup_host_library") != std::string::npos);
  CHECK(text.find("NOT TARGET demo_recompiled") != std::string::npos);
  CHECK(text.find("add_library(demo_recompiled OBJECT") != std::string::npos);
  CHECK(text.find("set(REXGLUE_RECOMP_OPTIMIZATION") != std::string::npos);
  CHECK(text.find("Optimization level for generated code") != std::string::npos);
  CHECK(text.find("MSVC_RUNTIME_CHECKS") != std::string::npos);
  CHECK(text.find("demo_recomp ") == std::string::npos);
}

REX_TEST_CASE("init maps the optimization level per MSVC style driver", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_map_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::string text = ReadText(root.path / "generated" / "rexglue.cmake");
  CHECK(text.find("/clang:-${REXGLUE_RECOMP_OPTIMIZATION}") != std::string::npos);
  CHECK(text.find("list(APPEND REXGLUE_RECOMP_OPTIONS /Od)") != std::string::npos);
  CHECK(text.find("list(APPEND REXGLUE_RECOMP_OPTIONS /O2 /Ob1)") != std::string::npos);
  CHECK(text.find("list(APPEND REXGLUE_RECOMP_OPTIONS /O2 /Ob2)") != std::string::npos);
  CHECK(text.find("/O1") == std::string::npos);
}

REX_TEST_CASE("init sets the recompiled optimization only in Debug presets", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_presets_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const auto presets = nlohmann::json::parse(ReadText(root.path / "CMakePresets.json"));
  size_t debug = 0;
  for (const auto& preset : presets.at("configurePresets")) {
    const auto vars = preset.value("cacheVariables", nlohmann::json::object());
    const std::string type = vars.value("CMAKE_BUILD_TYPE", "");
    if (type == "Debug") {
      ++debug;
      CHECK(vars.value("REXGLUE_RECOMP_OPTIMIZATION", "") == "O1");
    } else {
      CHECK_FALSE(vars.contains("REXGLUE_RECOMP_OPTIMIZATION"));
    }
  }
  CHECK(debug == 6);
}

REX_TEST_CASE("init checks the optimization value", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_opt_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::string text = ReadText(root.path / "generated" / "rexglue.cmake");
  CHECK(text.find("REXGLUE_RECOMP_OPTIMIZATION MATCHES \"^O[0-3]$\"") != std::string::npos);
  CHECK(text.find("must be empty or one of O0, O1, O2, O3") != std::string::npos);
}

REX_TEST_CASE("init applies the recompiled options to the library target only", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_once_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::string text = ReadText(root.path / "generated" / "rexglue.cmake");
  const size_t library = text.find("add_library(demo_recompiled OBJECT");
  REQUIRE(library != std::string::npos);
  const size_t clear = text.find("PROPERTIES COMPILE_OPTIONS \"\"", library);
  const size_t apply = text.find("rexglue_apply_recomp_settings(", library);
  REQUIRE(clear != std::string::npos);
  REQUIRE(apply != std::string::npos);
  CHECK(clear < apply);
}

REX_TEST_CASE("init keeps the separators of the project name", "[rexglue][init][case]") {
  rex::test::TempFolder root("rexglue_init_name_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false, "my_game-2"));
  CHECK(std::filesystem::exists(root.path / "src" / "my_game-2_hooks.cpp"));
  const std::string text = ReadText(root.path / "CMakeLists.txt");
  CHECK(text.find("project(my_game-2 ") != std::string::npos);
  CHECK(text.find("add_library(my_game-2_common OBJECT") != std::string::npos);
  const std::string glue = ReadText(root.path / "generated" / "rexglue.cmake");
  CHECK(glue.find("my_game-2_codegen") != std::string::npos);
}
