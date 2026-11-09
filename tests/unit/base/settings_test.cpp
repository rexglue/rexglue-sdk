/**
 * @file        tests/unit/base/settings_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/console_command.h>

#include "base/app_harness.h"

namespace {

using rex::SettingLayer;
using rex::test::HarnessApp;
using rex::test::RunInInit;

class AudioOwner final : public rex::SettingOwner {
 public:
  AudioOwner() : SettingOwner("audio") {}
  REX_SETTING(rex::f64, gain, 1.0, "Gain", rex::Range{0.0, 4.0});
  REX_SETTING(rex::i32, count, 3, "Count");
  REX_SETTING(std::string, mode, "fast", "Mode", rex::Allowed("fast", "slow"));
  REX_SETTING(bool, flag, false, "Flag");
};

class AliasNamed final : public rex::SettingOwner {
 public:
  AliasNamed() : SettingOwner("bind") {}
  REX_SETTING(bool, debug, false, "Debug");
};

class LifeOwner final : public rex::SettingOwner {
 public:
  LifeOwner() : SettingOwner("life") {}
  REX_SETTING(rex::i32, startup, 1, "Startup", rex::kStartupOnly);
  REX_SETTING(rex::i32, launch, 1, "Until launch", rex::kUntilLaunch);
  REX_SETTING(rex::i32, restart, 5, "Restart", rex::kRestart);
  REX_SETTING(rex::i32, live, 1, "Live");
};

class PathOwner final : public rex::SettingOwner {
 public:
  PathOwner() : SettingOwner("tpath") {}
  REX_SETTING(std::filesystem::path, root, "/a", "Root");
  REX_SETTING(std::filesystem::path, log_root, "{tpath.root}/logs", "Logs");

  size_t CountOwned() const { return GetOwnedSettings().size(); }
  bool NamesRegistry() {
    const rex::SettingsRegistry* first = &REX_SETTINGS();
    return first == &REX_SETTINGS();
  }
};

std::filesystem::path TempFile(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

}  // namespace

REX_TEST_CASE("settings: value for a later owner applies on register", "[base][settings]") {
  RunInInit([] {
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kFile, "audio.gain", "2.0"));
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.gain.Get() == 2.0);
    REQUIRE(audio.gain.GetLayer() == SettingLayer::kFile);
  });
}

REX_TEST_CASE("settings: alias maps an old name", "[base][settings]") {
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REX_SETTINGS().AddAlias("apu.old", "audio.gain");
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kFile, "apu.old", "0.5"));
    REQUIRE(audio.gain.Get() == 0.5);
  });
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> lines;
  sink.CopySince(0, lines);
  int notices = 0;
  for (const auto& line : lines) {
    notices += line.text.find("apu.old") != std::string::npos;
  }
  REQUIRE(notices == 1);
}

REX_TEST_CASE("a setting cannot take a name that is an alias", "[base][settings]") {
  RunInInit([] {
    AliasNamed named;
    auto refused = REX_SETTINGS().Register(named);
    REQUIRE_FALSE(refused);
    const std::string message = refused.error().GetDescription();
    REQUIRE(message.find("bind.debug") != std::string::npos);
    REQUIRE(message.find("ui.bind.debug") != std::string::npos);

    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REX_SETTINGS().AddAlias("apu.renamed", "audio.gain");
    REQUIRE(REX_SETTINGS().Find("audio.gain"));
  });
}

REX_TEST_CASE("settings: startup only refuses after freeze", "[base][settings]") {
  LifeOwner life;
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry& settings) -> rex::Result<void> {
    REX_TRY_VOID(settings.Register(life));
    REQUIRE(life.startup.Set(2));
    return rex::Ok();
  };
  app.init = [&]() -> rex::Result<void> {
    REQUIRE(life.startup.IsFrozen());
    REQUIRE_FALSE(life.startup.Set(3));
    REQUIRE(life.startup.Get() == 2);
    REQUIRE(life.live.Set(9));
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("settings: until launch writes at the title layer", "[base][settings]") {
  RunInInit([] {
    LifeOwner life;
    REQUIRE(REX_SETTINGS().Register(life));
    REQUIRE(life.launch.Set(6));
    REQUIRE(life.launch.GetLayer() == SettingLayer::kTitle);
    REQUIRE(life.live.Set(4));
    REQUIRE(life.live.Get() == 4);
  });
}

REX_TEST_CASE("settings: restart stages the value", "[base][settings]") {
  RunInInit([] {
    LifeOwner life;
    REQUIRE(REX_SETTINGS().Register(life));
    REQUIRE(life.restart.Set(3));
    REQUIRE(life.restart.Get() == 5);
    REQUIRE(life.restart.GetText() == "3");
    REQUIRE(life.restart.IsRestartPending());
    const auto pending = REX_SETTINGS().GetRestartPending();
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0] == "life.restart");
  });
}

REX_TEST_CASE("settings: lock wins over runtime", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.count.Lock(1));
    REQUIRE_FALSE(audio.count.SetText(SettingLayer::kRuntime, "2"));
    REQUIRE_FALSE(audio.count.Reset());
    REQUIRE(audio.count.Get() == 1);
    REQUIRE(audio.count.GetLayer() == SettingLayer::kLock);
  });
}

REX_TEST_CASE("settings: higher layers win", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kCommandLine, "audio.count", "30"));
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kFile, "audio.count", "20"));
    REQUIRE(audio.count.Get() == 30);
    REQUIRE_FALSE(audio.count.SetText(SettingLayer::kTitle, "10"));
    REQUIRE(audio.count.SetText(SettingLayer::kRuntime, "40"));
    REQUIRE(audio.count.Get() == 40);
  });
}

REX_TEST_CASE("settings: path template resolves at read", "[base][settings]") {
  RunInInit([] {
    PathOwner paths;
    REQUIRE(REX_SETTINGS().Register(paths));
    REQUIRE(paths.log_root.GetText() == "{tpath.root}/logs");
    REQUIRE(paths.log_root.Get() == std::filesystem::path("/a/logs"));
    REQUIRE(paths.root.Set("/b"));
    REQUIRE(paths.log_root.Get() == std::filesystem::path("/b/logs"));
  });
}

REX_TEST_CASE("settings: owner name does not hide the registry", "[base][settings]") {
  PathOwner paths;
  REQUIRE(paths.NamesRegistry());
  REQUIRE(paths.CountOwned() == 2);
}

REX_TEST_CASE("settings: range, allowed texts and types are checked", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE_FALSE(audio.gain.Set(4.5));
    REQUIRE_FALSE(audio.gain.Set(-0.1));
    REQUIRE(audio.gain.Set(4.0));
    REQUIRE_FALSE(audio.mode.Set("medium"));
    REQUIRE(audio.mode.Set("slow"));
    REQUIRE_FALSE(REX_SETTINGS().SetText("audio.count", "twelve"));
    REQUIRE_FALSE(REX_SETTINGS().SetText("audio.flag", "maybe"));
    REQUIRE(REX_SETTINGS().SetText("audio.flag", "true"));
    REQUIRE(audio.flag.Get());
    REQUIRE(REX_SETTINGS().SetText("audio.nothing", "1").error().category ==
            rex::ErrorCategory::kNotFound);
  });
}

REX_TEST_CASE("settings: a change watch runs until it is destroyed", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    int calls = 0;
    rex::i32 last = 0;
    {
      auto watch = audio.count.OnChange([&](const rex::i32& value) {
        ++calls;
        last = value;
      });
      REQUIRE(audio.count.Set(3));
      REQUIRE(audio.count.Set(4));
      REQUIRE(calls == 2);
      REQUIRE(last == 4);
    }
    REQUIRE(audio.count.Set(5));
    REQUIRE(calls == 2);
  });
}

REX_TEST_CASE("settings: save writes changed settings and load reads them back",
              "[base][settings]") {
  const auto file = TempFile("rex_settings_save_test.toml");
  RunInInit([&] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.count.Set(12));
    REQUIRE(audio.mode.Set("slow"));
    REQUIRE(audio.gain.Set(0.75));
    REQUIRE(REX_SETTINGS().Save(file));
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    REQUIRE(text.str().find("count = 12") != std::string::npos);
    REQUIRE(text.str().find("flag") == std::string::npos);
  });
  AudioOwner audio;
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry& settings) -> rex::Result<void> {
    REX_TRY_VOID(settings.Register(audio));
    REQUIRE(audio.count.Get() == 3);
    REX_TRY_VOID(settings.Load(file));
    REQUIRE(audio.count.Get() == 12);
    REQUIRE(audio.mode.Get() == "slow");
    REQUIRE(audio.gain.Get() == 0.75);
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
  std::filesystem::remove(file);
}

REX_TEST_CASE("settings: a duplicate full name is refused and names both owners",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner first;
    AudioOwner second;
    REQUIRE(REX_SETTINGS().Register(first));
    auto result = REX_SETTINGS().Register(second);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message.find("audio.gain") != std::string::npos);
  });
}

REX_TEST_CASE("settings: a command runs through the registry", "[base][settings]") {
  class WithCommand final : public rex::SettingOwner {
   public:
    WithCommand() : SettingOwner("cmdowner") {}
    REX_COMMAND(echo, "Echoes", [this](std::string_view args) {
      text = std::string(args);
      return rex::Result<void>();
    });
    std::string text;
  };
  RunInInit([] {
    WithCommand owner;
    REQUIRE(REX_SETTINGS().Register(owner));
    REQUIRE(REX_SETTINGS().Run("cmdowner.echo", "hello"));
    REQUIRE(owner.text == "hello");
    REQUIRE_FALSE(REX_SETTINGS().Run("cmdowner.nothing", ""));
  });
}
