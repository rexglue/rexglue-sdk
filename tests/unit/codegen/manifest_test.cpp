/**
 * @file        tests/unit/codegen/manifest_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "file_text.h"
#include "temp_folder.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "codegen/manifest.h"
#include "codegen/template_registry.h"

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kProject =
    "[project]\nname = \"demo\"\n[entrypoint]\nfile_path = \"default.xex\"\n\n";

nlohmann::json ModuleEntry() {
  return {{"guest_path", "game/a.xex"},
          {"file_path", "game/a.xex"},
          {"out_directory_path", "generated/a"}};
}

std::vector<rex::codegen::BinaryConfig> ModulesOf(
    const std::optional<rex::codegen::ManifestConfig>& manifest) {
  return manifest ? manifest->modules : std::vector<rex::codegen::BinaryConfig>{};
}

void Touch(const fs::path& path) {
  std::ofstream file(path);
}

fs::path WriteProject(const rex::test::TempFolder& folder, std::string_view modules) {
  Touch(folder.path / "default.xex");
  fs::create_directories(folder.path / "game");
  Touch(folder.path / "game" / "a.xex");
  const auto path = folder.path / "demo_manifest.toml";
  std::ofstream(path) << kProject << modules;
  return path;
}

fs::path WriteManifest(const rex::test::TempFolder& folder, std::string_view module_key) {
  return WriteProject(folder, std::string("[[modules]]\nfile_path = \"game/a.xex\"\n") +
                                  std::string(module_key) + " = \"game/a.xex\"\n");
}

}  // namespace

REX_TEST_CASE("A manifest module reads its guest path", "[codegen]") {
  rex::test::TempFolder folder;
  const auto manifest = rex::codegen::ManifestConfig::Load(WriteManifest(folder, "guest_path"));
  REQUIRE(manifest);
  const auto modules = ModulesOf(manifest);
  REQUIRE(modules.size() == 1);
  REQUIRE(modules[0].guest_path == "game/a.xex");
}

REX_TEST_CASE("An init module entry renders from an override directory", "[codegen]") {
  rex::test::TempFolder folder;
  fs::create_directories(folder.path / "init");
  rex::test::WriteText(folder.path / "init" / "manifest_module.inja",
                       "override {{ m.guest_path }}\n");
  rex::codegen::TemplateRegistry registry;
  registry.LoadOverrides(folder.path);
  const nlohmann::json data = {{"m", ModuleEntry()}};
  REQUIRE(registry.Render("init/manifest_module", data.dump()) == "override game/a.xex\n");
}

REX_TEST_CASE("An init module entry loads back with its guest path", "[codegen]") {
  rex::test::TempFolder folder;
  rex::codegen::TemplateRegistry registry;
  const nlohmann::json data = {{"m", ModuleEntry()}};
  const auto manifest = rex::codegen::ManifestConfig::Load(
      WriteProject(folder, registry.Render("init/manifest_module", data.dump())));
  REQUIRE(manifest);
  const auto modules = ModulesOf(manifest);
  REQUIRE(modules.size() == 1);
  REQUIRE(modules[0].guest_path == "game/a.xex");

  rex::test::TempFolder full_folder;
  Touch(full_folder.path / "default.xex");
  fs::create_directories(full_folder.path / "game");
  Touch(full_folder.path / "game" / "a.xex");
  const nlohmann::json full = {
      {"names", {{"original", "demo"}}},
      {"sdk_version", "1.0"},
      {"sdk_version_full", "1.0.0"},
      {"generated_on", "2026-10-11"},
      {"include_stamp", false},
      {"xex_path", "default.xex"},
      {"game_root", ""},
      {"out_directory_path", "generated"},
      {"modules", nlohmann::json::array({ModuleEntry()})},
  };
  const auto full_path = full_folder.path / "demo_manifest.toml";
  std::ofstream(full_path) << registry.Render("init/manifest_toml", full.dump());
  const auto full_manifest = rex::codegen::ManifestConfig::Load(full_path);
  REQUIRE(full_manifest);
  const auto full_modules = ModulesOf(full_manifest);
  REQUIRE(full_modules.size() == 1);
  REQUIRE(full_modules[0].guest_path == "game/a.xex");
}

REX_TEST_CASE("A manifest keeps the top level keys as a shared layer", "[codegen]") {
  rex::test::TempFolder folder;
  Touch(folder.path / "default.xex");
  const auto path = folder.path / "demo_manifest.toml";
  std::ofstream(path) << R"(skip_lr = true
longjmp_address = 0x82001000
[functions]
0x82001000 = { name = "shared_fn" }
)" << kProject;
  const auto manifest = rex::codegen::ManifestConfig::Load(path);
  if (!manifest) {
    FAIL("the manifest did not load");
    return;
  }
  CHECK(manifest->shared.functions.contains(0x82001000));
  CHECK(manifest->shared.skip_lr);
  CHECK(manifest->entrypoint.recompiler.skip_lr);
  CHECK(manifest->entrypoint.recompiler.functions.empty());
  CHECK(manifest->entrypoint.recompiler.longjmp_address == 0);
}

REX_TEST_CASE("A manifest reads the hook source folders", "[codegen]") {
  rex::test::TempFolder folder;
  Touch(folder.path / "default.xex");
  const auto path = folder.path / "demo_manifest.toml";
  std::ofstream(path) << "[project]\nname = \"demo\"\nhook_sources = [\"src\", \"hooks/extra\"]\n"
                         "[entrypoint]\nfile_path = \"default.xex\"\n";
  const auto manifest = rex::codegen::ManifestConfig::Load(path);
  REQUIRE(manifest);
  CHECK(manifest.value_or(rex::codegen::ManifestConfig{}).hook_sources ==
        std::vector<std::string>{"src", "hooks/extra"});
}

REX_TEST_CASE("A manifest without hook source folders has none", "[codegen]") {
  rex::test::TempFolder folder;
  const auto manifest = rex::codegen::ManifestConfig::Load(WriteProject(folder, ""));
  REQUIRE(manifest);
  CHECK(manifest.value_or(rex::codegen::ManifestConfig{}).hook_sources.empty());
}

REX_TEST_CASE("A manifest rejects a hook source entry that is not a folder string", "[codegen]") {
  const char* const bad[] = {"hook_sources = \"src\"\n", "hook_sources = [1]\n",
                             "hook_sources = [\"\"]\n"};
  for (const char* line : bad) {
    rex::test::TempFolder folder;
    Touch(folder.path / "default.xex");
    const auto path = folder.path / "demo_manifest.toml";
    std::ofstream(path) << "[project]\nname = \"demo\"\n"
                        << line << "[entrypoint]\nfile_path = \"default.xex\"\n";
    INFO(line);
    CHECK_FALSE(rex::codegen::ManifestConfig::Load(path));
  }
}

REX_TEST_CASE("The init manifest scans the src folder for hooks", "[codegen]") {
  rex::test::TempFolder folder;
  Touch(folder.path / "default.xex");
  const nlohmann::json data = {
      {"names", {{"original", "demo"}}},
      {"sdk_version", "1.0"},
      {"sdk_version_full", "1.0.0"},
      {"generated_on", "2026-10-11"},
      {"include_stamp", true},
      {"xex_path", "default.xex"},
      {"game_root", ""},
      {"out_directory_path", "generated"},
      {"modules", nlohmann::json::array()},
  };
  rex::codegen::TemplateRegistry registry;
  const auto path = folder.path / "demo_manifest.toml";
  std::ofstream(path) << registry.Render("init/manifest_toml", data.dump());
  const auto manifest = rex::codegen::ManifestConfig::Load(path);
  REQUIRE(manifest);
  CHECK(manifest.value_or(rex::codegen::ManifestConfig{}).hook_sources ==
        std::vector<std::string>{"src"});
}

namespace {

bool Mentions(const std::vector<std::string>& warnings, std::string_view text) {
  return std::ranges::any_of(warnings, [text](const std::string& warning) {
    return warning.find(text) != std::string::npos;
  });
}

fs::path WriteKeyed(const rex::test::TempFolder& folder, std::string_view project_keys,
                    std::string_view entrypoint_keys, std::string_view module_keys,
                    std::string_view shared_keys) {
  Touch(folder.path / "default.xex");
  fs::create_directories(folder.path / "game");
  Touch(folder.path / "game" / "a.xex");
  const auto path = folder.path / "demo_manifest.toml";
  std::ofstream(path) << shared_keys << "\n[project]\nname = \"demo\"\n"
                      << project_keys << "\n[entrypoint]\nfile_path = \"default.xex\"\n"
                      << entrypoint_keys
                      << "\n[[modules]]\nguest_path = \"game/a.xex\"\nfile_path = \"game/a.xex\"\n"
                      << module_keys;
  return path;
}

}  // namespace

REX_TEST_CASE("An unknown project key warns with the file", "[codegen][keys]") {
  rex::test::TempFolder folder;
  const auto manifest =
      rex::codegen::ManifestConfig::Load(WriteKeyed(folder, "game_rooot = \"x\"\n", "", "", ""));
  REQUIRE(manifest);
  CHECK(Mentions(manifest->warnings, "game_rooot"));
  CHECK(Mentions(manifest->warnings, "demo_manifest.toml"));
}

REX_TEST_CASE("A mistyped project key warns", "[codegen][keys]") {
  rex::test::TempFolder folder;
  const auto manifest =
      rex::codegen::ManifestConfig::Load(WriteKeyed(folder, "sdk_version = 5\n", "", "", ""));
  REQUIRE(manifest);
  CHECK(Mentions(manifest->warnings, "sdk_version"));
  CHECK(Mentions(manifest->warnings, "string"));
}

REX_TEST_CASE("An unknown entrypoint or module key warns on that binary", "[codegen][keys]") {
  rex::test::TempFolder folder;
  const auto manifest = rex::codegen::ManifestConfig::Load(
      WriteKeyed(folder, "", "skip_lrr = true\n", "ctr_as_locall = true\n", ""));
  REQUIRE(manifest);
  CHECK(Mentions(manifest->entrypoint.recompiler.load_warnings, "skip_lrr"));
  CHECK(Mentions(manifest->entrypoint.recompiler.load_warnings, "[entrypoint]"));
  CHECK_FALSE(Mentions(manifest->entrypoint.recompiler.load_warnings, "ctr_as_locall"));
  REQUIRE(manifest->modules.size() == 1);
  CHECK(Mentions(manifest->modules[0].recompiler.load_warnings, "ctr_as_locall"));
  CHECK(Mentions(manifest->modules[0].recompiler.load_warnings, "[[modules]]"));
}

REX_TEST_CASE("A shared layer warning is reported once", "[codegen][keys]") {
  rex::test::TempFolder folder;
  const auto manifest =
      rex::codegen::ManifestConfig::Load(WriteKeyed(folder, "", "", "", "skip_msrr = true\n"));
  REQUIRE(manifest);
  CHECK(Mentions(manifest->shared.load_warnings, "skip_msrr"));
  CHECK_FALSE(Mentions(manifest->entrypoint.recompiler.load_warnings, "skip_msrr"));
  CHECK_FALSE(Mentions(manifest->modules[0].recompiler.load_warnings, "skip_msrr"));
}

REX_TEST_CASE("A manifest with only known keys has no warnings", "[codegen][keys]") {
  rex::test::TempFolder folder;
  const auto manifest = rex::codegen::ManifestConfig::Load(
      WriteKeyed(folder, "sdk_version = \"1.0.0\"\nhook_sources = []\n", "", "", ""));
  REQUIRE(manifest);
  CHECK(manifest->warnings.empty());
  CHECK(manifest->shared.load_warnings.empty());
  CHECK(manifest->entrypoint.recompiler.load_warnings.empty());
  CHECK(manifest->modules[0].recompiler.load_warnings.empty());
}
