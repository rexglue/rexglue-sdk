/**
 * @file        tests/unit/core/config_test.cpp
 * @brief       Unit tests for ParseCommandLine, LoadConfigFile, SaveConfigFile and ApplyEnvironment
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/api.h>
#include <rex/platform/env.h>

#include "core/cvar_registry.h"

namespace {

std::vector<std::string> Parse(rex::Config& cfg, std::vector<std::string> args) {
  std::vector<char*> argv;
  std::string exe = "app.exe";
  argv.push_back(exe.data());
  for (auto& a : args)
    argv.push_back(a.data());
  return rex::ParseCommandLine(
      cfg, rex::CLIArgList::Parse(static_cast<rex::i32>(argv.size()), argv.data()));
}

const rex::CvarOverride* Find(const rex::Config& cfg, std::string_view name) {
  for (const auto& o : cfg.cvars.values) {
    if (o.name == name)
      return &o;
  }
  return nullptr;
}

std::filesystem::path TempFile(std::string_view name, std::string_view content) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream(path, std::ios::binary) << content;
  return path;
}

}  // namespace

namespace file_cvars {
REXCVAR_BOOL(gpu, vsync, true, "vsync");
REXCVAR_STRING(gpu, path, "a", "a string");
REXCVAR_TABLE(kFileCvars, vsync, path);
}  // namespace file_cvars

TEST_CASE("ParseCommandLine reads every cvar form", "[config]") {
  rex::Config cfg;
  auto rest =
      Parse(cfg, {"--gpu.vsync=false", "--gpu.scale", "2", "--gpu.debug", "--no-gpu.markers"});
  REQUIRE(rest.empty());
  REQUIRE(cfg.cvars.values.size() == 4);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.scale")->value == "2");
  REQUIRE(Find(cfg, "gpu.debug")->value == "true");
  REQUIRE(Find(cfg, "gpu.markers")->value == "false");
  REQUIRE(Find(cfg, "gpu.vsync")->source == rex::cvar::CvarSource::CommandLine);
}

TEST_CASE("ParseCommandLine fills structural fields and modules", "[config]") {
  rex::Config cfg;
  auto rest = Parse(cfg, {"--game_data_root=X:/game", "--cache_root", "X:/cache", "--backend",
                          "vulkan", "--modules", "a,b", "--config_file=X:/app.toml"});
  REQUIRE(rest.empty());
  REQUIRE(cfg.paths.game_data_root == "X:/game");
  REQUIRE(cfg.paths.cache_root == "X:/cache");
  REQUIRE(cfg.paths.config_file == "X:/app.toml");
  REQUIRE(cfg.gpu.backend == "vulkan");
  REQUIRE(cfg.modules == std::vector<std::string>{"a", "b"});
  REQUIRE(cfg.cvars.values.empty());
}

TEST_CASE("--modules appends to the configured list", "[config]") {
  rex::Config cfg;
  cfg.modules = {"x"};
  auto rest = Parse(cfg, {"--modules=a", "--modules", "b,c"});
  REQUIRE(rest.empty());
  REQUIRE(cfg.modules == std::vector<std::string>{"x", "a", "b", "c"});
}

TEST_CASE("ParseCommandLine returns unknown options in order", "[config]") {
  rex::Config cfg;
  auto rest =
      Parse(cfg, {"game_dir", "--foo", "--foo=1", "--Bad.Name=1", "--gpu.vsync", "--modules"});
  REQUIRE(rest == std::vector<std::string>{"--foo", "--foo=1", "--Bad.Name=1", "--modules"});
  REQUIRE(Find(cfg, "gpu.vsync")->value == "true");
}

TEST_CASE("ParseCommandLine treats a following -- argument as a bool value boundary", "[config]") {
  rex::Config cfg;
  auto rest = Parse(cfg, {"--gpu.vsync", "--gpu.scale=2"});
  REQUIRE(rest.empty());
  REQUIRE(Find(cfg, "gpu.vsync")->value == "true");
  REQUIRE(Find(cfg, "gpu.scale")->value == "2");
}

TEST_CASE("LoadConfigFile reads scoped tables and flat keys alike", "[config]") {
  auto path = TempFile("rex_config_test_1.toml",
                       "fs.path = \"C:\\\\x\"\n\n[gpu]\nvsync = false\nscale = 2\n");
  rex::Config cfg;
  REQUIRE(rex::LoadConfigFile(cfg, path));
  REQUIRE(cfg.cvars.values.size() == 3);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.vsync")->source == rex::cvar::CvarSource::Config);
  REQUIRE(Find(cfg, "gpu.scale")->value == "2");
  REQUIRE(Find(cfg, "fs.path")->value == "C:\\x");
  std::filesystem::remove(path);
}

TEST_CASE("LoadConfigFile keeps a dotless key as an override", "[config]") {
  auto path = TempFile("rex_config_test_2.toml", "vsync = false\n");
  rex::Config cfg;
  REQUIRE(rex::LoadConfigFile(cfg, path));
  REQUIRE(cfg.cvars.values.size() == 1);
  REQUIRE(Find(cfg, "vsync")->value == "false");
  std::filesystem::remove(path);
}

TEST_CASE("LoadConfigFile rejects nested tables and malformed files, accepts a missing file",
          "[config]") {
  rex::Config cfg;
  REQUIRE(rex::LoadConfigFile(
      cfg, std::filesystem::temp_directory_path() / "rex_config_test_missing.toml"));
  REQUIRE(cfg.cvars.values.empty());
  auto nested = TempFile("rex_config_test_3.toml", "[ui.window]\nwidth = 3\n");
  auto status = rex::LoadConfigFile(cfg, nested);
  REQUIRE_FALSE(status);
  REQUIRE(status.error().message.find("ui.window.width") != std::string::npos);
  std::filesystem::remove(nested);
  auto broken = TempFile("rex_config_test_4.toml", "[gpu\nvsync = ");
  auto parse = rex::LoadConfigFile(cfg, broken);
  REQUIRE_FALSE(parse);
  REQUIRE(parse.error().message.find("rex_config_test_4.toml") != std::string::npos);
  std::filesystem::remove(broken);
}

TEST_CASE("SaveConfigFile round trips through LoadConfigFile", "[config]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("gpu", file_cvars::kFileCvars));
  REQUIRE(registry->Set("gpu.vsync", "false"));
  REQUIRE(registry->Set("gpu.path", "C:\\x \"y\""));
  auto path = std::filesystem::temp_directory_path() / "rex_config_test_5.toml";
  REQUIRE(rex::SaveConfigFile(*registry, path));
  rex::Config cfg;
  REQUIRE(rex::LoadConfigFile(cfg, path));
  REQUIRE(cfg.cvars.values.size() == 2);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.path")->value == "C:\\x \"y\"");
  std::filesystem::remove(path);
  REQUIRE(file_cvars::vsync.Parse("true"));
  REQUIRE(file_cvars::path.Parse("a"));
}

TEST_CASE("ApplyEnvironment reads REX_ variables into Environment overrides", "[config]") {
  rex::platform::SetEnvironmentValue("REX_GPU_VSYNC", "false");
  rex::platform::SetEnvironmentValue("REX_APP_DEBUG_MARKERS", "1");
  rex::platform::SetEnvironmentValue("REX_NOSCOPE", "x");
  rex::platform::SetEnvironmentValue("REXX_FOO", "x");
  rex::Config cfg;
  rex::cvar::ApplyEnvironment(cfg);
  REQUIRE(Find(cfg, "gpu.vsync") != nullptr);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.vsync")->source == rex::cvar::CvarSource::Environment);
  REQUIRE(Find(cfg, "app.debug_markers")->value == "1");
  REQUIRE(Find(cfg, "noscope") == nullptr);
  for (const auto& o : cfg.cvars.values)
    REQUIRE(o.name.find("foo") == std::string::npos);
  rex::platform::UnsetEnvironmentValue("REX_GPU_VSYNC");
  rex::platform::UnsetEnvironmentValue("REX_APP_DEBUG_MARKERS");
  rex::platform::UnsetEnvironmentValue("REX_NOSCOPE");
  rex::platform::UnsetEnvironmentValue("REXX_FOO");
}

TEST_CASE("CvarConfig::Get returns the highest precedence queued value", "[config]") {
  rex::Config config;
  config.cvars.values.push_back({"gpu.vsync", "false", rex::cvar::CvarSource::Environment});
  config.cvars.SetDefault("gpu.vsync", "true");
  config.cvars.values.push_back({"gpu.vsync", "true", rex::cvar::CvarSource::Config});
  REQUIRE(config.cvars.Get("gpu.vsync") == "false");
  config.cvars.Set("gpu.vsync", "true");
  REQUIRE(config.cvars.Get("gpu.vsync") == "true");
  REQUIRE_FALSE(config.cvars.Get("gpu.nothing").has_value());
}

TEST_CASE("Config defaults for the 6a fields", "[config]") {
  rex::Config config;
  REQUIRE(config.single_instance.empty());
  REQUIRE(config.runtime.terminate_timeout == std::chrono::milliseconds(2000));
  REQUIRE_FALSE(config.input.shared_pads);
  REQUIRE(config.ui.aspect_ratio == rex::ui::AspectRatio::kStretch);
  REQUIRE(config.ui.min_size.width_px == 0);
  REQUIRE(config.ui.title.empty());
}
