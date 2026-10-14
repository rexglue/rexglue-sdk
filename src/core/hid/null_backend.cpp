/**
 * @file        core/hid/null_backend.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/hid/backend.h>
#include <rex/move_only_function.h>

namespace rex::hid {

namespace {

rex::Error NotConnected() {
  return BuildError(HIDError::kNotConnected, "only user 0 has the idle pad");
}

}  // namespace

Result<X_INPUT_CAPABILITIES> NullHIDBackend::GetCapabilities(u32 user, u32) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  X_INPUT_CAPABILITIES caps{};
  caps.type = kXInputDevTypeGamepad;
  caps.sub_type = kXInputDevSubtypeGamepad;
  caps.gamepad.buttons = 0xFFFF;
  caps.gamepad.left_trigger = 0xFF;
  caps.gamepad.right_trigger = 0xFF;
  caps.gamepad.thumb_lx = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_ly = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_rx = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_ry = static_cast<i16>(0x7FFF);
  caps.vibration.left_motor_speed = 0xFFFF;
  caps.vibration.right_motor_speed = 0xFFFF;
  return caps;
}

Result<X_INPUT_STATE> NullHIDBackend::GetState(u32 user) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  std::lock_guard lock(mutex_);
  if (!reported_) {
    reported_ = true;
    if (listener_) {
      listener_(0, true);
    }
  }
  return X_INPUT_STATE{};
}

Result<void> NullHIDBackend::SetVibration(u32 user, const X_INPUT_VIBRATION&) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  return Ok();
}

Result<X_INPUT_KEYSTROKE> NullHIDBackend::GetKeystroke(u32 user, u32) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  return std::unexpected(BuildError(HIDError::kEmpty, "the idle pad has no keystrokes"));
}

void NullHIDBackend::SetConnectionListener(rex::MoveOnlyFunction<void(u32, bool)> listener) {
  std::lock_guard lock(mutex_);
  listener_ = std::move(listener);
}

}  // namespace rex::hid
