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
#include "support/temp_folder.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rex/codegen/manifest.h>
#include <rex/codegen/template_registry.h>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kProject =
    "[project]\nname = \"demo\"\n[entrypoint]\nfile_path = \"default.xex\"\n\n";

nlohmann::json ModuleData() {
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
  REQUIRE(modules[0].guestPath == "game/a.xex");
}

REX_TEST_CASE("The init manifest writes each module's guest path", "[codegen]") {
  const nlohmann::json data = {
      {"names", {{"original", "demo"}}},
      {"sdk_version", "1.0"},
      {"sdk_version_full", "1.0.0"},
      {"generated_on", "2026-10-11"},
      {"include_stamp", false},
      {"xex_path", "default.xex"},
      {"game_root", ""},
      {"out_directory_path", "generated"},
      {"modules", nlohmann::json::array({ModuleData()})},
  };
  rex::codegen::TemplateRegistry registry;
  const auto text = registry.render("init/manifest_toml", data.dump());
  REQUIRE(text.find("guest_path = \"game/a.xex\"") != std::string::npos);
}

REX_TEST_CASE("An init module entry loads back with its guest path", "[codegen]") {
  rex::test::TempFolder folder;
  rex::codegen::TemplateRegistry registry;
  const nlohmann::json data = {{"m", ModuleData()}};
  const auto manifest = rex::codegen::ManifestConfig::Load(
      WriteProject(folder, registry.render("init/manifest_module", data.dump())));
  REQUIRE(manifest);
  const auto modules = ModulesOf(manifest);
  REQUIRE(modules.size() == 1);
  REQUIRE(modules[0].guestPath == "game/a.xex");
}
