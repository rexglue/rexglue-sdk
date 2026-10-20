// The program's remaps of the keyboard's controller buttons (SetButtonRemap):
// the keys of one bind row press another button; a real controller is never
// touched (the remap lives in the keyboard driver only).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_STATE;
namespace mnk = rex::input::mnk;
using rex::X_RESULT;
using rex::X_STATUS;

namespace {

constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);  // the driver's one device

void Key(mnk::MnkInputDriver& driver, rex::ui::VirtualKey key, bool down) {
  rex::ui::KeyEvent e(nullptr, key, 1, !down, false, false, false, false);
  if (down)
    driver.OnKeyDown(e);
  else
    driver.OnKeyUp(e);
}

uint16_t Buttons(mnk::MnkInputDriver& driver) {
  X_INPUT_STATE state{};
  REQUIRE(driver.GetDeviceState(kDevice, &state) == X_ERROR_SUCCESS);
  return static_cast<uint16_t>(state.gamepad.buttons);
}

}  // namespace

TEST_CASE("Keyboard rows press the remapped button", "[input][mnk]") {
  REQUIRE(rex::cvar::SetFlagByName("mnk_mode", "true"));
  REQUIRE(rex::cvar::SetFlagByName("keybind_right_shoulder", "G"));
  REQUIRE(rex::cvar::SetFlagByName("keybind_left_shoulder", "4"));
  REQUIRE(rex::cvar::SetFlagByName("keybind_a", "Space"));
  mnk::ClearButtonRemaps();
  mnk::MnkInputDriver driver(nullptr, 0);

  Key(driver, rex::ui::VirtualKey::kG, true);
  CHECK(Buttons(driver) == X_INPUT_GAMEPAD_RIGHT_SHOULDER);

  // Swap the shoulders: G presses the left one, 4 the right one.
  mnk::SetButtonRemap(X_INPUT_GAMEPAD_RIGHT_SHOULDER, X_INPUT_GAMEPAD_LEFT_SHOULDER);
  mnk::SetButtonRemap(X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  CHECK(mnk::RemappedButton(X_INPUT_GAMEPAD_RIGHT_SHOULDER) == X_INPUT_GAMEPAD_LEFT_SHOULDER);
  CHECK(Buttons(driver) == X_INPUT_GAMEPAD_LEFT_SHOULDER);
  Key(driver, rex::ui::VirtualKey::k4, true);
  CHECK(Buttons(driver) == (X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER));
  Key(driver, rex::ui::VirtualKey::kG, false);
  CHECK(Buttons(driver) == X_INPUT_GAMEPAD_RIGHT_SHOULDER);  // 4 alone: the right shoulder now
  Key(driver, rex::ui::VirtualKey::k4, false);

  // Other rows are untouched.
  Key(driver, rex::ui::VirtualKey::kSpace, true);
  CHECK(Buttons(driver) == X_INPUT_GAMEPAD_A);
  Key(driver, rex::ui::VirtualKey::kSpace, false);

  // Undo one row (to itself), then all.
  mnk::SetButtonRemap(X_INPUT_GAMEPAD_RIGHT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  Key(driver, rex::ui::VirtualKey::kG, true);
  CHECK(Buttons(driver) == X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  Key(driver, rex::ui::VirtualKey::kG, false);
  mnk::ClearButtonRemaps();
  Key(driver, rex::ui::VirtualKey::k4, true);
  CHECK(Buttons(driver) == X_INPUT_GAMEPAD_LEFT_SHOULDER);
  Key(driver, rex::ui::VirtualKey::k4, false);

  // Not single buttons: ignored.
  mnk::SetButtonRemap(X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_A, X_INPUT_GAMEPAD_A);
  mnk::SetButtonRemap(X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  CHECK(mnk::RemappedButton(X_INPUT_GAMEPAD_LEFT_SHOULDER) == X_INPUT_GAMEPAD_LEFT_SHOULDER);
  rex::cvar::SetFlagByName("mnk_mode", "false");
}
