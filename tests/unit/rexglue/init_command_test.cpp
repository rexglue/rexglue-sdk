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
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
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

REX_TEST_CASE("init writes one multi config preset per platform", "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_presets_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const auto presets = nlohmann::json::parse(ReadText(root.path / "CMakePresets.json"));
  std::set<std::string> configure;
  for (const auto& preset : presets.at("configurePresets")) {
    if (!preset.contains("inherits")) {
      CHECK(preset.value("generator", "") == "Ninja Multi-Config");
    }
    const auto vars = preset.value("cacheVariables", nlohmann::json::object());
    CHECK_FALSE(vars.contains("CMAKE_BUILD_TYPE"));
    CHECK_FALSE(vars.contains("REXGLUE_RECOMP_OPTIMIZATION"));
    if (!preset.value("hidden", false)) {
      configure.insert(preset.at("name").get<std::string>());
    }
  }
  CHECK(configure == std::set<std::string>{"win-amd64", "win-arm64", "linux-amd64", "linux-arm64",
                                           "mac-amd64", "mac-arm64"});
  size_t builds = 0;
  for (const auto& preset : presets.at("buildPresets")) {
    ++builds;
    const std::string configuration = preset.at("configuration");
    CHECK(configure.contains(preset.at("configurePreset").get<std::string>()));
    CHECK(preset.at("name").get<std::string>() ==
          preset.at("configurePreset").get<std::string>() + "-" + [&] {
            std::string lower = configuration;
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
            return lower;
          }());
  }
  CHECK(builds == 18);
}

REX_TEST_CASE(
    "init writes the recompiled library macros and applies the options to the library only",
    "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_once_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::string text = ReadText(root.path / "generated" / "rexglue.cmake");
  CHECK(text.find("macro (rexglue_setup_host_library") != std::string::npos);
  CHECK(text.find("NOT TARGET demo_recompiled") != std::string::npos);
  CHECK(text.find("rexglue_recomp_options(REXGLUE_RECOMP_OPTIONS REXGLUE_RECOMP_OPTIMIZED)") !=
        std::string::npos);
  CHECK(text.find("MSVC_RUNTIME_CHECKS") != std::string::npos);
  CHECK(text.find("if (MSVC AND REXGLUE_RECOMP_OPTIMIZED)") != std::string::npos);
  CHECK(text.find("demo_recomp ") == std::string::npos);
  const size_t library = text.find("add_library(demo_recompiled OBJECT");
  REQUIRE(library != std::string::npos);
  const size_t clear = text.find("PROPERTIES COMPILE_OPTIONS \"\"", library);
  const size_t apply = text.find("rexglue_apply_recomp_settings(", library);
  REQUIRE(clear != std::string::npos);
  REQUIRE(apply != std::string::npos);
  CHECK(clear < apply);
}

REX_TEST_CASE("init keeps the underscores of the project name", "[rexglue][init][case]") {
  rex::test::TempFolder root("rexglue_init_name_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false, "my_game_2"));
  CHECK(std::filesystem::exists(root.path / "src" / "my_game_2_hooks.cpp"));
  const std::string text = ReadText(root.path / "CMakeLists.txt");
  CHECK(text.find("project(my_game_2 ") != std::string::npos);
  CHECK(text.find("add_library(my_game_2_common OBJECT") != std::string::npos);
  const std::string glue = ReadText(root.path / "generated" / "rexglue.cmake");
  CHECK(glue.find("my_game_2_codegen") != std::string::npos);
}

REX_TEST_CASE("init rejects a project name the manifest loader would refuse",
              "[rexglue][init][case]") {
  for (const char* name : {"my-game", "my game", "9lives", "_game"}) {
    rex::test::TempFolder root("rexglue_init_bad_name_");
    WriteText(root.path / "default.xex", "");
    const auto result = RunInit(root.path, false, name);
    CHECK_FALSE(result);
    CHECK_FALSE(std::filesystem::exists(root.path / "CMakeLists.txt"));
  }
}

REX_TEST_CASE("init names the character it rejects in the project name", "[rexglue][init][case]") {
  rex::test::TempFolder root("rexglue_init_bad_char_");
  WriteText(root.path / "default.xex", "");
  const auto result = RunInit(root.path, false, "my-game");
  REQUIRE_FALSE(result);
  const std::string text = result.error().GetDescription();
  CHECK(text.find("'-'") != std::string::npos);
  CHECK(text.find("letters, digits and underscore") != std::string::npos);
}

REX_TEST_CASE("init renders codegen with the file size setting and the sources as outputs",
              "[rexglue][init]") {
  rex::test::TempFolder root("rexglue_init_codegen_");
  WriteText(root.path / "default.xex", "");
  REQUIRE(RunInit(root.path, false));
  const std::string text = ReadText(root.path / "generated" / "rexglue.cmake");
  const size_t call = text.find("rexglue_add_codegen(\n  demo_codegen");
  REQUIRE(call != std::string::npos);
  CHECK(text.find("${_rexglue_codegen_max_file_size}", call) != std::string::npos);
  CHECK(text.find("MAX_FILE_SIZE \"${REXGLUE_RECOMP_MAX_FILE_SIZE}\"") < call);
  CHECK(text.find("MAX_FILE_SIZE", call) == std::string::npos);
  CHECK(text.find("SOURCES ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}", call) != std::string::npos);
  CHECK(text.find("set(REXGLUE_RECOMP_MAX_FILE_SIZE") != std::string::npos);
}