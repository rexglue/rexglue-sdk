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

TEST_CASE("TemplateRegistry: registeredIds returns all template IDs", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  auto ids = registry.registeredIds();

  REQUIRE(ids.size() == 17);

  auto has = [&](const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  CHECK(has("init/cmakelists"));
  CHECK(has("init/cmake_presets"));
  CHECK(has("init/main_cpp"));
  CHECK(has("init/app_header"));
  CHECK(has("init/manifest_toml"));
  CHECK(has("init/rexglue_cmake"));
  CHECK(has("codegen/init_h"));
  CHECK(has("codegen/init_cpp"));
  CHECK(has("codegen/sources_cmake"));
  CHECK(has("codegen/_indirect_call"));
  CHECK(has("codegen/dll_targets_cmake"));
  CHECK(has("codegen/module_registry_cpp"));
  CHECK(has("codegen/register_cpp"));
  CHECK(has("test/ppc_config_h"));
  CHECK(has("test/ppc_test_cases_cpp"));
  CHECK(has("test/ppc_test_decls_h"));
  CHECK(has("test/ppc_test_functions_cpp"));
}

TEST_CASE("TemplateRegistry: render with simple CLI data", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  std::string json = R"({
    "names": {"snake_case": "test_app"},
    "sdk_version": "1.0.0",
    "include_stamp": false,
    "xex_path": "assets/default.xex",
    "out_directory_path": "generated/default"
  })";
  std::string result = registry.render("init/manifest_toml", json);

  CHECK(result.find("name = \"test_app\"") != std::string::npos);
  CHECK(result.find("[entrypoint]") != std::string::npos);
  CHECK(result.find("file_path = \"assets/default.xex\"") != std::string::npos);
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

  std::string result = registry.render("codegen/init_cpp", json);
  CHECK(result.find("PPCImageConfig") != std::string::npos);
  CHECK(result.find("test_proj") != std::string::npos);

  std::string dll_result = registry.render("codegen/register_cpp", R"({
    "project": "test_proj",
    "is_dll": true,
    "functions": []
  })");
  CHECK(dll_result.find("ReXModule_GetImageInfo") != std::string::npos);
}

TEST_CASE("TemplateRegistry: render unknown ID throws TemplateError", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  REQUIRE_THROWS_AS(registry.render("nonexistent/template_id", "{}"), rex::codegen::TemplateError);
}

TEST_CASE("TemplateRegistry: init_h includes shared indirect-call partial", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  std::string json = R"({
    "config_flags": {
      "skip_lr": false,
      "ctr_as_local": false,
      "xer_as_local": false,
      "reserved_as_local": false,
      "skip_msr": false,
      "cr_as_local": false,
      "non_argument_as_local": false,
      "non_volatile_as_local": false
    },
    "image_base": "0x82000000",
    "image_size": "0x1000000",
    "code_base": "0x82010000",
    "code_size": "0x100000",
    "thunk_reserve_size": "0x1000",
    "rexcrt_heap": false,
    "functions": [],
    "imports": []
  })";
  std::string result = registry.render("codegen/init_h", json);
  CHECK(result.find("REX_LOOKUP_FUNC") != std::string::npos);
  CHECK(result.find("ResolveIndirectFunction") != std::string::npos);
  CHECK(result.find("last_indirect_target") != std::string::npos);
  CHECK(result.find("REX_THUNK_RESERVE_SIZE") != std::string::npos);
  CHECK(result.find("[[likely]]") != std::string::npos);
  CHECK(result.find("[[unlikely]]") != std::string::npos);
}

TEST_CASE("TemplateRegistry: ppc_config_h includes shared indirect-call partial",
          "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  std::string json = R"({
    "image_base": "0x82000000",
    "image_size": "0x1000000",
    "code_base": "0x82010000",
    "code_size": "0x100000",
    "thunk_reserve_size": "0x1000"
  })";
  std::string result = registry.render("test/ppc_config_h", json);
  CHECK(result.find("ResolveIndirectFunction") != std::string::npos);
  CHECK(result.find("last_indirect_target") != std::string::npos);
  CHECK(result.find("[[likely]]") != std::string::npos);
}

TEST_CASE("TemplateRegistry: cmake_var callback works", "[TemplateRegistry]") {
  rex::codegen::TemplateRegistry registry;
  std::string result = registry.renderString("{{ cmake_var(\"FOO\") }}", "{}");
  CHECK(result == "${FOO}");
}

TEST_CASE("TemplateRegistry: loadOverrides with valid override", "[TemplateRegistry]") {
  auto tmpDir = CreateTempDir();

  // Write a custom init/manifest_toml.inja override
  WriteTempFile(tmpDir / "init" / "manifest_toml.inja", "custom_override = true\n");

  rex::codegen::TemplateRegistry registry;
  registry.loadOverrides(tmpDir);

  std::string result = registry.render("init/manifest_toml", "{}");
  CHECK(result.find("custom_override = true") != std::string::npos);
  // Should NOT contain the default template content
  CHECK(result.find("[entrypoint]") == std::string::npos);

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

TEST_CASE("Template: manifest_toml emits sdk_version when include_stamp is true",
          "[TemplateRegistry][manifest]") {
  rex::codegen::TemplateRegistry registry;
  std::string json =
      R"({"names": {"snake_case": "mygame", "pascal_case": "Mygame", "upper_case": "MYGAME"}, "sdk_version": "0.8.0", "include_stamp": true})";
  std::string out = registry.render("init/manifest_toml", json);
  CHECK(out.find("sdk_version = \"0.8.0\"") != std::string::npos);
  CHECK(out.find("name = \"mygame\"") != std::string::npos);
}

TEST_CASE("Template: manifest_toml omits sdk_version when include_stamp is false",
          "[TemplateRegistry][manifest]") {
  rex::codegen::TemplateRegistry registry;
  std::string json =
      R"({"names": {"snake_case": "mygame", "pascal_case": "Mygame", "upper_case": "MYGAME"}, "sdk_version": "0.8.0", "include_stamp": false})";
  std::string out = registry.render("init/manifest_toml", json);
  CHECK(out.find("sdk_version") == std::string::npos);
  CHECK(out.find("name = \"mygame\"") != std::string::npos);
}
