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

/** XInput's published thresholds. */
constexpr i32 X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849;
constexpr i32 X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE = 8689;
constexpr u8 X_INPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;

/** XInput capability flags. */
enum X_INPUT_CAPS : u8 {
  /** XINPUT_CAPS_FFB_SUPPORTED. The device supports force feedback. */
  X_INPUT_CAPS_FFB_SUPPORTED = 0x0001,
  X_INPUT_CAPS_WIRELESS = 0x0002, /**< XINPUT_CAPS_WIRELESS. The device is wireless. */
  /** XINPUT_CAPS_VOICE_SUPPORTED. The device has an integrated voice device. */
  X_INPUT_CAPS_VOICE_SUPPORTED = 0x0004,
  /** XINPUT_CAPS_PMD_SUPPORTED. The device supports plugin module devices. */
  X_INPUT_CAPS_PMD_SUPPORTED = 0x0008,
  /** Capability bit of X_INPUT_CAPABILITIES::flags. No code reads it. */
  X_INPUT_CAPS_NO_NAVIGATION = 0x0010,
};

/** XInput device types. */
enum X_INPUT_DEVTYPE : u8 {
  /** The device is a game controller. The HID drivers report it in X_INPUT_CAPABILITIES::type. */
  XINPUT_DEVTYPE_GAMEPAD = 0x00000001,
  /** The device is a keyboard. The keyboard driver reports it in X_INPUT_CAPABILITIES::type. */
  XINPUT_DEVTYPE_KEYBOARD = 0x00000002,
};

/** XInput device subtypes. */
enum X_INPUT_DEVSUBTYPE : u8 {
  /** Keyboard subtype. The keyboard driver reports it for the keyboard device. */
  XINPUT_DEVSUBTYPE_USB_KEYBOARD = 0x00000000,
  /** XDK gamepad subtype. Titles treat unrecognized subtypes as this one. */
  XINPUT_DEVSUBTYPE_GAMEPAD = 0x00000001,
  XINPUT_DEVSUBTYPE_WHEEL = 0x00000002,        /**< XDK wheel subtype. */
  XINPUT_DEVSUBTYPE_ARCADE_STICK = 0x00000003, /**< XDK arcade style joystick subtype. */
  XINPUT_DEVSUBTYPE_FLIGHT_STICK = 0x00000004, /**< XDK joystick for flight games subtype. */
  XINPUT_DEVSUBTYPE_DANCE_PAD = 0x00000005,    /**< XDK dance pad subtype. */
  XINPUT_DEVSUBTYPE_GUITAR = 0x00000006,       /**< XDK guitar controller subtype. */
  /** Alternate guitar controller subtype. Not in the XDK. */
  XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE = 0x00000007,
  XINPUT_DEVSUBTYPE_DRUM_KIT = 0x00000008, /**< XDK drum kit subtype. */
  /** Bass guitar controller subtype. Not in the XDK. */
  XINPUT_DEVSUBTYPE_GUITAR_BASS = 0x0000000B,
  XINPUT_DEVSUBTYPE_ARCADE_PAD = 0x00000013, /**< Arcade pad subtype. Not in the XDK. */
};

/** XInput query flags. */
enum X_INPUT_FLAG {
  X_INPUT_FLAG_GAMEPAD = 0x00000001,   /**< XINPUT_FLAG_GAMEPAD. Limits a query to the gamepad. */
  X_INPUT_FLAG_KEYBOARD = 0x00000002,  /**< XINPUT_FLAG_KEYBOARD. Limits a query to the keyboard. */
  X_INPUT_FLAG_UNKNOWN = 0x00000004,   /**< Query flag bit 2. No code reads it. */
  X_INPUT_FLAG_UNKNOWN2 = 0x00000008,  /**< Query flag bit 3. No code reads it. */
  X_INPUT_FLAG_MIC = 0x00000020,       /**< Query flag bit 5. No code reads it. */
  X_INPUT_FLAG_ANYDEVICE = 0x000000FF, /**< Mask of every device type bit in the low byte. */
  X_INPUT_FLAG_ANY_USER = 1 << 30,     /**< Query flag bit 30. No code reads it. */
};

/** XInput gamepad button bits. */
enum X_INPUT_GAMEPAD_BUTTON : u16 {
  X_INPUT_GAMEPAD_DPAD_UP = 0x0001,    /**< Bit 0 of X_INPUT_GAMEPAD::buttons. D-pad up. */
  X_INPUT_GAMEPAD_DPAD_DOWN = 0x0002,  /**< Bit 1 of X_INPUT_GAMEPAD::buttons. D-pad down. */
  X_INPUT_GAMEPAD_DPAD_LEFT = 0x0004,  /**< Bit 2 of X_INPUT_GAMEPAD::buttons. D-pad left. */
  X_INPUT_GAMEPAD_DPAD_RIGHT = 0x0008, /**< Bit 3 of X_INPUT_GAMEPAD::buttons. D-pad right. */
  X_INPUT_GAMEPAD_START = 0x0010,      /**< Bit 4 of X_INPUT_GAMEPAD::buttons. START button. */
  X_INPUT_GAMEPAD_BACK = 0x0020,       /**< Bit 5 of X_INPUT_GAMEPAD::buttons. BACK button. */
  /** Bit 6 of X_INPUT_GAMEPAD::buttons. Left thumbstick click. */
  X_INPUT_GAMEPAD_LEFT_THUMB = 0x0040,
  /** Bit 7 of X_INPUT_GAMEPAD::buttons. Right thumbstick click. */
  X_INPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
  /** Bit 8 of X_INPUT_GAMEPAD::buttons. Left shoulder button. */
  X_INPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
  /** Bit 9 of X_INPUT_GAMEPAD::buttons. Right shoulder button. */
  X_INPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
  X_INPUT_GAMEPAD_GUIDE = 0x0400, /**< Bit 10 of X_INPUT_GAMEPAD::buttons. Guide button. */
  X_INPUT_GAMEPAD_A = 0x1000,     /**< Bit 12 of X_INPUT_GAMEPAD::buttons. A button. */
  X_INPUT_GAMEPAD_B = 0x2000,     /**< Bit 13 of X_INPUT_GAMEPAD::buttons. B button. */
  X_INPUT_GAMEPAD_X = 0x4000,     /**< Bit 14 of X_INPUT_GAMEPAD::buttons. X button. */
  X_INPUT_GAMEPAD_Y = 0x8000,     /**< Bit 15 of X_INPUT_GAMEPAD::buttons. Y button. */
};

/** XInput keystroke flags. */
enum X_INPUT_KEYSTROKE_FLAGS : u16 {
  X_INPUT_KEYSTROKE_KEYDOWN = 0x0001, /**< XINPUT_KEYSTROKE_KEYDOWN. The key was pressed. */
  X_INPUT_KEYSTROKE_KEYUP = 0x0002,   /**< XINPUT_KEYSTROKE_KEYUP. The key was released. */
  X_INPUT_KEYSTROKE_REPEAT = 0x0004,  /**< XINPUT_KEYSTROKE_REPEAT. A repeat of a held key. */
  X_INPUT_KEYSTROKE_SHIFT = 0x0008,   /**< XINPUT_KEYSTROKE_SHIFT. A SHIFT key is down. */
  X_INPUT_KEYSTROKE_CTRL = 0x0010,    /**< XINPUT_KEYSTROKE_CTRL. A CTRL key is down. */
  X_INPUT_KEYSTROKE_ALT = 0x0020,     /**< XINPUT_KEYSTROKE_ALT. An ALT key is down. */
  /** Set by the keyboard driver when unicode holds a valid character. */
  X_INPUT_KEYSTROKE_VALIDUNICODE = 0x1000,
};

/** The XInput gamepad state. */
struct X_INPUT_GAMEPAD {
  be_u16 buttons;   /**< Offset 0x0. Bitmask of pressed buttons, see X_INPUT_GAMEPAD_BUTTON. */
  u8 left_trigger;  /**< Offset 0x2. Left trigger analog value, 0 to 255. */
  u8 right_trigger; /**< Offset 0x3. Right trigger analog value, 0 to 255. */
  be_i16 thumb_lx;  /**< Offset 0x4. Left thumbstick x axis, 0 centered and positive right. */
  be_i16 thumb_ly;  /**< Offset 0x6. Left thumbstick y axis, 0 centered and positive up. */
  be_i16 thumb_rx;  /**< Offset 0x8. Right thumbstick x axis, 0 centered and positive right. */
  be_i16 thumb_ry;  /**< Offset 0xA. Right thumbstick y axis, 0 centered and positive up. */
};
REX_STATIC_ASSERT_SIZE(X_INPUT_GAMEPAD, 12);

/** An XInput state packet. */
struct X_INPUT_STATE {
  /** Offset 0x0. Changes only when the device state changed since the last poll. */
  be_u32 packet_number;
  X_INPUT_GAMEPAD gamepad; /**< Offset 0x4. Current gamepad state. */
};
REX_STATIC_ASSERT_SIZE(X_INPUT_STATE, sizeof(X_INPUT_GAMEPAD) + 4);

/** XInput motor speeds. */
struct X_INPUT_VIBRATION {
  /** Offset 0x0. Low frequency motor speed, 0 for off to 65535 for full. */
  be_u16 left_motor_speed;
  /** Offset 0x2. High frequency motor speed, 0 for off to 65535 for full. */
  be_u16 right_motor_speed;
};
REX_STATIC_ASSERT_SIZE(X_INPUT_VIBRATION, 4);

/** XInput device capabilities. */
struct X_INPUT_CAPABILITIES {
  u8 type;                     /**< Offset 0x0. Device type, see X_INPUT_DEVTYPE. */
  u8 sub_type;                 /**< Offset 0x1. Device subtype, see X_INPUT_DEVSUBTYPE. */
  be_u16 flags;                /**< Offset 0x2. Capability bits, see X_INPUT_CAPS. */
  X_INPUT_GAMEPAD gamepad;     /**< Offset 0x4. Supported gamepad controls and their resolution. */
  X_INPUT_VIBRATION vibration; /**< Offset 0x10. Supported vibration motors and their resolution. */
};
REX_STATIC_ASSERT_SIZE(X_INPUT_CAPABILITIES,
                       sizeof(X_INPUT_GAMEPAD) + sizeof(X_INPUT_VIBRATION) + 4);

/** https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinput_keystroke(v=vs.85).aspx */
struct X_INPUT_KEYSTROKE {
  be_u16 virtual_key; /**< Offset 0x0. VirtualKey code of the key, button or stick movement. */
  be_u16 unicode;     /**< Offset 0x2. Character the input produced, 0 when none. */
  be_u16 flags;       /**< Offset 0x4. Keyboard state at the input, see X_INPUT_KEYSTROKE_FLAGS. */
  u8 user_index;      /**< Offset 0x6. Index of the signed in user the device belongs to, 0 to 3. */
  u8 hid_code;        /**< Offset 0x7. USB HID usage of the key, 0 when none. */
};
REX_STATIC_ASSERT_SIZE(X_INPUT_KEYSTROKE, 8);

}  // namespace rex::hid
