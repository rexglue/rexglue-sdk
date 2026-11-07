/**
 * @file        tests/unit/kernel/title_launch_test.cpp
 * @brief       Title launch attaches every system and unload detaches them
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/app/app.h>
#include <rex/apu/audio_system.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/driver.h>
#include <rex/app_info.h>
#include <rex/kernel/export.h>
#include <rex/kernel/title_runtime.h>
#include <rex/setting.h>

#include "kernel/title_entries.h"
#include "kernel/title_fakes.h"
#include "kernel/title_image.h"
#include "kernel/title_state.h"
#include "temp_folder.h"
#include "test_memory.h"
#include "ui/ui_harness.h"
#include <rex/kernel/types/content.h>

namespace {

using namespace std::chrono_literals;
using rex::test::CountingProvider;
using rex::test::g_counts;
using rex::test::g_pad_revision;
using rex::test::g_pads;
using rex::test::MakePad;
using rex::test::UnloadWithin;
using rex::test::WaitFor;

class LaunchApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  LaunchApp() : ObjectApp("launch_test") { rex::test::WriteTitle(folder_.path / "game"); }

  std::function<void(LaunchApp&)> body;

  std::string audio_driver = "count";

  std::exception_ptr failure;

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::vector<std::string>{})); }

  int Finish() {
    const int code = Shutdown();
    if (failure) {
      std::rethrow_exception(failure);
    }
    return code;
  }

 protected:
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs;
    needs.graphics_family = rex::GraphicsFamily::kXenos;
    return needs;
  }

  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
  }

  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    (void)settings.SetText(rex::SettingLayer::kTitle, "kernel.user_language", "0");
    (void)settings.SetText(rex::SettingLayer::kTitle, "title.game_root",
                           (folder_.path / "game").string());
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "count");
    (void)GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "count");
    (void)GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, audio_driver);
    return settings.SetText(rex::SettingLayer::kTitle, "gpu.driver", "count");
  }

  rex::Result<void> OnInit() override {
    try {
      if (body) {
        body(*this);
      }
    } catch (...) {
      failure = std::current_exception();
      return rex::Err(rex::ErrorCategory::kRuntime, "the test body threw");
    }
    return rex::Ok();
  }

 private:
  rex::test::TempFolder folder_{"rex_title_launch_"};
  CountingProvider provider_;
};

struct Fixture {
  Fixture() {
    rex::test::ReleaseTestMemory();
    g_counts.Reset();
    g_pads.clear();
    g_pad_revision = 1;
    rex::test::g_entry.Reset();
  }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "launch attaches every system", "[kernel][title]") {
  LaunchApp app;
  app.body = [](LaunchApp&) {
    rex::kernel::TitleRuntime title(rex::test::MakeTitleImage(rex::test::kReturningEntry));
    REQUIRE(title.Load());
    CHECK(g_counts.graphics_attach == 0);
    REQUIRE(title.Launch());
    CHECK(title.IsRunning());
    CHECK(g_counts.graphics_attach == 1);
    CHECK(WaitFor([] { return rex::test::g_entry.finished == 1; }));
    title.Unload();
    CHECK(g_counts.graphics_detach == 1);
    CHECK_FALSE(title.IsLoaded());
    CHECK_FALSE(title.IsRunning());
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "a language of 0 follows the host at launch", "[kernel][title]") {
  LaunchApp app;
  app.body = [](LaunchApp&) {
    rex::test_ui::g_ui.locales = {{"da", "DK"}, {"ja", "JP"}};
    rex::kernel::TitleRuntime title(rex::test::MakeTitleImage(rex::test::kReturningEntry));
    REQUIRE(title.Load());
    CHECK(title.GetUserLanguage() == rex::kernel::XLanguage::kEnglish);
    REQUIRE(title.Launch());
    CHECK(title.GetUserLanguage() == rex::kernel::XLanguage::kJapanese);
    title.Unload();
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "unload ends a guest thread blocked in a wait", "[kernel][title]") {
  LaunchApp app;
  app.body = [](LaunchApp&) {
    rex::kernel::TitleRuntime title(rex::test::MakeTitleImage(rex::test::kDelayingEntry));
    REQUIRE(title.Load());
    REQUIRE(title.Launch());
    REQUIRE(WaitFor([] { return rex::test::g_entry.entered == 1; }));
    UnloadWithin(title, 5s);
    CHECK(g_counts.graphics_detach == 1);
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "a title launches again after unload with a waiting guest thread",
                     "[kernel][title]") {
  LaunchApp app;
  app.body = [](LaunchApp&) {
    rex::kernel::TitleRuntime title(rex::test::MakeTitleImage(rex::test::kDelayingEntry));
    for (int round = 1; round <= 2; ++round) {
      REQUIRE(title.Load());
      REQUIRE(title.Launch());
      REQUIRE(WaitFor([&] { return rex::test::g_entry.entered == round; }));
      UnloadWithin(title, 5s);
      CHECK(g_counts.graphics_attach == round);
      CHECK(g_counts.graphics_detach == round);
    }
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "unload ends a guest thread waiting on an event nobody signals",
                     "[kernel][title]") {
  LaunchApp app;
  app.body = [](LaunchApp&) {
    rex::kernel::TitleRuntime title(rex::test::MakeTitleImage(rex::test::kEventEntry));
    rex::u32 first_handle = 0;
    for (int round = 1; round <= 2; ++round) {
      REQUIRE(title.Load());
      REQUIRE(title.Launch());
      REQUIRE(WaitFor(
          [&] { return rex::test::g_entry.entered == round && rex::test::g_entry.handle != 0; }));
      UnloadWithin(title, 5s);
      CHECK_FALSE(title.IsLoaded());
      if (round == 1) {
        first_handle = rex::test::g_entry.handle;
      } else {
        CHECK(rex::test::g_entry.handle == first_handle);
      }
      rex::test::g_entry.handle = 0;
    }
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "device change notification sent once", "[kernel][title]") {
  LaunchApp app;
  app.body = [](LaunchApp& self) {
    rex::kernel::TitleRuntime title(rex::test::MakeTitleImage(rex::test::kListeningEntry));
    REQUIRE(title.Load());
    REQUIRE(title.Launch());
    REQUIRE(WaitFor([] { return rex::test::g_entry.listening.load(); }));
    REQUIRE(WaitFor([] { return rex::test::g_entry.devices_changed >= 2; }));
    const int baseline = rex::test::g_entry.devices_changed;
    CHECK(baseline == 2);
    auto settles_at = [&self, baseline](int expected_changes) {
      const int expected = baseline + expected_changes;
      REQUIRE(WaitFor([&] {
        (void)self.GetInput().GetState(0);
        return rex::test::g_entry.devices_changed == expected;
      }));
      CHECK(rex::test::g_entry.devices_changed == expected);
    };
    g_pads = {MakePad(1, 0)};
    ++g_pad_revision;
    settles_at(1);
    g_pads.clear();
    ++g_pad_revision;
    settles_at(2);
    UnloadWithin(title, 5s);
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

namespace {
rex::u32 ReadSpeakerConfig(LaunchApp& app, rex::u32 expected_channels) {
  CHECK(app.GetAudio().GetOutputChannels() == expected_channels);
  rex::kernel::TitleRuntime title(rex::test::MakeTitleImage(rex::test::kSpeakerEntry));
  REQUIRE(title.Load());
  REQUIRE(title.Launch());
  CHECK(StateOf(title).GetAPU() != nullptr);
  CHECK(WaitFor([] { return rex::test::g_entry.finished == 1; }));
  title.Unload();
  return rex::test::g_entry.speaker_config;
}
}  // namespace

REX_TEST_CASE_METHOD(Fixture, "the speaker config is surround for a 6 channel device",
                     "[kernel][title]") {
  LaunchApp app;
  g_counts.audio_channels = 6;
  app.body = [](LaunchApp& launched) { CHECK(ReadSpeakerConfig(launched, 6) == 0x00010001); };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "the speaker config is analog surround for a stereo device",
                     "[kernel][title]") {
  LaunchApp app;
  g_counts.audio_channels = 2;
  app.body = [](LaunchApp& launched) { CHECK(ReadSpeakerConfig(launched, 2) == 0x00000001); };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "the speaker config is analog surround with no audio driver",
                     "[kernel][title]") {
  LaunchApp app;
  app.audio_driver = "";
  app.body = [](LaunchApp& launched) { CHECK(ReadSpeakerConfig(launched, 2) == 0x00000001); };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}
