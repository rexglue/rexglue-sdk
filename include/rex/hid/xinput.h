/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/assert.h>
#include <rex/types.h>

namespace rex::hid {

// NOLINTBEGIN(readability-identifier-naming) platform defined names

/** XInput's published thresholds. */
constexpr i32 X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849;
constexpr i32 X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE = 8689;
constexpr u8 X_INPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;

/** XInput capability flags. */
enum X_INPUT_CAPS {
  X_INPUT_CAPS_FFB_SUPPORTED = 0x0001,
  X_INPUT_CAPS_WIRELESS = 0x0002,
  X_INPUT_CAPS_VOICE_SUPPORTED = 0x0004,
  X_INPUT_CAPS_PMD_SUPPORTED = 0x0008,
  X_INPUT_CAPS_NO_NAVIGATION = 0x0010,
};

/** XInput device types. */
enum X_INPUT_DEVTYPE {
  XINPUT_DEVTYPE_GAMEPAD = 0x00000001,
  XINPUT_DEVTYPE_KEYBOARD = 0x00000002,
};

/** XInput device subtypes. */
enum X_INPUT_DEVSUBTYPE {
  XINPUT_DEVSUBTYPE_USB_KEYBOARD = 0x00000000,
  XINPUT_DEVSUBTYPE_GAMEPAD = 0x00000001,
  XINPUT_DEVSUBTYPE_WHEEL = 0x00000002,
  XINPUT_DEVSUBTYPE_ARCADE_STICK = 0x00000003,
  XINPUT_DEVSUBTYPE_FLIGHT_STICK = 0x00000004,
  XINPUT_DEVSUBTYPE_DANCE_PAD = 0x00000005,
  XINPUT_DEVSUBTYPE_GUITAR = 0x00000006,
  XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE = 0x00000007,
  XINPUT_DEVSUBTYPE_DRUM_KIT = 0x00000008,
  XINPUT_DEVSUBTYPE_GUITAR_BASS = 0x0000000B,
  XINPUT_DEVSUBTYPE_ARCADE_PAD = 0x00000013,
};

/** XInput query flags. */
enum X_INPUT_FLAG {
  X_INPUT_FLAG_GAMEPAD = 0x00000001,
  X_INPUT_FLAG_KEYBOARD = 0x00000002,
  X_INPUT_FLAG_UNKNOWN = 0x00000004,
  X_INPUT_FLAG_UNKNOWN2 = 0x00000008,
  X_INPUT_FLAG_MIC = 0x00000020,
  X_INPUT_FLAG_ANYDEVICE = 0x000000FF,
  X_INPUT_FLAG_ANY_USER = 1 << 30,
};

/** XInput gamepad button bits. */
enum X_INPUT_GAMEPAD_BUTTON {
  X_INPUT_GAMEPAD_DPAD_UP = 0x0001,
  X_INPUT_GAMEPAD_DPAD_DOWN = 0x0002,
  X_INPUT_GAMEPAD_DPAD_LEFT = 0x0004,
  X_INPUT_GAMEPAD_DPAD_RIGHT = 0x0008,
  X_INPUT_GAMEPAD_START = 0x0010,
  X_INPUT_GAMEPAD_BACK = 0x0020,
  X_INPUT_GAMEPAD_LEFT_THUMB = 0x0040,
  X_INPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
  X_INPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
  X_INPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
  X_INPUT_GAMEPAD_GUIDE = 0x0400,
  X_INPUT_GAMEPAD_A = 0x1000,
  X_INPUT_GAMEPAD_B = 0x2000,
  X_INPUT_GAMEPAD_X = 0x4000,
  X_INPUT_GAMEPAD_Y = 0x8000,
};

/** XInput keystroke flags. */
enum X_INPUT_KEYSTROKE_FLAGS {
  X_INPUT_KEYSTROKE_KEYDOWN = 0x0001,
  X_INPUT_KEYSTROKE_KEYUP = 0x0002,
  X_INPUT_KEYSTROKE_REPEAT = 0x0004,
  X_INPUT_KEYSTROKE_SHIFT = 0x0008,
  X_INPUT_KEYSTROKE_CTRL = 0x0010,
  X_INPUT_KEYSTROKE_ALT = 0x0020,
  X_INPUT_KEYSTROKE_VALIDUNICODE = 0x1000,
};

/** The XInput gamepad state. */
struct X_INPUT_GAMEPAD {
  be_u16 buttons;
  u8 left_trigger;
  u8 right_trigger;
  be_i16 thumb_lx;
  be_i16 thumb_ly;
  be_i16 thumb_rx;
  be_i16 thumb_ry;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_GAMEPAD, 12);

/** An XInput state packet. */
struct X_INPUT_STATE {
  be_u32 packet_number;
  X_INPUT_GAMEPAD gamepad;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_STATE, sizeof(X_INPUT_GAMEPAD) + 4);

/** XInput motor speeds. */
struct X_INPUT_VIBRATION {
  be_u16 left_motor_speed;
  be_u16 right_motor_speed;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_VIBRATION, 4);

/** XInput device capabilities. */
struct X_INPUT_CAPABILITIES {
  u8 type;
  u8 sub_type;
  be_u16 flags;
  X_INPUT_GAMEPAD gamepad;
  X_INPUT_VIBRATION vibration;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_CAPABILITIES,
                       sizeof(X_INPUT_GAMEPAD) + sizeof(X_INPUT_VIBRATION) + 4);

/** https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinput_keystroke(v=vs.85).aspx */
struct X_INPUT_KEYSTROKE {
  be_u16 virtual_key;
  be_u16 unicode;
  be_u16 flags;
  u8 user_index;
  u8 hid_code;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_KEYSTROKE, 8);

// NOLINTEND(readability-identifier-naming)

}  // namespace rex::hid
