/**
 * @file        tests/unit/kernel/xam_input_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <cstring>

#include "../hid/input_harness.h"
#include "../ui/ui_harness.h"
#include "xam/input_convert.h"

using rex::xam::ToInputResult;
using rex::xam::ToPadVibration;
using rex::xam::ToXInputCapabilities;
using rex::xam::ToXInputKeystroke;
using rex::xam::ToXInputState;

namespace {

template <typename T>
std::array<u8, sizeof(T)> BytesOf(const T& value) {
  std::array<u8, sizeof(T)> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(T));
  return bytes;
}

}  // namespace

REX_TEST_CASE("xinput state converts to big endian", "[kernel][xam][input]") {
  rex::hid::IInputSystem::PadReading reading;
  reading.packet = 0x01020304;
  reading.state.buttons = rex::hid::kPadA | rex::hid::kPadStart;
  reading.state.left_trigger = 200;
  reading.state.thumb_lx = 1000;
  reading.state.thumb_ly = -2;

  const auto state = rex::xam::ToXInputState(reading);
  CHECK(BytesOf(state.packet_number) == std::array<u8, 4>{0x01, 0x02, 0x03, 0x04});
  CHECK(BytesOf(state.gamepad.thumb_lx) == std::array<u8, 2>{0x03, 0xE8});
  CHECK(BytesOf(state.gamepad.thumb_ly) == std::array<u8, 2>{0xFF, 0xFE});
  CHECK(BytesOf(state.gamepad.buttons) == std::array<u8, 2>{0x10, 0x10});
  CHECK(state.gamepad.left_trigger == 200);
  CHECK(state.gamepad.thumb_lx == 1000);
}

REX_TEST_CASE("xinput capabilities keep the gamepad and drop it for a keyboard",
              "[kernel][xam][input]") {
  rex::hid::PadCapabilities pad;
  pad.subtype = 0x01;
  pad.flags = 0x0002;
  pad.supported.buttons = 0xF3FF;
  pad.motors.left_motor = 0xFFFF;
  const auto gamepad = rex::xam::ToXInputCapabilities(pad);
  CHECK(gamepad.type == rex::xam::kXInputDevTypeGamepad);
  CHECK(gamepad.sub_type == 0x01);
  CHECK(gamepad.flags == 0x0002);
  CHECK(gamepad.gamepad.buttons == 0xF3FF);
  CHECK(gamepad.vibration.left_motor_speed == 0xFFFF);

  rex::hid::PadCapabilities keyboard;
  keyboard.subtype = 0x00;
  keyboard.supported.buttons = 0xFFFF;
  const auto typed = rex::xam::ToXInputCapabilities(keyboard);
  CHECK(typed.type == rex::xam::kXInputDevTypeKeyboard);
  CHECK(typed.sub_type == 0x00);
  CHECK(typed.gamepad.buttons == 0);
}

REX_TEST_CASE("xinput keystroke and vibration convert", "[kernel][xam][input]") {
  rex::hid::PadKeystroke keystroke;
  keystroke.virtual_key = 0x5800;
  keystroke.flags = 0x0001;
  keystroke.user = 2;
  const auto converted = rex::xam::ToXInputKeystroke(keystroke);
  CHECK(converted.virtual_key == 0x5800);
  CHECK(converted.flags == 0x0001);
  CHECK(converted.user_index == 2);
  CHECK(BytesOf(converted.virtual_key) == std::array<u8, 2>{0x58, 0x00});

  rex::xam::X_INPUT_VIBRATION vibration;
  vibration.left_motor_speed = 0x1234;
  vibration.right_motor_speed = 0x00FF;
  const auto motors = rex::xam::ToPadVibration(vibration);
  CHECK(motors.left_motor == 0x1234);
  CHECK(motors.right_motor == 0x00FF);
}

REX_TEST_CASE("a missing pad reads as not connected and other failures as failed",
              "[kernel][xam][input]") {
  CHECK(rex::xam::ToInputResult(rex::Error(rex::ErrorCategory::kNotFound, "no pad")) ==
        rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(rex::xam::ToInputResult(rex::Error(rex::ErrorCategory::kPlatform, "refused")) ==
        rex::kernel::X_ERROR_FUNCTION_FAILED);
}

REX_TEST_CASE("an any user keystroke reports the user that produced it", "[kernel][xam][input]") {
  using namespace rex::test_input;
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0), MakePad(2, 1), MakePad(3, 2)};
    g_fake[0].states[1] = Buttons(0);
    g_fake[0].states[2] = Buttons(0);
    g_fake[0].states[3] = Buttons(rex::hid::kPadStart);
    u32 user = 0;
    rex::xam::X_INPUT_KEYSTROKE out{};
    const u32 flags = rex::xam::kXInputFlagGamepad | rex::xam::kXInputFlagAnyUser;
    CHECK(rex::xam::TakeInputKeystroke(input, user, flags, out) == rex::kernel::X_ERROR_SUCCESS);
    CHECK(user == 2);
    CHECK(out.user_index == 2);
    CHECK(out.virtual_key != 0);
  });
}

REX_TEST_CASE("a keystroke query without the gamepad bit leaves a pad key queued",
              "[kernel][xam][input]") {
  using namespace rex::test_input;
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    g_fake[0].devices = {MakePad(1, 0)};
    g_fake[0].states[1] = Buttons(rex::hid::kPadStart);
    u32 user = 0;
    rex::xam::X_INPUT_KEYSTROKE out{};
    out.virtual_key = 0x1234;
    CHECK(rex::xam::TakeInputKeystroke(input, user, rex::xam::kXInputFlagKeyboard, out) ==
          rex::kernel::X_ERROR_EMPTY);
    CHECK(out.virtual_key == 0);
    CHECK(rex::xam::TakeInputKeystroke(input, user, rex::xam::kXInputFlagGamepad, out) ==
          rex::kernel::X_ERROR_SUCCESS);
    CHECK(out.virtual_key != 0);
  });
}

REX_TEST_CASE("a keystroke query without the gamepad bit still returns a keyboard key",
              "[kernel][xam][input]") {
  rex::test_ui::UIApp app;
  app.input_drivers = "none";
  REQUIRE(app.Start());
  rex::hid::IInputSystem& input = app.GetInput();
  REQUIRE(input.GetSettings().mnk_passthrough.Set(true));
  rex::test_ui::DeliverKey(rex::InputEvent::Type::kKeyDown, rex::VirtualKey::kW);
  app.Pump();
  u32 user = 3;
  rex::xam::X_INPUT_KEYSTROKE out{};
  const u32 any_user = rex::xam::kXInputFlagAnyUser;
  CHECK(rex::xam::TakeInputKeystroke(input, user, rex::xam::kXInputFlagGamepad | any_user, out) ==
        rex::kernel::X_ERROR_EMPTY);
  CHECK(rex::xam::TakeInputKeystroke(input, user, rex::xam::kXInputFlagKeyboard | any_user, out) ==
        rex::kernel::X_ERROR_SUCCESS);
  CHECK(out.virtual_key != 0);
  CHECK(user == 0);
  app.Shutdown();
}
