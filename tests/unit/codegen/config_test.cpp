/**
 * @file        tests/unit/codegen/config_test.cpp
 * @brief       Unit tests for RecompilerConfig include-based config layering
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/codegen/config.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static fs::path WriteTempToml(const fs::path& dir, const std::string& name,
                              const std::string& content) {
  auto path = dir / name;
  fs::create_directories(path.parent_path());
  std::ofstream(path) << content;
  return path;
}

// Produce a chain of N TOML files where each file includes the next.
// Returns the path to the first file in the chain (depth 0).
static fs::path WriteIncludeChain(const fs::path& dir, uint32_t length) {
  // Write the leaf (no includes), then work backwards.
  for (uint32_t i = length - 1; i > 0; --i) {
    std::string next = "chain_" + std::to_string(i) + ".toml";
    std::string content = "includes = [\"" + next +
                          "\"]\n"
                          "file_path = \"dummy.xex\"\n";
    WriteTempToml(dir, "chain_" + std::to_string(i - 1) + ".toml", content);
  }
  // Leaf file
  WriteTempToml(dir, "chain_" + std::to_string(length - 1) + ".toml",
                "file_path = \"dummy.xex\"\n");
  return dir / "chain_0.toml";
}

// ---------------------------------------------------------------------------
// Test 1: Scalar override -- last (top-level) wins
// ---------------------------------------------------------------------------

TEST_CASE("Config scalar override - last wins", "[codegen][config]") {
  auto tmp = fs::temp_directory_path() / "rex_cfg_scalar";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // base.toml sets project_name and file_path
  WriteTempToml(tmp, "base.toml",
                "project_name = \"base_project\"\n"
                "file_path = \"base.xex\"\n"
                "skip_lr = false\n");

  // top.toml includes base, then overrides project_name and skip_lr
  WriteTempToml(tmp, "top.toml",
                "includes = [\"base.toml\"]\n"
                "project_name = \"top_project\"\n"
                "file_path = \"base.xex\"\n"
                "skip_lr = true\n");

  rex::codegen::RecompilerConfig cfg;
  REQUIRE(cfg.Load((tmp / "top.toml").string()));

  // Top-level values must win
  CHECK(cfg.projectName == "top_project");
  CHECK(cfg.skipLr == true);
  // file_path from base is not re-overridden by top (top also sets it)
  CHECK(cfg.filePath == "base.xex");

  fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 2: Collection merge -- additive
// ---------------------------------------------------------------------------

TEST_CASE("Config collection merge - additive", "[codegen][config]") {
  auto tmp = fs::temp_directory_path() / "rex_cfg_additive";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // base.toml defines one function
  WriteTempToml(tmp, "base.toml",
                "file_path = \"game.xex\"\n"
                "\n"
                "[functions.\"82200000\"]\n"
                "size = 0x100\n"
                "name = \"BaseFunc\"\n");

  // top.toml includes base and adds a second distinct function
  WriteTempToml(tmp, "top.toml",
                "includes = [\"base.toml\"]\n"
                "file_path = \"game.xex\"\n"
                "\n"
                "[functions.\"82200200\"]\n"
                "size = 0x80\n"
                "name = \"TopFunc\"\n");

  rex::codegen::RecompilerConfig cfg;
  REQUIRE(cfg.Load((tmp / "top.toml").string()));

  // Both functions must be present
  REQUIRE(cfg.functions.count(0x82200000u) == 1);
  REQUIRE(cfg.functions.count(0x82200200u) == 1);
  CHECK(cfg.functions.at(0x82200000u).name == "BaseFunc");
  CHECK(cfg.functions.at(0x82200200u).name == "TopFunc");

  fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 3: Keyed conflict -- last wins
// ---------------------------------------------------------------------------

TEST_CASE("Config keyed conflict - last wins", "[codegen][config]") {
  auto tmp = fs::temp_directory_path() / "rex_cfg_conflict";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // included.toml defines a function at address 0x82210000
  WriteTempToml(tmp, "included.toml",
                "file_path = \"game.xex\"\n"
                "\n"
                "[functions.\"82210000\"]\n"
                "size = 0x200\n"
                "name = \"OldName\"\n");

  // top.toml includes the above, then redefines the SAME address
  WriteTempToml(tmp, "top.toml",
                "includes = [\"included.toml\"]\n"
                "file_path = \"game.xex\"\n"
                "\n"
                "[functions.\"82210000\"]\n"
                "size = 0x400\n"
                "name = \"NewName\"\n");

  rex::codegen::RecompilerConfig cfg;
  REQUIRE(cfg.Load((tmp / "top.toml").string()));

  // Only one entry for the address; top-level definition wins
  REQUIRE(cfg.functions.count(0x82210000u) == 1);
  CHECK(cfg.functions.at(0x82210000u).name == "NewName");
  CHECK(cfg.functions.at(0x82210000u).size == 0x400u);

  fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 4: Array-of-table dedup -- same address, last wins
// ---------------------------------------------------------------------------

TEST_CASE("Config switch_tables dedup - last wins", "[codegen][config]") {
  auto tmp = fs::temp_directory_path() / "rex_cfg_switchtable";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // first.toml defines a switch_table at bctrAddress 0x82220000 with 2 labels
  WriteTempToml(tmp, "first.toml",
                "file_path = \"game.xex\"\n"
                "\n"
                "[[switch_tables]]\n"
                "address = 0x82220000\n"
                "register = 3\n"
                "labels = [0x82220100, 0x82220200]\n");

  // top.toml includes first, then redefines the SAME bctrAddress with 3 labels
  WriteTempToml(tmp, "top.toml",
                "includes = [\"first.toml\"]\n"
                "file_path = \"game.xex\"\n"
                "\n"
                "[[switch_tables]]\n"
                "address = 0x82220000\n"
                "register = 4\n"
                "labels = [0x82220100, 0x82220200, 0x82220300]\n");

  rex::codegen::RecompilerConfig cfg;
  REQUIRE(cfg.Load((tmp / "top.toml").string()));

  // One entry at the address; top-level definition wins
  REQUIRE(cfg.switchTables.count(0x82220000u) == 1);
  const auto& jt = cfg.switchTables.at(0x82220000u);
  CHECK(jt.indexRegister == 4);
  REQUIRE(jt.targets.size() == 3);
  CHECK(jt.targets[2] == 0x82220300u);

  fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 5: Circular include detection -> Load() returns false
// ---------------------------------------------------------------------------

TEST_CASE("Config circular include detection", "[codegen][config]") {
  auto tmp = fs::temp_directory_path() / "rex_cfg_circular";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // a.toml includes b.toml
  WriteTempToml(tmp, "a.toml",
                "file_path = \"game.xex\"\n"
                "includes = [\"b.toml\"]\n");

  // b.toml includes a.toml (cycle)
  WriteTempToml(tmp, "b.toml",
                "file_path = \"game.xex\"\n"
                "includes = [\"a.toml\"]\n");

  rex::codegen::RecompilerConfig cfg;
  CHECK_FALSE(cfg.Load((tmp / "a.toml").string()));

  fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 6: Max depth enforcement (chain of 33 non-cyclic files -> false)
// ---------------------------------------------------------------------------

TEST_CASE("Config max include depth enforcement", "[codegen][config]") {
  // kMaxIncludeDepth == 32; a chain of 34 files has depth 33 at the leaf.
  constexpr uint32_t kChainLength = 34;

  auto tmp = fs::temp_directory_path() / "rex_cfg_maxdepth";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  fs::path root = WriteIncludeChain(tmp, kChainLength);

  rex::codegen::RecompilerConfig cfg;
  CHECK_FALSE(cfg.Load(root.string()));

  fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 7: Recursive includes happy path
// ---------------------------------------------------------------------------

TEST_CASE("Config recursive includes happy path", "[codegen][config]") {
  auto tmp = fs::temp_directory_path() / "rex_cfg_recursive";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // leaf.toml: lowest-level config
  WriteTempToml(tmp, "leaf.toml",
                "file_path = \"game.xex\"\n"
                "project_name = \"leaf_project\"\n"
                "\n"
                "[functions.\"82230000\"]\n"
                "size = 0x80\n"
                "name = \"LeafFunc\"\n");

  // mid.toml: includes leaf, adds another function
  WriteTempToml(tmp, "mid.toml",
                "includes = [\"leaf.toml\"]\n"
                "file_path = \"game.xex\"\n"
                "\n"
                "[functions.\"82230100\"]\n"
                "size = 0x80\n"
                "name = \"MidFunc\"\n");

  // top.toml: includes mid, adds yet another function and overrides project_name
  WriteTempToml(tmp, "top.toml",
                "includes = [\"mid.toml\"]\n"
                "file_path = \"game.xex\"\n"
                "project_name = \"top_project\"\n"
                "\n"
                "[functions.\"82230200\"]\n"
                "size = 0x80\n"
                "name = \"TopFunc\"\n");

  rex::codegen::RecompilerConfig cfg;
  REQUIRE(cfg.Load((tmp / "top.toml").string()));

  // All three functions must be present
  CHECK(cfg.functions.count(0x82230000u) == 1);
  CHECK(cfg.functions.count(0x82230100u) == 1);
  CHECK(cfg.functions.count(0x82230200u) == 1);

  // Top-level project_name overrides leaf's value
  CHECK(cfg.projectName == "top_project");

  fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Test 8: Relative path resolution
// ---------------------------------------------------------------------------

TEST_CASE("Config relative path resolution from subdirectory", "[codegen][config]") {
  auto tmp = fs::temp_directory_path() / "rex_cfg_relpath";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "sub");

  // sub/leaf.toml: lives in a subdirectory
  WriteTempToml(tmp, "sub/leaf.toml",
                "file_path = \"game.xex\"\n"
                "\n"
                "[functions.\"82240000\"]\n"
                "size = 0xC0\n"
                "name = \"SubFunc\"\n");

  // top.toml: includes sub/leaf.toml using a relative path
  WriteTempToml(tmp, "top.toml",
                "includes = [\"sub/leaf.toml\"]\n"
                "file_path = \"game.xex\"\n");

  rex::codegen::RecompilerConfig cfg;
  REQUIRE(cfg.Load((tmp / "top.toml").string()));

  REQUIRE(cfg.functions.count(0x82240000u) == 1);
  CHECK(cfg.functions.at(0x82240000u).name == "SubFunc");

  // Now test that an included file resolves its OWN includes relative to itself.
  // Create: top2.toml -> subdir/mid.toml -> ../leaf2.toml (back up to tmp root)
  WriteTempToml(tmp, "leaf2.toml",
                "file_path = \"game.xex\"\n"
                "\n"
                "[functions.\"82240100\"]\n"
                "size = 0xC0\n"
                "name = \"Leaf2Func\"\n");

  // sub/mid.toml includes the parent directory's leaf2.toml
  WriteTempToml(tmp, "sub/mid.toml",
                "includes = [\"../leaf2.toml\"]\n"
                "file_path = \"game.xex\"\n");

  WriteTempToml(tmp, "top2.toml",
                "includes = [\"sub/mid.toml\"]\n"
                "file_path = \"game.xex\"\n");

  rex::codegen::RecompilerConfig cfg2;
  REQUIRE(cfg2.Load((tmp / "top2.toml").string()));

  REQUIRE(cfg2.functions.count(0x82240100u) == 1);
  CHECK(cfg2.functions.at(0x82240100u).name == "Leaf2Func");

  fs::remove_all(tmp);
}
