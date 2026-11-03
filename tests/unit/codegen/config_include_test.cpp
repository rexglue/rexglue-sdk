/**
 * @file        tests/unit/codegen/config_include_test.cpp
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
#include <string>

#include "codegen/config.h"

namespace fs = std::filesystem;
using rex::codegen::RecompilerConfig;
using rex::test::WriteText;

namespace {

constexpr const char* kBase = "file_path = \"game.xex\"\nout_directory_path = \"out\"\n";

}  // namespace

REX_TEST_CASE("An include that appears twice in one list loads", "[codegen][config]") {
  rex::test::TempFolder folder("rexglue_include_twice_");
  WriteText(folder.path / "common.toml", "skip_lr = true\n");
  WriteText(folder.path / "main.toml",
            std::string(kBase) + "includes = [\"common.toml\", \"common.toml\"]\n");
  RecompilerConfig config;
  REQUIRE(config.Load((folder.path / "main.toml").string()));
  CHECK(config.skip_lr);
}

REX_TEST_CASE("Two includes that share an include load", "[codegen][config]") {
  rex::test::TempFolder folder("rexglue_include_diamond_");
  WriteText(folder.path / "common.toml", "skip_msr = true\n");
  WriteText(folder.path / "left.toml", "includes = [\"common.toml\"]\nskip_lr = true\n");
  WriteText(folder.path / "right.toml", "includes = [\"common.toml\"]\nctr_as_local = true\n");
  WriteText(folder.path / "main.toml",
            std::string(kBase) + "includes = [\"left.toml\", \"right.toml\"]\n");
  RecompilerConfig config;
  REQUIRE(config.Load((folder.path / "main.toml").string()));
  CHECK(config.skip_lr);
  CHECK(config.skip_msr);
  CHECK(config.ctr_as_local_variable);
}

REX_TEST_CASE("A shared include is listed once in the loaded files", "[codegen][config]") {
  rex::test::TempFolder folder("rexglue_include_listed_");
  WriteText(folder.path / "common.toml", "skip_lr = true\n");
  WriteText(folder.path / "main.toml",
            std::string(kBase) + "includes = [\"common.toml\", \"common.toml\"]\n");
  RecompilerConfig config;
  REQUIRE(config.Load((folder.path / "main.toml").string()));
  const auto common = std::ranges::count_if(config.loaded_files, [](const std::string& file) {
    return fs::path(file).filename() == "common.toml";
  });
  CHECK(common == 1);
}

REX_TEST_CASE("An include cycle fails", "[codegen][config]") {
  rex::test::TempFolder folder("rexglue_include_cycle_");
  WriteText(folder.path / "a.toml", "includes = [\"b.toml\"]\n");
  WriteText(folder.path / "b.toml", "includes = [\"a.toml\"]\n");
  WriteText(folder.path / "main.toml", std::string(kBase) + "includes = [\"a.toml\"]\n");
  RecompilerConfig config;
  CHECK_FALSE(config.Load((folder.path / "main.toml").string()));
}

REX_TEST_CASE("A file that includes itself fails", "[codegen][config]") {
  rex::test::TempFolder folder("rexglue_include_self_");
  WriteText(folder.path / "main.toml", std::string(kBase) + "includes = [\"main.toml\"]\n");
  RecompilerConfig config;
  CHECK_FALSE(config.Load((folder.path / "main.toml").string()));
}
