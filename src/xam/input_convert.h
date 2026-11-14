/**
 * @file        xam/input_convert.h
 * @brief       Converts IInputSystem values to the XInput structs a title reads
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/hid/input_system.h>
#include <rex/xam/input.h>
#include <rex/result.h>
#include <rex/kernel/types/status.h>

namespace rex::xam {

/** Converts a pad reading to the title's state struct, big endian. */
inline rex::xam::X_INPUT_STATE ToXInputState(const rex::hid::IInputSystem::PadReading& reading) {
  rex::xam::X_INPUT_STATE out{};
  out.packet_number = reading.packet;
  out.gamepad.buttons = reading.state.buttons;
  out.gamepad.left_trigger = reading.state.left_trigger;
  out.gamepad.right_trigger = reading.state.right_trigger;
  out.gamepad.thumb_lx = reading.state.thumb_lx;
  out.gamepad.thumb_ly = reading.state.thumb_ly;
  out.gamepad.thumb_rx = reading.state.thumb_rx;
  out.gamepad.thumb_ry = reading.state.thumb_ry;
  return out;
}

/**
 * Converts pad capabilities to the title's struct. A keyboard, which has subtype zero, reports the
 * keyboard device type and nothing else.
 */
inline rex::xam::X_INPUT_CAPABILITIES ToXInputCapabilities(const rex::hid::PadCapabilities& caps) {
  rex::xam::X_INPUT_CAPABILITIES out{};
  out.sub_type = caps.subtype;
  if (caps.subtype == rex::xam::kXInputDevSubtypeUSBKeyboard) {
    out.type = rex::xam::kXInputDevTypeKeyboard;
    return out;
  }
  out.type = rex::xam::kXInputDevTypeGamepad;
  out.flags = caps.flags;
  out.gamepad.buttons = caps.supported.buttons;
  out.gamepad.left_trigger = caps.supported.left_trigger;
  out.gamepad.right_trigger = caps.supported.right_trigger;
  out.gamepad.thumb_lx = caps.supported.thumb_lx;
  out.gamepad.thumb_ly = caps.supported.thumb_ly;
  out.gamepad.thumb_rx = caps.supported.thumb_rx;
  out.gamepad.thumb_ry = caps.supported.thumb_ry;
  out.vibration.left_motor_speed = caps.motors.left_motor;
  out.vibration.right_motor_speed = caps.motors.right_motor;
  return out;
}

/** Converts a keystroke to the title's struct. */
inline rex::xam::X_INPUT_KEYSTROKE ToXInputKeystroke(const rex::hid::PadKeystroke& keystroke) {
  rex::xam::X_INPUT_KEYSTROKE out{};
  out.virtual_key = keystroke.virtual_key;
  out.unicode = keystroke.unicode;
  out.flags = keystroke.flags;
  out.user_index = keystroke.user;
  out.hid_code = keystroke.hid_code;
  return out;
}

/**
 * Takes the next keystroke the way XamInputGetKeystrokeEx does.
 *
 * @param input The input system.
 * @param user The user index the title passed. With the any user flag it receives the user that
 * produced the keystroke.
 * @param flags The query flags: kXInputFlagGamepad and kXInputFlagKeyboard select the devices,
 * kXInputFlagAnyUser takes the keystroke from any user.
 * @param out The keystroke struct, zeroed first.
 * @return X_ERROR_SUCCESS, X_ERROR_EMPTY when a user is connected and no keystroke waits, else
 * X_ERROR_DEVICE_NOT_CONNECTED.
 */
inline rex::kernel::X_RESULT TakeInputKeystroke(rex::hid::IInputSystem& input, u32& user, u32 flags,
                                                rex::xam::X_INPUT_KEYSTROKE& out) {
  out = {};
  const bool any_user = (flags & rex::xam::kXInputFlagAnyUser) != 0;
  const u32 query = any_user ? rex::hid::IInputSystem::kAnyUser : (user & 0xFF) == 0xFF ? 0 : user;
  if (!any_user && query >= rex::hid::IInputSystem::kMaxUsers) {
    return rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  using rex::hid::KeystrokeSource;
  KeystrokeSource sources = KeystrokeSource::kNone;
  if (flags & rex::xam::kXInputFlagGamepad) {
    sources = sources | KeystrokeSource::kPad;
  }
  if (flags & rex::xam::kXInputFlagKeyboard) {
    sources = sources | KeystrokeSource::kKeyboard;
  }
  auto result = input.GetKeystroke(query, sources);
  if (result) {
    out = ToXInputKeystroke(*result);
    if (any_user) {
      user = result->user;
    }
    return rex::kernel::X_ERROR_SUCCESS;
  }
  const u32 first = any_user ? 0 : query;
  const u32 last = any_user ? rex::hid::IInputSystem::kMaxUsers : query + 1;
  for (u32 candidate = first; candidate < last; ++candidate) {
    if (input.GetState(candidate)) {
      return rex::kernel::X_ERROR_EMPTY;
    }
  }
  return rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED;
}

/**
 * Reads the motor speeds the title asked for.
 *
 * @return The speeds in host order.
 */
inline rex::hid::PadVibration ToPadVibration(const rex::xam::X_INPUT_VIBRATION& vibration) {
  rex::hid::PadVibration out;
  out.left_motor = vibration.left_motor_speed;
  out.right_motor = vibration.right_motor_speed;
  return out;
}

/**
 * Returns the result code the title sees for a failed input call. A missing pad reads as not
 * connected, any other failure as a failed function.
 */
inline kernel::X_RESULT ToInputResult(const Error& error) {
  return error.category == ErrorCategory::kNotFound ? kernel::X_ERROR_DEVICE_NOT_CONNECTED
                                                    : kernel::X_ERROR_FUNCTION_FAILED;
}

}  // namespace rex::xam
