/**
 * @file        tests/unit/codegen/template_registry_test.cpp
 * @brief       Unit tests for TemplateRegistry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/codegen/template_registry.h>

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

// Helper to create a temp directory for test overrides
static fs::path CreateTempDir() {
  auto tmp = fs::temp_directory_path() / "rex_template_test";
  fs::create_directories(tmp);
  return tmp;
}

static void CleanupTempDir(const fs::path& dir) {
  fs::remove_all(dir);
}

static void WriteTempFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream f(path);
  f << content;
}

TEST_CASE("TemplateRegistry: registeredIds returns all 11 template IDs", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  auto ids = registry.registeredIds();

  // Should have all 11 templates
  REQUIRE(ids.size() == 11);

  // Check specific IDs exist
  auto has = [&](const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  CHECK(has("init/cmakelists"));
  CHECK(has("init/cmake_presets"));
  CHECK(has("init/main_cpp"));
  CHECK(has("init/app_header"));
  CHECK(has("init/config_toml"));
  CHECK(has("init/rexglue_cmake"));
  CHECK(has("codegen/config_h"));
  CHECK(has("codegen/init_h"));
  CHECK(has("codegen/init_cpp"));
  CHECK(has("codegen/config_cpp"));
  CHECK(has("codegen/sources_cmake"));
}

TEST_CASE("TemplateRegistry: render with simple CLI data", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  std::string json = R"({"names": {"snake_case": "test_app"}, "sdk_version": "1.0.0"})";
  std::string result = registry.render("init/config_toml", json);

  CHECK(result.find("project_name = \"test_app\"") != std::string::npos);
  CHECK(result.find("test_app") != std::string::npos);
}

TEST_CASE("TemplateRegistry: render with codegen data", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  std::string json = R"({
    "project": "test_proj",
    "image_base": "0x82000000",
    "image_size": "0x1000000",
    "code_base": "0x82010000",
    "code_size": "0x100000",
    "rexcrt_heap": 1,
    "config_flags": {},
    "functions": [],
    "imports": []
  })";

  std::string result = registry.render("codegen/config_cpp", json);
  CHECK(result.find("PPCImageConfig") != std::string::npos);
  CHECK(result.find("test_proj") != std::string::npos);
}

TEST_CASE("TemplateRegistry: render unknown ID throws TemplateError", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  REQUIRE_THROWS_AS(registry.render("nonexistent/template_id", "{}"), rex::codegen::TemplateError);
}

TEST_CASE("TemplateRegistry: cmake_var callback works", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  std::string result = registry.renderString("{{ cmake_var(\"FOO\") }}", "{}");
  CHECK(result == "${FOO}");
}

TEST_CASE("TemplateRegistry: loadOverrides with valid override", "[TemplateRegistry]") {
  auto tmpDir = CreateTempDir();

  // Write a custom init/config_toml.inja override
  WriteTempFile(tmpDir / "init" / "config_toml.inja", "custom_override = true\n");

  rex::codegen::TemplateRegistry registry;
  registry.loadOverrides(tmpDir);

  std::string result = registry.render("init/config_toml", "{}");
  CHECK(result.find("custom_override = true") != std::string::npos);
  // Should NOT contain the default template content
  CHECK(result.find("project_name") == std::string::npos);

  CleanupTempDir(tmpDir);
}

TEST_CASE("TemplateRegistry: loadOverrides ignores unknown IDs", "[TemplateRegistry]") {
  auto tmpDir = CreateTempDir();

  // Write a file with unrecognized canonical ID
  WriteTempFile(tmpDir / "unknown" / "template.inja", "should be ignored\n");

  rex::codegen::TemplateRegistry registry;
  // Should not throw
  REQUIRE_NOTHROW(registry.loadOverrides(tmpDir));

  // Unknown template should still not be renderable
  REQUIRE_THROWS_AS(registry.render("unknown/template", "{}"), rex::codegen::TemplateError);

  CleanupTempDir(tmpDir);
}
