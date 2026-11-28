/**
 * @file        tests/unit/base/settings_feed_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <rex/platform/process.h>
#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "base/app_harness.h"

namespace {

using rex::SettingLayer;
using rex::test::HarnessApp;

class EnvironmentValue {
 public:
  EnvironmentValue(std::string name, std::string_view value) : name_(std::move(name)) {
    saved_ = rex::platform::GetEnvironmentValue(name_);
    rex::platform::SetEnvironmentValue(name_, value);
  }
  ~EnvironmentValue() {
    if (saved_) {
      rex::platform::SetEnvironmentValue(name_, *saved_);
    } else {
      rex::platform::UnsetEnvironmentValue(name_);
    }
  }

  EnvironmentValue(const EnvironmentValue&) = delete;
  EnvironmentValue& operator=(const EnvironmentValue&) = delete;

 private:
  std::string name_;
  std::optional<std::string> saved_;
};

std::string WriteFile(const HarnessApp& app, std::string_view text) {
  const auto path = app.folder / "feed.toml";
  std::ofstream(path, std::ios::binary) << text;
  return "--config_file=" + path.string();
}

size_t CountWarnings(rex::log::LogCaptureSink& sink, std::string_view text) {
  std::vector<rex::log::CapturedMessage> lines;
  sink.CopySince(0, lines);
  return static_cast<size_t>(std::ranges::count_if(lines, [&](const auto& line) {
    return line.level == rex::log::Level::kWarn && line.text.contains(text);
  }));
}

}  // namespace

REX_TEST_CASE("file value reaches a setting", "[base][settings_feed]") {
  HarnessApp app;
  app.init = [&]() -> rex::Result<void> {
    CHECK(REX_LOGGER().level.Get() == "debug");
    CHECK(REX_LOGGER().level.GetLayer() == SettingLayer::kFile);
    CHECK(REX_SETTINGS().Find("clock.no_scaling").value()->GetText() == "true");
    return rex::Ok();
  };
  const auto arg = WriteFile(app, "[log]\nlevel = \"debug\"\n\n[clock]\nno_scaling = true\n");
  REQUIRE(app.Start({arg}));
  app.Finish();
}

REX_TEST_CASE("a nested file table reaches a setting", "[base][settings_feed]") {
  HarnessApp app;
  app.init = []() -> rex::Result<void> {
    auto found = REX_SETTINGS().Find("log.categories.core");
    REQUIRE(found);
    CHECK(found.value()->GetText() == "warn");
    return rex::Ok();
  };
  const auto arg = WriteFile(app, "[log.categories]\ncore = \"warn\"\n");
  REQUIRE(app.Start({arg}));
  app.Finish();
}

REX_TEST_CASE("command line beats environment beats file", "[base][settings_feed]") {
  HarnessApp app;
  const EnvironmentValue env("REX_LOG_LEVEL", "warn");
  app.init = []() -> rex::Result<void> {
    CHECK(REX_LOGGER().level.Get() == "error");
    CHECK(REX_LOGGER().level.GetLayer() == SettingLayer::kCommandLine);
    return rex::Ok();
  };
  const auto arg = WriteFile(app, "[log]\nlevel = \"debug\"\n");
  REQUIRE(app.Start({arg, "--log.level=error"}));
  app.Finish();
}

REX_TEST_CASE("environment beats file", "[base][settings_feed]") {
  HarnessApp app;
  const EnvironmentValue env("REX_LOG_LEVEL", "warn");
  app.init = []() -> rex::Result<void> {
    CHECK(REX_LOGGER().level.Get() == "warn");
    CHECK(REX_LOGGER().level.GetLayer() == SettingLayer::kEnvironment);
    return rex::Ok();
  };
  const auto arg = WriteFile(app, "[log]\nlevel = \"debug\"\n");
  REQUIRE(app.Start({arg}));
  app.Finish();
}

REX_TEST_CASE("a setting on the command line is not an unknown option", "[base][settings_feed]") {
  HarnessApp app;
  app.init = []() -> rex::Result<void> {
    CHECK(REX_SETTINGS().Find("log.categories.core").value()->GetText() == "error");
    CHECK_FALSE(REX_LOGGER().color.Get());
    return rex::Ok();
  };
  REQUIRE(app.Start({"--log.categories.core=error", "--no-log.color"}));
  app.Finish();
}

REX_TEST_CASE("a config file key holding an array is ignored with a warning",
              "[base][settings_feed]") {
  HarnessApp app;
  const auto arg = WriteFile(app, "[log]\ncategories = [1, 2]\nlevel = \"debug\"\n");
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REQUIRE(app.Start({arg}));
  CHECK(REX_LOGGER().level.Get() == "debug");
  app.Finish();
  sink_guard.Reset();
  CHECK(CountWarnings(sink, "key 'log.categories' is an array") == 1);
}

REX_TEST_CASE("a no flag on a setting that is not a bool warns and keeps the value",
              "[base][settings_feed]") {
  HarnessApp app;
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REQUIRE(app.Start({"--no-log.level"}));
  CHECK(REX_LOGGER().level.GetLayer() == SettingLayer::kDefault);
  app.Finish();
  sink_guard.Reset();
  CHECK(CountWarnings(sink, "--no-log.level needs a bool setting") == 1);
}

REX_TEST_CASE("a refused setting value warns and keeps the value", "[base][settings_feed]") {
  HarnessApp app;
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REQUIRE(app.Start({"--log.level=loud"}));
  CHECK(REX_LOGGER().level.Get() != "loud");
  CHECK(REX_LOGGER().level.GetLayer() == SettingLayer::kDefault);
  app.Finish();
  sink_guard.Reset();
  CHECK(CountWarnings(sink, "loud") == 1);
}

REX_TEST_CASE("a config file that does not parse is ignored with a warning",
              "[base][settings_feed]") {
  HarnessApp app;
  const auto arg = WriteFile(app, "[log\nlevel = = debug\n");
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REQUIRE(app.Start({arg}));
  CHECK(REX_LOGGER().level.GetLayer() == SettingLayer::kDefault);
  app.Finish();
  sink_guard.Reset();
  CHECK(CountWarnings(sink, "feed.toml") == 1);
}

REX_TEST_CASE("unknown name reported once", "[base][settings_feed]") {
  HarnessApp app;
  const auto arg = WriteFile(
      app, "[nope]\nname = 1\n\n[log]\nlevel = \"debug\"\n\n[clock]\nno_scaling = true\n");
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REQUIRE(app.Start({arg}));
  app.Finish();
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> lines;
  sink.CopySince(0, lines);
  int unknown = 0;
  int known = 0;
  for (const auto& line : lines) {
    if (line.text.find("unknown setting") == std::string::npos) {
      continue;
    }
    unknown += line.text.find("nope.name") != std::string::npos;
    known += line.text.find("log.level") != std::string::npos ||
             line.text.find("clock.no_scaling") != std::string::npos;
  }
  CHECK(unknown == 1);
  CHECK(known == 0);
}
