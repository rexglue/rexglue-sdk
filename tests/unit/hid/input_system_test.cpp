/**
 * @file        tests/unit/hid/input_system_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "input_harness.h"
#include "../ui/ui_harness.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/hid/input_keys.h>
#include <rex/hid/binding.h>
#include <rex/logging/logger.h>

using namespace rex::test_input;

namespace {

constexpr rex::hid::KeystrokeSource kAllSources =
    rex::hid::KeystrokeSource::kPad | rex::hid::KeystrokeSource::kKeyboard;

class WarningSink final : public rex::log::ILogSink {
 public:
  void Write(const rex::log::Message& message) override {
    std::lock_guard lock(mutex_);
    lines_.emplace_back(message.text);
  }

  void Flush() override {}

  std::vector<std::string> Take() {
    std::lock_guard lock(mutex_);
    return std::exchange(lines_, {});
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> lines_;
};

}  // namespace

REX_TEST_CASE("missing input driver keeps the others", "[hid]") {
  Run("nosuch,fake_a,fake_b", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    g_fake[1].devices = {MakePad(7, 1)};
    g_fake[1].states[7] = Buttons(rex::hid::kPadB);
    auto first = input.GetState(0);
    REQUIRE(first.has_value());
    CHECK(first->state.buttons == rex::hid::kPadA);
    auto second = input.GetState(1);
    REQUIRE(second.has_value());
    CHECK(second->state.buttons == rex::hid::kPadB);
  });
}

REX_TEST_CASE("one pad seen by two drivers appears once", "[hid]") {
  Run("fake_a,fake_b", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    g_fake[1].devices = {MakePad(7, 0)};
    g_fake[1].states[7] = Buttons(rex::hid::kPadB);
    auto first = input.GetState(0);
    REQUIRE(first.has_value());
    CHECK(first->state.buttons == rex::hid::kPadA);
    CHECK_FALSE(input.GetState(1).has_value());
    CHECK(input.GetState(1).error().category == rex::ErrorCategory::kNotFound);
  });
}

REX_TEST_CASE("a pad of the driver listed first wins", "[hid]") {
  Run("fake_b,fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    g_fake[1].devices = {MakePad(7, 0)};
    g_fake[1].states[7] = Buttons(rex::hid::kPadB);
    auto reading = input.GetState(0);
    REQUIRE(reading.has_value());
    CHECK(reading->state.buttons == rex::hid::kPadB);
  });
}

REX_TEST_CASE("two pads with ids reversed against the slots keep their users", "[hid]") {
  Run("fake_a,fake_b", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 1), MakePad(2, 0)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadB);
    g_fake[0].states[2] = Buttons(rex::hid::kPadA);
    g_fake[1].devices = {MakePad(5, 0), MakePad(6, 1)};
    g_fake[1].states[5] = Buttons(rex::hid::kPadX);
    g_fake[1].states[6] = Buttons(rex::hid::kPadY);
    auto user0 = input.GetState(0);
    auto user1 = input.GetState(1);
    REQUIRE(user0.has_value());
    REQUIRE(user1.has_value());
    CHECK(user0->state.buttons == rex::hid::kPadA);
    CHECK(user1->state.buttons == rex::hid::kPadB);
    CHECK_FALSE(input.GetState(2).has_value());
  });
}

REX_TEST_CASE("guide masked when off", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadGuide | rex::hid::kPadA);
    auto masked = input.GetState(0);
    REQUIRE(masked.has_value());
    CHECK(masked->state.buttons == rex::hid::kPadA);
    auto caps = input.GetCapabilities(0);
    REQUIRE(caps.has_value());
    CHECK((caps->supported.buttons & rex::hid::kPadGuide) == 0);

    REQUIRE(input.GetSettings().guide_button.Set(true));
    auto shown = input.GetState(0);
    REQUIRE(shown.has_value());
    CHECK(shown->state.buttons == (rex::hid::kPadGuide | rex::hid::kPadA));
    CHECK((input.GetCapabilities(0)->supported.buttons & rex::hid::kPadGuide) != 0);
  });
}

REX_TEST_CASE("keystrokes respect the UI block", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(0);
    CHECK_FALSE(input.GetKeystroke(0, kAllSources).has_value());

    input.AcquireUIBlock();
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    CHECK_FALSE(input.GetKeystroke(0, kAllSources).has_value());
    CHECK(input.GetState(0)->state.buttons == 0);

    input.ReleaseUIBlock();
    CHECK_FALSE(input.GetKeystroke(0, kAllSources).has_value());
    CHECK(input.GetState(0)->state.buttons == 0);

    g_fake[0].states[1] = Buttons(0);
    CHECK_FALSE(input.GetKeystroke(0, kAllSources).has_value());
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    auto pressed = input.GetKeystroke(0, kAllSources);
    REQUIRE(pressed.has_value());
    CHECK((pressed->flags & 0x0001) != 0);
    CHECK(pressed->user == 0);
  });
}

REX_TEST_CASE("any user takes a keystroke from the user that has one", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0), MakePad(2, 1), MakePad(3, 2)};
    g_fake[0].states[1] = Buttons(0);
    g_fake[0].states[2] = Buttons(0);
    g_fake[0].states[3] = Buttons(rex::hid::kPadStart);
    auto stroke = input.GetKeystroke(rex::hid::IInputSystem::kAnyUser, kAllSources);
    REQUIRE(stroke.has_value());
    CHECK(stroke->user == 2);
  });
}

REX_TEST_CASE("packet never goes back", "[hid]") {
  Run("fake_a,fake_b", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, -1)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    g_fake[1].devices = {MakePad(7, -1)};
    g_fake[1].states[7] = Buttons(rex::hid::kPadA);
    const u32 first = input.GetState(0)->packet;
    CHECK(input.GetState(0)->packet == first);

    g_fake[0].devices.clear();
    g_fake[0].states.clear();
    g_fake[0].revision++;
    const u32 second = input.GetState(0)->packet;
    CHECK(second > first);

    g_fake[0].devices = {MakePad(1, -1)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    g_fake[0].revision++;
    g_fake[1].devices.clear();
    g_fake[1].states.clear();
    g_fake[1].revision++;
    CHECK(input.GetState(0)->packet > second);
  });
}

REX_TEST_CASE("unplug sends one disconnect", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    std::vector<std::pair<u32, bool>> events;
    auto watch = input.OnConnectionChange(
        [&](u32 user, bool connected) { events.emplace_back(user, connected); });
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(0);
    REQUIRE(input.GetState(0).has_value());
    REQUIRE(events.size() == 1);
    CHECK(events[0] == std::pair<u32, bool>{0, true});

    g_fake[0].devices.clear();
    g_fake[0].states.clear();
    g_fake[0].revision++;
    REQUIRE(input.GetState(0).has_value());
    REQUIRE(input.GetState(0).has_value());
    REQUIRE(events.size() == 2);
    CHECK(events[1] == std::pair<u32, bool>{0, false});
  });
}

REX_TEST_CASE("idle pad answers user 0", "[hid]") {
  Run("none", [](rex::hid::IInputSystem& input) {
    auto reading = input.GetState(0);
    REQUIRE(reading.has_value());
    CHECK(reading->state.buttons == 0);
    CHECK(reading->state.thumb_lx == 0);
    CHECK_FALSE(input.GetState(1).has_value());
    CHECK_FALSE(input.GetPadInfo(0).has_value());
    CHECK(input.GetCapabilities(0).has_value());
    CHECK(input.SetVibration(0, {}).has_value());
  });
}

REX_TEST_CASE("connection callback holds no lock", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    bool answered = false;
    auto watch = input.OnConnectionChange([&](u32 user, bool) {
      (void)input.GetState(user);
      answered = true;
    });
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(0);
    REQUIRE(input.GetState(0).has_value());
    CHECK(answered);
  });
}

REX_TEST_CASE("a destroyed watch gets no more calls", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    int calls = 0;
    {
      auto watch = input.OnConnectionChange([&](u32, bool) { ++calls; });
    }
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(0);
    REQUIRE(input.GetState(0).has_value());
    CHECK(calls == 0);
  });
}

REX_TEST_CASE("vibration follows the setting", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(0);
    rex::hid::PadVibration motors;
    motors.left_motor = 100;
    motors.right_motor = 200;
    REQUIRE(input.SetVibration(0, motors).has_value());
    REQUIRE(g_fake[0].vibrations.size() == 1);
    CHECK(g_fake[0].vibrations[0].left_motor == 100);

    REQUIRE(input.GetSettings().vibration.Set(false));
    REQUIRE(input.SetVibration(0, motors).has_value());
    CHECK(g_fake[0].vibrations.back().left_motor == 0);
  });
}

REX_TEST_CASE("API serves the input system only while the app runs", "[hid]") {
  g_fake = {};
  InputApp app("none");
  CHECK(REX_INTERFACE(rex::hid::IInputSystem) == nullptr);
  REQUIRE(app.Start());
  CHECK(REX_INTERFACE(rex::hid::IInputSystem) == &app.GetInput());
  app.Finish();
  CHECK(REX_INTERFACE(rex::hid::IInputSystem) == nullptr);
}

REX_TEST_CASE("keyboard and mouse feed a pad when enabled", "[hid]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "none";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  REQUIRE(input.GetSettings().mnk_mode.Set(true));
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, rex::hid::VirtualKey::kW);
  app.Pump();
  CHECK(input.IsKeyDown(rex::hid::VirtualKey::kW));
  auto reading = input.GetState(0);
  REQUIRE(reading.has_value());
  CHECK(reading->state.thumb_ly == INT16_MAX);

  rex::test_ui::DeliverFocus(false);
  app.Pump();
  CHECK_FALSE(input.IsKeyDown(rex::hid::VirtualKey::kW));
  CHECK(input.GetState(0)->state.thumb_ly == 0);
  app.Shutdown();
}

REX_TEST_CASE("an unfocused window keeps the controller and drops the keyboard", "[hid]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "fake_a";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  g_fake[0].devices = {MakePad(1, 0)};
  g_fake[0].states[1] = Buttons(rex::hid::kPadA);
  CHECK(input.GetState(0)->state.buttons == rex::hid::kPadA);
  rex::test_ui::DeliverFocus(false);
  app.Pump();
  CHECK(input.GetState(0)->state.buttons == rex::hid::kPadA);
  app.Shutdown();
}

REX_TEST_CASE("an overlay that asks for exclusive input blocks the pad before any event", "[hid]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "fake_a";
  REQUIRE(app.Start());
  g_fake[0].devices = {MakePad(1, 0)};
  g_fake[0].states[1] = Buttons(rex::hid::kPadA);
  rex::hid::IInputSystem& input = app.GetInput();
  CHECK(input.GetState(0)->state.buttons == rex::hid::kPadA);
  app.GetUI().ShowOverlay("console", true);
  CHECK(input.GetState(0)->state.buttons == 0);
  app.GetUI().ShowOverlay("console", false);
  CHECK(input.GetState(0)->state.buttons == 0);
  g_fake[0].states[1] = Buttons(0);
  CHECK(input.GetState(0)->state.buttons == 0);
  g_fake[0].states[1] = Buttons(rex::hid::kPadA);
  CHECK(input.GetState(0)->state.buttons == rex::hid::kPadA);
  app.Shutdown();
}

REX_TEST_CASE("a passive overlay leaves the pad to the title", "[hid]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "fake_a";
  REQUIRE(app.Start());
  g_fake[0].devices = {MakePad(1, 0)};
  g_fake[0].states[1] = Buttons(rex::hid::kPadA);
  rex::hid::IInputSystem& input = app.GetInput();
  app.GetUI().ShowOverlay("debug", true);
  CHECK(input.GetState(0)->state.buttons == rex::hid::kPadA);
  app.GetUI().ShowOverlay("debug", false);
  app.Shutdown();
}

REX_TEST_CASE("dropping the guard removes the pointer mode listener", "[hid]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "none";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  int calls = 0;
  {
    auto listener_guard = input.SetPointerModeListener([&] { ++calls; });
    input.SetPointerMode(rex::hid::PointerMode::kLocked);
    CHECK(calls == 1);
  }
  input.SetPointerMode(rex::hid::PointerMode::kFree);
  CHECK(calls == 1);
  app.Shutdown();
}

REX_TEST_CASE("an old guard does not clear a newer pointer mode listener", "[hid]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "none";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  int first_calls = 0;
  int second_calls = 0;
  auto first = input.SetPointerModeListener([&] { ++first_calls; });
  auto second = input.SetPointerModeListener([&] { ++second_calls; });
  first.Reset();
  input.SetPointerMode(rex::hid::PointerMode::kLocked);
  CHECK(first_calls == 0);
  CHECK(second_calls == 1);
  second.Reset();
  app.Shutdown();
}

REX_TEST_CASE("a bad key binding warns and keeps the previous keys", "[hid][keybind]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "none";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  WarningSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  REQUIRE(input.GetSettings().mnk_mode.Set(true));
  REQUIRE(REX_SETTINGS().SetText(rex::SettingLayer::kTitle, "hid.keybind_lstick_up",
                                 "Space,NoSuchKey"));
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, rex::hid::VirtualKey::kW);
  app.Pump();
  REX_LOGGER().Flush();
  sink_guard.Reset();

  const std::vector<std::string> lines = sink.Take();
  size_t warned = 0;
  for (const std::string& line : lines) {
    if (line.find("keybind_lstick_up") != std::string::npos &&
        line.find("NoSuchKey") != std::string::npos) {
      ++warned;
    }
  }
  CHECK(warned == 1);
  auto reading = input.GetState(0);
  REQUIRE(reading.has_value());
  CHECK(reading->state.thumb_ly == INT16_MAX);
  app.Shutdown();
}

REX_TEST_CASE("a key binding written at run time moves the pad stick", "[hid][keybind]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "none";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  REQUIRE(input.GetSettings().mnk_mode.Set(true));
  REQUIRE(REX_SETTINGS().SetText(rex::SettingLayer::kTitle, "hid.keybind_lstick_up", "Shift+G"));
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, rex::hid::VirtualKey::kW);
  app.Pump();
  CHECK(input.GetState(0)->state.thumb_ly == 0);
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, rex::hid::VirtualKey::kShift);
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, rex::hid::VirtualKey::kG);
  app.Pump();
  CHECK(input.GetState(0)->state.thumb_ly == INT16_MAX);
  app.Shutdown();
}

REX_TEST_CASE("a pad keystroke stays queued while only the keyboard is asked for", "[hid]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0), MakePad(2, 1), MakePad(3, 2)};
    g_fake[0].states[1] = Buttons(0);
    g_fake[0].states[2] = Buttons(0);
    g_fake[0].states[3] = Buttons(rex::hid::kPadStart);
    CHECK_FALSE(
        input.GetKeystroke(rex::hid::IInputSystem::kAnyUser, rex::hid::KeystrokeSource::kKeyboard)
            .has_value());
    auto stroke =
        input.GetKeystroke(rex::hid::IInputSystem::kAnyUser, rex::hid::KeystrokeSource::kPad);
    REQUIRE(stroke.has_value());
    CHECK(stroke->user == 2);
  });
}

REX_TEST_CASE("a keyboard keystroke stays queued while only the pad is asked for", "[hid]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "none";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  REQUIRE(input.GetSettings().mnk_passthrough.Set(true));
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, rex::hid::VirtualKey::kW);
  app.Pump();
  CHECK_FALSE(input.GetKeystroke(0, rex::hid::KeystrokeSource::kPad).has_value());
  auto stroke = input.GetKeystroke(0, rex::hid::KeystrokeSource::kKeyboard);
  REQUIRE(stroke.has_value());
  CHECK(stroke->virtual_key != 0);
  CHECK_FALSE(input.GetKeystroke(0, kAllSources).has_value());
  app.Shutdown();
}
