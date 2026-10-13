/**
 * @file        tests/unit/core/config_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"

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

std::filesystem::path TempFile(const rex::test::TempFolder& folder, std::string_view name,
                               std::string_view content) {
  auto path = folder.path / name;
  std::ofstream(path, std::ios::binary) << content;
  return path;
}

}  // namespace

namespace file_cvars {
REXCVAR_BOOL(gpu, vsync, true, "vsync");
REXCVAR_STRING(gpu, path, "a", "a string");
REXCVAR_TABLE(kFileCvars, vsync, path);
}  // namespace file_cvars

REX_TEST_CASE("ParseCommandLine reads every cvar form", "[core][config]") {
  rex::Config cfg;
  auto rest =
      Parse(cfg, {"--gpu.vsync=false", "--gpu.scale", "2", "--gpu.debug", "--no-gpu.markers"});
  REQUIRE(rest.empty());
  REQUIRE(cfg.cvars.values.size() == 4);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.scale")->value == "2");
  REQUIRE(Find(cfg, "gpu.debug")->value == "true");
  REQUIRE(Find(cfg, "gpu.markers")->value == "false");
  REQUIRE(Find(cfg, "gpu.vsync")->source == rex::cvar::CvarSource::kCommandLine);
}

REX_TEST_CASE("ParseCommandLine fills structural fields and modules", "[core][config]") {
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

REX_TEST_CASE("The --modules flag appends to the configured list", "[core][config]") {
  rex::Config cfg;
  cfg.modules = {"x"};
  auto rest = Parse(cfg, {"--modules=a", "--modules", "b,c"});
  REQUIRE(rest.empty());
  REQUIRE(cfg.modules == std::vector<std::string>{"x", "a", "b", "c"});
}

REX_TEST_CASE("ParseCommandLine returns unknown options in order", "[core][config]") {
  rex::Config cfg;
  auto rest =
      Parse(cfg, {"game_dir", "--foo", "--foo=1", "--Bad.Name=1", "--gpu.vsync", "--modules"});
  REQUIRE(rest == std::vector<std::string>{"--foo", "--foo=1", "--Bad.Name=1", "--modules"});
  REQUIRE(Find(cfg, "gpu.vsync")->value == "true");
}

REX_TEST_CASE("ParseCommandLine treats a following -- argument as a bool value boundary",
              "[core][config]") {
  rex::Config cfg;
  auto rest = Parse(cfg, {"--gpu.vsync", "--gpu.scale=2"});
  REQUIRE(rest.empty());
  REQUIRE(Find(cfg, "gpu.vsync")->value == "true");
  REQUIRE(Find(cfg, "gpu.scale")->value == "2");
}

REX_TEST_CASE("LoadConfigFile reads scoped tables and flat keys alike", "[core][config]") {
  const rex::test::TempFolder folder("rex_config_test_");
  auto path = TempFile(folder, "rex_config_test_1.toml",
                       "fs.path = \"C:\\\\x\"\n\n[gpu]\nvsync = false\nscale = 2\n");
  rex::Config cfg;
  REQUIRE(rex::LoadConfigFile(cfg, path));
  REQUIRE(cfg.cvars.values.size() == 3);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.vsync")->source == rex::cvar::CvarSource::kConfig);
  REQUIRE(Find(cfg, "gpu.scale")->value == "2");
  REQUIRE(Find(cfg, "fs.path")->value == "C:\\x");
}

REX_TEST_CASE("LoadConfigFile keeps a dotless key as an override", "[core][config]") {
  const rex::test::TempFolder folder("rex_config_test_");
  auto path = TempFile(folder, "rex_config_test_2.toml", "vsync = false\n");
  rex::Config cfg;
  REQUIRE(rex::LoadConfigFile(cfg, path));
  REQUIRE(cfg.cvars.values.size() == 1);
  REQUIRE(Find(cfg, "vsync")->value == "false");
}

REX_TEST_CASE("LoadConfigFile rejects nested tables and malformed files, accepts a missing file",
              "[core][config]") {
  rex::Config cfg;
  const rex::test::TempFolder missing_folder("rex_config_test_");
  REQUIRE(rex::LoadConfigFile(cfg, missing_folder.path / "missing.toml"));
  REQUIRE(cfg.cvars.values.empty());
  const rex::test::TempFolder folder("rex_config_test_");
  auto nested = TempFile(folder, "rex_config_test_3.toml", "[ui.window]\nwidth = 3\n");
  auto status = rex::LoadConfigFile(cfg, nested);
  REQUIRE_FALSE(status);
  REQUIRE(status.error().message.find("ui.window.width") != std::string::npos);
  auto broken = TempFile(folder, "rex_config_test_4.toml", "[gpu\nvsync = ");
  auto parse = rex::LoadConfigFile(cfg, broken);
  REQUIRE_FALSE(parse);
  REQUIRE(parse.error().message.find("rex_config_test_4.toml") != std::string::npos);
}

REX_TEST_CASE("SaveConfigFile round trips through LoadConfigFile", "[core][config]") {
  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("gpu", file_cvars::kFileCvars));
  REQUIRE(registry->Set("gpu.vsync", "false"));
  REQUIRE(registry->Set("gpu.path", "C:\\x \"y\""));
  const rex::test::TempFolder folder("rex_config_test_");
  const auto path = folder.path / "round_trip.toml";
  REQUIRE(rex::SaveConfigFile(*registry, path));
  rex::Config cfg;
  REQUIRE(rex::LoadConfigFile(cfg, path));
  REQUIRE(cfg.cvars.values.size() == 2);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.path")->value == "C:\\x \"y\"");
  REQUIRE(file_cvars::vsync.Parse("true"));
  REQUIRE(file_cvars::path.Parse("a"));
}

REX_TEST_CASE("ApplyEnvironment reads REX_ variables into Environment overrides",
              "[core][config]") {
  rex::platform::SetEnvironmentValue("REX_GPU_VSYNC", "false");
  rex::platform::SetEnvironmentValue("REX_APP_DEBUG_MARKERS", "1");
  rex::platform::SetEnvironmentValue("REX_NOSCOPE", "x");
  rex::platform::SetEnvironmentValue("REXX_FOO", "x");
  rex::Config cfg;
  rex::cvar::ApplyEnvironment(cfg);
  REQUIRE(Find(cfg, "gpu.vsync") != nullptr);
  REQUIRE(Find(cfg, "gpu.vsync")->value == "false");
  REQUIRE(Find(cfg, "gpu.vsync")->source == rex::cvar::CvarSource::kEnvironment);
  REQUIRE(Find(cfg, "app.debug_markers")->value == "1");
  REQUIRE(Find(cfg, "noscope") == nullptr);
  for (const auto& o : cfg.cvars.values)
    REQUIRE(o.name.find("foo") == std::string::npos);
  rex::platform::UnsetEnvironmentValue("REX_GPU_VSYNC");
  rex::platform::UnsetEnvironmentValue("REX_APP_DEBUG_MARKERS");
  rex::platform::UnsetEnvironmentValue("REX_NOSCOPE");
  rex::platform::UnsetEnvironmentValue("REXX_FOO");
}

REX_TEST_CASE("CvarConfig::Get returns the highest precedence queued value", "[core][config]") {
  rex::Config config;
  config.cvars.values.push_back({"gpu.vsync", "false", rex::cvar::CvarSource::kEnvironment});
  config.cvars.SetDefault("gpu.vsync", "true");
  config.cvars.values.push_back({"gpu.vsync", "true", rex::cvar::CvarSource::kConfig});
  REQUIRE(config.cvars.Get("gpu.vsync") == "false");
  config.cvars.Set("gpu.vsync", "true");
  REQUIRE(config.cvars.Get("gpu.vsync") == "true");
  REQUIRE_FALSE(config.cvars.Get("gpu.nothing").has_value());
}

REX_TEST_CASE("Config defaults for the 6a fields", "[core][config]") {
  rex::Config config;
  REQUIRE(config.single_instance.empty());
  REQUIRE(config.runtime.terminate_timeout == std::chrono::milliseconds(2000));
  REQUIRE_FALSE(config.hid.shared_pads);
  REQUIRE(config.ui.aspect_ratio == rex::ui::AspectRatio::kStretch);
  REQUIRE(config.ui.min_size.width_px == 0);
  REQUIRE(config.ui.title.empty());
}
