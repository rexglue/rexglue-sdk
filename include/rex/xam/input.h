/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/input.h
 * @brief       XInput structures and constants and the XAM error codes
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>

#include <rex/assert.h>
#include <rex/types.h>
#include <rex/result.h>
#include <rex/kernel/types/status.h>

namespace rex::xam {

/** XInput's published thresholds. */
constexpr i32 kXInputGamepadLeftThumbDeadzone = 7849;
constexpr i32 kXInputGamepadRightThumbDeadzone = 8689;
constexpr u8 kXInputGamepadTriggerThreshold = 30;

enum X_INPUT_CAPS : u8 {
  /** XINPUT_CAPS_FFB_SUPPORTED. The device supports force feedback. */
  kXInputCapsFFBSupported = 0x0001,
  kXInputCapsWireless = 0x0002, /**< XINPUT_CAPS_WIRELESS. The device is wireless. */
  /** XINPUT_CAPS_VOICE_SUPPORTED. The device has an integrated voice device. */
  kXInputCapsVoiceSupported = 0x0004,
  /** XINPUT_CAPS_PMD_SUPPORTED. The device supports plugin module devices. */
  kXInputCapsPMDSupported = 0x0008,
  /** Capability bit of X_INPUT_CAPABILITIES::flags. No code reads it. */
  kXInputCapsNoNavigation = 0x0010,
};

enum X_INPUT_DEVTYPE : u8 {
  /** The device is a game controller. The HID drivers report it in X_INPUT_CAPABILITIES::type. */
  kXInputDevTypeGamepad = 0x00000001,
  /** The device is a keyboard. The keyboard driver reports it in X_INPUT_CAPABILITIES::type. */
  kXInputDevTypeKeyboard = 0x00000002,
};

enum X_INPUT_DEVSUBTYPE : u8 {
  /** Keyboard subtype. The keyboard driver reports it for the keyboard device. */
  kXInputDevSubtypeUSBKeyboard = 0x00000000,
  /** XDK gamepad subtype. Titles treat unrecognized subtypes as this one. */
  kXInputDevSubtypeGamepad = 0x00000001,
  kXInputDevSubtypeWheel = 0x00000002,
  kXInputDevSubtypeArcadeStick = 0x00000003,
  kXInputDevSubtypeFlightStick = 0x00000004,
  kXInputDevSubtypeDancePad = 0x00000005,
  kXInputDevSubtypeGuitar = 0x00000006,
  /** Alternate guitar controller subtype. Not in the XDK. */
  kXInputDevSubtypeGuitarAlternate = 0x00000007,
  kXInputDevSubtypeDrumKit = 0x00000008,
  /** Bass guitar controller subtype. Not in the XDK. */
  kXInputDevSubtypeGuitarBass = 0x0000000B,
  kXInputDevSubtypeArcadePad = 0x00000013, /**< Arcade pad subtype. Not in the XDK. */
};

enum X_INPUT_FLAG {
  kXInputFlagGamepad = 0x00000001,   /**< XINPUT_FLAG_GAMEPAD. Limits a query to the gamepad. */
  kXInputFlagKeyboard = 0x00000002,  /**< XINPUT_FLAG_KEYBOARD. Limits a query to the keyboard. */
  kXInputFlagUnknown = 0x00000004,   /**< Query flag bit 2. No code reads it. */
  kXInputFlagUnknown2 = 0x00000008,  /**< Query flag bit 3. No code reads it. */
  kXInputFlagMic = 0x00000020,       /**< Query flag bit 5. No code reads it. */
  kXInputFlagAnyDevice = 0x000000FF, /**< Mask of every device type bit in the low byte. */
  kXInputFlagAnyUser = 1 << 30,      /**< Query flag bit 30. Selects any user. */
};

enum X_INPUT_GAMEPAD_BUTTON : u16 {
  kXInputGamepadDPadUp = 0x0001,
  kXInputGamepadDPadDown = 0x0002,
  kXInputGamepadDPadLeft = 0x0004,
  kXInputGamepadDPadRight = 0x0008,
  kXInputGamepadStart = 0x0010,
  kXInputGamepadBack = 0x0020,
  kXInputGamepadLeftThumb = 0x0040,
  kXInputGamepadRightThumb = 0x0080,
  kXInputGamepadLeftShoulder = 0x0100,
  kXInputGamepadRightShoulder = 0x0200,
  kXInputGamepadGuide = 0x0400,
  kXInputGamepadA = 0x1000,
  kXInputGamepadB = 0x2000,
  kXInputGamepadX = 0x4000,
  kXInputGamepadY = 0x8000,
};

enum X_INPUT_KEYSTROKE_FLAGS : u16 {
  kXInputKeystrokeKeyDown = 0x0001, /**< XINPUT_KEYSTROKE_KEYDOWN. The key was pressed. */
  kXInputKeystrokeKeyUp = 0x0002,   /**< XINPUT_KEYSTROKE_KEYUP. The key was released. */
  kXInputKeystrokeRepeat = 0x0004,  /**< XINPUT_KEYSTROKE_REPEAT. A repeat of a held key. */
  kXInputKeystrokeShift = 0x0008,   /**< XINPUT_KEYSTROKE_SHIFT. A SHIFT key is down. */
  kXInputKeystrokeCtrl = 0x0010,    /**< XINPUT_KEYSTROKE_CTRL. A CTRL key is down. */
  kXInputKeystrokeAlt = 0x0020,     /**< XINPUT_KEYSTROKE_ALT. An ALT key is down. */
  /** Set by the keyboard driver when unicode holds a valid character. */
  kXInputKeystrokeValidUnicode = 0x1000,
};

struct X_INPUT_GAMEPAD {
  be_u16 buttons;   /**< Bitmask of pressed buttons, see X_INPUT_GAMEPAD_BUTTON. */
  u8 left_trigger;  /**< Left trigger analog value, 0 to 255. */
  u8 right_trigger; /**< Right trigger analog value, 0 to 255. */
  be_i16 thumb_lx;  /**< Left thumbstick x axis, 0 centered and positive right. */
  be_i16 thumb_ly;  /**< Left thumbstick y axis, 0 centered and positive up. */
  be_i16 thumb_rx;  /**< Right thumbstick x axis, 0 centered and positive right. */
  be_i16 thumb_ry;  /**< Right thumbstick y axis, 0 centered and positive up. */
};
REX_STATIC_ASSERT_SIZE(X_INPUT_GAMEPAD, 12);

struct X_INPUT_STATE {
  /** Changes only when the device state changed since the last poll. */
  be_u32 packet_number;
  X_INPUT_GAMEPAD gamepad;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_STATE, sizeof(X_INPUT_GAMEPAD) + 4);

struct X_INPUT_VIBRATION {
  /** Low frequency motor speed, 0 for off to 65535 for full. */
  be_u16 left_motor_speed;
  /** High frequency motor speed, 0 for off to 65535 for full. */
  be_u16 right_motor_speed;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_VIBRATION, 4);

struct X_INPUT_CAPABILITIES {
  u8 type;                     /**< Device type, see X_INPUT_DEVTYPE. */
  u8 sub_type;                 /**< Device subtype, see X_INPUT_DEVSUBTYPE. */
  be_u16 flags;                /**< Capability bits, see X_INPUT_CAPS. */
  X_INPUT_GAMEPAD gamepad;     /**< Supported gamepad controls and their resolution. */
  X_INPUT_VIBRATION vibration; /**< Supported vibration motors and their resolution. */
};
REX_STATIC_ASSERT_SIZE(X_INPUT_CAPABILITIES,
                       sizeof(X_INPUT_GAMEPAD) + sizeof(X_INPUT_VIBRATION) + 4);

/** https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinput_keystroke(v=vs.85).aspx */
struct X_INPUT_KEYSTROKE {
  be_u16 virtual_key; /**< VirtualKey code of the key, button or stick movement. */
  be_u16 unicode;     /**< Character the input produced, 0 when none. */
  be_u16 flags;       /**< Keyboard state at the input, see X_INPUT_KEYSTROKE_FLAGS. */
  u8 user_index;      /**< Index of the signed in user the device belongs to, 0 to 3. */
  u8 hid_code;        /**< USB HID usage of the key, 0 when none. */
};
REX_STATIC_ASSERT_SIZE(X_INPUT_KEYSTROKE, 8);

}  // namespace rex::xam

namespace rex::xam {

enum class HIDError : u8 {
  kNotConnected = 1, /**< No device answers for the user. */
  kEmpty = 2,        /**< The device has no keystroke queued. */
  kBadArguments = 3,
  kFailed = 4,
};

/**
 * Builds a rex::Error that carries a HID code.
 *
 * @return The error, in category Platform.
 */
[[nodiscard]] inline rex::Error BuildError(HIDError code, std::string message) {
  return {ErrorCategory::kPlatform, std::move(message), static_cast<int>(code)};
}

/**
 * @param error An error built by BuildError.
 */
[[nodiscard]] inline HIDError CodeOf(const rex::Error& error) {
  return static_cast<HIDError>(error.code);
}

/**
 * Returns the HID code a Win32 input result stands for. NotConnected, Empty and BadArguments map to
 * their codes and any other result gives Failed.
 *
 * @param result A failing Win32 code.
 */
[[nodiscard]] inline HIDError ToHIDError(rex::kernel::X_RESULT result) {
  switch (result) {
    case rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED:
      return HIDError::kNotConnected;
    case rex::kernel::X_ERROR_EMPTY:
      return HIDError::kEmpty;
    case rex::kernel::X_ERROR_BAD_ARGUMENTS:
      return HIDError::kBadArguments;
    default:
      return HIDError::kFailed;
  }
}

/** Returns the Win32 code the XAM input exports report for a HID code. */
[[nodiscard]] inline rex::kernel::X_RESULT ToXResult(HIDError code) {
  switch (code) {
    case HIDError::kNotConnected:
      return rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED;
    case HIDError::kEmpty:
      return rex::kernel::X_ERROR_EMPTY;
    case HIDError::kBadArguments:
      return rex::kernel::X_ERROR_BAD_ARGUMENTS;
    case HIDError::kFailed:
      break;
  }
  return rex::kernel::X_ERROR_FUNCTION_FAILED;
}

}  // namespace rex::xam
