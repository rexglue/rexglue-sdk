/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        hid/input_keys.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rex/bit.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::hid {

/** Windows and XInput virtual keys. Platform keys translate to it for keystrokes and UI events. */
enum class VirtualKey : u16 {
  kNone, /**< Not a valid key, MapVirtualKey has no translation. */

  kLButton = 0x01,
  kRButton = 0x02,
  kCancel = 0x03, /**< Control break. */
  kMButton = 0x04,

  kXButton1 = 0x05, /**< First extra mouse button. */
  kXButton2 = 0x06, /**< Second extra mouse button. */

  kBack = 0x08,
  kTab = 0x09,

  kClear = 0x0C,
  kReturn = 0x0D,
  kWheelUp = 0x0E,
  kWheelDown = 0x0F,

  kShift = 0x10, /**< Either Shift key. */
  kControl = 0x11,
  kMenu = 0x12, /**< Alt. */
  kPause = 0x13,
  kCapital = 0x14, /**< Caps Lock. */

  kKana = 0x15,    /**< IME Kana mode, Japanese keyboards. */
  kHangeul = 0x15, /**< IME Hangul mode, same code as kKana. */
  kHangul = 0x15,  /**< IME Hangul mode, same code as kKana. */
  kIMEOn = 0x16,
  kJunja = 0x17,
  kFinal = 0x18,
  kHanja = 0x19, /**< IME Hanja mode, Korean keyboards. */
  kKanji = 0x19, /**< IME Kanji mode, same code as kHanja. */
  kIMEOff = 0x1A,

  kEscape = 0x1B,

  kConvert = 0x1C,
  kNonConvert = 0x1D,
  kAccept = 0x1E,
  kModeChange = 0x1F, /**< IME mode change request. */

  kSpace = 0x20,
  kPrior = 0x21, /**< Page Up. */
  kNext = 0x22,  /**< Page Down. */
  kEnd = 0x23,
  kHome = 0x24,
  kLeft = 0x25,
  kUp = 0x26,
  kRight = 0x27,
  kDown = 0x28,
  kSelect = 0x29,
  kPrint = 0x2A,
  kExecute = 0x2B,
  kSnapshot = 0x2C, /**< Print Screen key. */
  kInsert = 0x2D,
  kDelete = 0x2E,
  kHelp = 0x2F,

  /** @name Digit keys 0 to 9, equal to their ASCII codes 0x30 to 0x39. */
  /** @{ */
  k0 = 0x30,
  k1 = 0x31,
  k2 = 0x32,
  k3 = 0x33,
  k4 = 0x34,
  k5 = 0x35,
  k6 = 0x36,
  k7 = 0x37,
  k8 = 0x38,
  k9 = 0x39,
  /** @} */

  /** @name Letter keys A to Z, equal to their uppercase ASCII codes 0x41 to 0x5A. */
  /** @{ */
  kA = 0x41,
  kB = 0x42,
  kC = 0x43,
  kD = 0x44,
  kE = 0x45,
  kF = 0x46,
  kG = 0x47,
  kH = 0x48,
  kI = 0x49,
  kJ = 0x4A,
  kK = 0x4B,
  kL = 0x4C,
  kM = 0x4D,
  kN = 0x4E,
  kO = 0x4F,
  kP = 0x50,
  kQ = 0x51,
  kR = 0x52,
  kS = 0x53,
  kT = 0x54,
  kU = 0x55,
  kV = 0x56,
  kW = 0x57,
  kX = 0x58,
  kY = 0x59,
  kZ = 0x5A,
  /** @} */

  kLWin = 0x5B,
  kRWin = 0x5C,
  kApps = 0x5D, /**< Applications key that opens the context menu. */

  kSleep = 0x5F,

  kNumpad0 = 0x60,
  kNumpad1 = 0x61,
  kNumpad2 = 0x62,
  kNumpad3 = 0x63,
  kNumpad4 = 0x64,
  kNumpad5 = 0x65,
  kNumpad6 = 0x66,
  kNumpad7 = 0x67,
  kNumpad8 = 0x68,
  kNumpad9 = 0x69,
  kMultiply = 0x6A,  /**< Numeric keypad multiply key. */
  kAdd = 0x6B,       /**< Numeric keypad add key. */
  kSeparator = 0x6C, /**< Numeric keypad separator key. */
  kSubtract = 0x6D,  /**< Numeric keypad subtract key. */
  kDecimal = 0x6E,   /**< Numeric keypad decimal key. */
  kDivide = 0x6F,    /**< Numeric keypad divide key. */
  kF1 = 0x70,
  kF2 = 0x71,
  kF3 = 0x72,
  kF4 = 0x73,
  kF5 = 0x74,
  kF6 = 0x75,
  kF7 = 0x76,
  kF8 = 0x77,
  kF9 = 0x78,
  kF10 = 0x79,
  kF11 = 0x7A,
  kF12 = 0x7B,
  kF13 = 0x7C,
  kF14 = 0x7D,
  kF15 = 0x7E,
  kF16 = 0x7F,
  kF17 = 0x80,
  kF18 = 0x81,
  kF19 = 0x82,
  kF20 = 0x83,
  kF21 = 0x84,
  kF22 = 0x85,
  kF23 = 0x86,
  kF24 = 0x87,

  /** VK_NAVIGATION_* added in _WIN32_WINNT 0x0604, reserved in WinUser.h. */
  kNavigationView = 0x88,
  kNavigationMenu = 0x89,
  kNavigationUp = 0x8A,
  kNavigationDown = 0x8B,
  kNavigationLeft = 0x8C,
  kNavigationRight = 0x8D,
  kNavigationAccept = 0x8E,
  kNavigationCancel = 0x8F,

  kNumLock = 0x90,
  kScroll = 0x91, /**< Scroll Lock key. */

  /** NEC PC-9800 keyboard. */
  kOemNecEqual = 0x92, /**< '=' key on the numpad. */

  /** Fujitsu/OASYS keyboard. */
  kOemFjJisho = 0x92,   /**< 'Dictionary' key. */
  kOemFjMasshou = 0x93, /**< 'Unregister word' key. */
  kOemFjTouroku = 0x94, /**< 'Register word' key. */
  kOemFjLOya = 0x95,    /**< 'Left OYAYUBI' key. */
  kOemFjROya = 0x96,    /**< 'Right OYAYUBI' key. */

  /**
   * Left and right Alt, Ctrl and Shift. Windows uses them only as parameters to
   * GetAsyncKeyState and GetKeyState.
   */
  kLShift = 0xA0,
  kRShift = 0xA1,
  kLControl = 0xA2,
  kRControl = 0xA3,
  kLMenu = 0xA4,
  kRMenu = 0xA5,

  kBrowserBack = 0xA6,
  kBrowserForward = 0xA7,
  kBrowserRefresh = 0xA8,
  kBrowserStop = 0xA9,
  kBrowserSearch = 0xAA,
  kBrowserFavorites = 0xAB,
  kBrowserHome = 0xAC,

  kVolumeMute = 0xAD,
  kVolumeDown = 0xAE,
  kVolumeUp = 0xAF,
  kMediaNextTrack = 0xB0,
  kMediaPrevTrack = 0xB1,
  kMediaStop = 0xB2,
  kMediaPlayPause = 0xB3,
  kLaunchMail = 0xB4,
  kLaunchMediaSelect = 0xB5,
  kLaunchApp1 = 0xB6,
  kLaunchApp2 = 0xB7,

  kOem1 = 0xBA,      /**< ';:' for the US. */
  kOemPlus = 0xBB,   /**< '+' for any country. */
  kOemComma = 0xBC,  /**< ',' for any country. */
  kOemMinus = 0xBD,  /**< '-' for any country. */
  kOemPeriod = 0xBE, /**< '.' for any country. */
  kOem2 = 0xBF,      /**< '/?' for the US. */
  kOem3 = 0xC0,      /**< The grave accent and tilde key for the US. */

  /** VK_GAMEPAD_* codes added in _WIN32_WINNT 0x0604, reserved in WinUser.h. */
  kXboxOneGamepadA = 0xC3,
  kXboxOneGamepadB = 0xC4,
  kXboxOneGamepadX = 0xC5,
  kXboxOneGamepadY = 0xC6,
  kXboxOneGamepadRightShoulder = 0xC7,
  kXboxOneGamepadLeftShoulder = 0xC8,
  kXboxOneGamepadLeftTrigger = 0xC9,
  kXboxOneGamepadRightTrigger = 0xCA,
  kXboxOneGamepadDpadUp = 0xCB,
  kXboxOneGamepadDpadDown = 0xCC,
  kXboxOneGamepadDpadLeft = 0xCD,
  kXboxOneGamepadDpadRight = 0xCE,
  kXboxOneGamepadMenu = 0xCF,
  kXboxOneGamepadView = 0xD0,
  kXboxOneGamepadLeftThumbstickButton = 0xD1,
  kXboxOneGamepadRightThumbstickButton = 0xD2,
  kXboxOneGamepadLeftThumbstickUp = 0xD3,
  kXboxOneGamepadLeftThumbstickDown = 0xD4,
  kXboxOneGamepadLeftThumbstickRight = 0xD5,
  kXboxOneGamepadLeftThumbstickLeft = 0xD6,
  kXboxOneGamepadRightThumbstickUp = 0xD7,
  kXboxOneGamepadRightThumbstickDown = 0xD8,
  kXboxOneGamepadRightThumbstickRight = 0xD9,
  kXboxOneGamepadRightThumbstickLeft = 0xDA,

  kOem4 = 0xDB, /**< '[{' for the US. */
  kOem5 = 0xDC, /**< '\|' for the US. */
  kOem6 = 0xDD, /**< ']}' for the US. */
  kOem7 = 0xDE, /**< ''"' for the US. */
  kOem8 = 0xDF, /**< Miscellaneous character key, varies by keyboard. */

  kOemAx = 0xE1,   /**< 'AX' key on the Japanese AX keyboard. */
  kOem102 = 0xE2,  /**< "<>" or "\|" on the RT 102 key keyboard. */
  kIcoHelp = 0xE3, /**< Help key on the Olivetti keyboard (ICO). */
  kIco00 = 0xE4,   /**< 00 key on the ICO. */

  kProcessKey = 0xE5,

  kIcoClear = 0xE6, /**< Clear key on the ICO. */

  /**
   * Passes Unicode characters as keystrokes. The low word of a 32 bit key value for non keyboard
   * input methods.
   */
  kPacket = 0xE7,

  /** Nokia/Ericsson. */
  kOemReset = 0xE9,
  kOemJump = 0xEA,
  kOemPa1 = 0xEB,
  kOemPa2 = 0xEC,
  kOemPa3 = 0xED,
  kOemWsCtrl = 0xEE,
  kOemCuSel = 0xEF,
  kOemAttn = 0xF0,
  kOemFinish = 0xF1,
  kOemCopy = 0xF2,
  kOemAuto = 0xF3,
  kOemEnlW = 0xF4, /**< OEM Enlarge Window key. */
  kOemBackTab = 0xF5,

  kAttn = 0xF6,
  kCrSel = 0xF7,
  kExSel = 0xF8,
  kErEof = 0xF9,
  kPlay = 0xFA,
  kZoom = 0xFB,
  kNoName = 0xFC, /**< Reserved code with no key. */
  kPa1 = 0xFD,
  kOemClear = 0xFE, /**< Clear key, OEM specific. */

  /** VK_PAD_* from XInput.h for XInputGetKeystroke. */

  kXInputPadA = 0x5800,
  kXInputPadB = 0x5801,
  kXInputPadX = 0x5802,
  kXInputPadY = 0x5803,
  kXInputPadRShoulder = 0x5804,
  kXInputPadLShoulder = 0x5805,
  kXInputPadLTrigger = 0x5806,
  kXInputPadRTrigger = 0x5807,

  kXInputPadDPadUp = 0x5810,
  kXInputPadDPadDown = 0x5811,
  kXInputPadDPadLeft = 0x5812,
  kXInputPadDPadRight = 0x5813,
  kXInputPadStart = 0x5814,
  kXInputPadBack = 0x5815,
  kXInputPadLThumbPress = 0x5816,
  kXInputPadRThumbPress = 0x5817,

  kXInputPadLThumbUp = 0x5820,
  kXInputPadLThumbDown = 0x5821,
  kXInputPadLThumbRight = 0x5822,
  kXInputPadLThumbLeft = 0x5823,
  kXInputPadLThumbUpLeft = 0x5824,
  kXInputPadLThumbUpRight = 0x5825,
  kXInputPadLThumbDownRight = 0x5826,
  kXInputPadLThumbDownLeft = 0x5827,

  kXInputPadRThumbUp = 0x5830,
  kXInputPadRThumbDown = 0x5831,
  kXInputPadRThumbRight = 0x5832,
  kXInputPadRThumbLeft = 0x5833,
  kXInputPadRThumbUpLeft = 0x5834,
  kXInputPadRThumbUpRight = 0x5835,
  kXInputPadRThumbDownRight = 0x5836,
  kXInputPadRThumbDownLeft = 0x5837,
};

/**
 * Parses a key name.
 *
 * @param name A key name such as "F3", "Backtick", "A" or "Escape".
 * @return The key, or VirtualKey::kNone for an unknown name.
 */
REX_HID_API VirtualKey ParseVirtualKey(std::string_view name);

/**
 * @return The name ParseVirtualKey accepts, or empty for a key with no name.
 */
REX_HID_API std::string VirtualKeyToString(VirtualKey key);

using rex::HasFlags;
using rex::operator|;
using rex::operator&;
using rex::operator^;
using rex::operator~;
using rex::operator|=;
using rex::operator&=;
using rex::operator^=;

enum class PointerMode : u8 {
  kFree,   /**< The arrow shows and moves freely. */
  kHidden, /**< The arrow hides, the title draws its own. */
  kLocked, /**< The arrow hides and stays in the window, motion arrives as deltas. */
};

enum class MouseButton : u8 {
  kLeft,
  kRight,
  kMiddle,
  kX1,
  kX2,
  kNone, /**< A button the SDK has no name for, which only moves the pointer. */
};

enum class KeyModifiers : u8 {
  kNone = 0,
  kShift = 1,
  kCtrl = 2,
  kAlt = 4,
};

struct PointerState {
  /** X in physical window pixels. */
  f32 x = 0.0f;
  /** Y in physical window pixels. */
  f32 y = 0.0f;
  /** Window width in physical pixels, 0 before the first resize event. */
  u32 window_width = 0;
  /** Window height in physical pixels, 0 before the first resize event. */
  u32 window_height = 0;
  /** True once a motion event arrived outside Locked mode. */
  bool has_position = false;
};

struct PointerDelta {
  /** Horizontal motion in physical pixels. */
  f32 dx = 0.0f;
  /** Vertical motion in physical pixels. */
  f32 dy = 0.0f;
};

/**
 * A window input event. The UI provider builds it, the UI system passes it to the listeners,
 * overlays and binds, then forwards it to the input system.
 */
struct InputEvent {
  enum class Type : u8 {
    kKeyDown, /**< A key went down or repeated. */
    kKeyUp,
    kKeyChar,
    kButtonDown,
    kButtonUp,
    kMotion,
    kWheel,
    kFocusGained,
    kFocusLost,
    kResize,
  };

  Type type = Type::kKeyDown;
  /** The key of KeyDown and KeyUp. */
  VirtualKey key = VirtualKey::kNone;
  /** The modifiers held for KeyDown and KeyUp. */
  KeyModifiers modifiers = KeyModifiers::kNone;
  /** True for an auto repeated KeyDown. */
  bool repeat = false;
  /** The code point of KeyChar. */
  u32 character = 0;
  /** The button of ButtonDown and ButtonUp. */
  MouseButton button = MouseButton::kLeft;
  /** Pointer x in physical pixels. */
  f32 x = 0.0f;
  /** Pointer y in physical pixels. */
  f32 y = 0.0f;
  /** Relative motion in physical pixels, valid under a relative pointer lock. */
  f32 dx = 0.0f;
  /** Relative motion in physical pixels, valid under a relative pointer lock. */
  f32 dy = 0.0f;
  /** Horizontal wheel turn in detents, fractional for a smooth wheel. */
  f32 scroll_x = 0.0f;
  /** Vertical wheel turn in detents, fractional for a smooth wheel, positive away from the user. */
  f32 scroll_y = 0.0f;
  /** Window width of Resize in physical pixels. */
  u32 width = 0;
  /** Window height of Resize in physical pixels. */
  u32 height = 0;
  /** True once a receiver consumed the event, so later receivers skip it. */
  bool handled = false;
};

}  // namespace rex::hid

template <>
inline constexpr bool rex::is_flags_enum<rex::hid::KeyModifiers> = true;

namespace rex::hid {

/** The types of input a binding can name. Mouse buttons are keys with their VirtualKey. */
enum class InputSourceType : u8 {
  kKey,        /**< A keyboard key or mouse button, code is a VirtualKey. */
  kMouseWheel, /**< A mouse wheel direction, sign gives the direction. */
  kPadButton,  /**< A pad button or a stick direction as a button, code is a PadInput. */
  kPadAxes,    /**< A whole pad stick, code is a PadAxes. */
};

/** Pad buttons in the order input sources and capture masks number them. */
enum class PadInput : u8 {
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kStart,
  kBack,
  kLeftStickPress,
  kRightStickPress,
  kA,
  kB,
  kX,
  kY,
  kLeftTrigger,
  kRightTrigger,
  kLeftShoulder,
  kRightShoulder,
  kLeftStickUp,
  kLeftStickDown,
  kLeftStickRight,
  kLeftStickLeft,
  kRightStickUp,
  kRightStickDown,
  kRightStickRight,
  kRightStickLeft,
};

inline constexpr u8 kPadButtonCount = 24;

enum class PadAxes : u8 {
  kNone,
  kLeft,
  kRight,
};

struct InputSource {
  InputSourceType type = InputSourceType::kKey;
  /** A VirtualKey, PadInput or PadAxes by type, 0 for a wheel. */
  u16 code = 0;
  /** The modifiers held with a key or wheel input, ignored for pad inputs. */
  KeyModifiers modifiers = KeyModifiers::kNone;
  /** The wheel direction, 1 for up and -1 for down, 0 for other types. */
  i8 sign = 0;
  /** The 1 based key half of a stick row this input fills, 0 outside a stick row. */
  u8 axis_slot = 0;

  /** Compares two sources field by field. */
  constexpr bool operator==(const InputSource& other) const = default;
};

/**
 * Compares what two sources read, ignoring the stick row slot.
 *
 * @return True when type, code, modifiers and sign are equal.
 */
constexpr bool IsSameInput(const InputSource& left, const InputSource& right) {
  return left.type == right.type && left.code == right.code && left.modifiers == right.modifiers &&
         left.sign == right.sign;
}

/** Returns the name of a pad button in binding text, or empty for a value outside PadInput. */
REX_HID_API std::string_view GetPadButtonName(PadInput button);

/**
 * Parses one input such as "Shift+W", "LMB", "WheelUp", "PadA" or "LStick". Key text follows
 * ParseKeyChord and pad names take no modifiers. Names are case sensitive.
 *
 * @return The source, or ErrorCategory::kConfig naming text when it is empty, a bare modifier or
 *         names nothing.
 */
[[nodiscard]] REX_HID_API Result<InputSource> ParseInputSource(std::string_view text);

/**
 * Writes a source as binding text, modifiers in the order Shift, Ctrl, Alt and then the name.
 *
 * @return The text ParseInputSource reads back, or empty when the source names nothing.
 */
REX_HID_API std::string InputSourceToString(const InputSource& source);

/** A key and the exact modifiers that must be held with it. */
struct KeyChord {
  /** The key, VirtualKey::kNone for an unbound chord. */
  VirtualKey key = VirtualKey::kNone;
  /** The modifiers held with the key, no more and no fewer. */
  KeyModifiers modifiers = KeyModifiers::kNone;

  constexpr bool operator==(const KeyChord& other) const = default;
};

/**
 * Parses a chord such as "F3", "Shift+Up" or "Ctrl+Alt+K". The prefixes Shift, Ctrl, Control and
 * Alt come in any order, then exactly one key name that ParseVirtualKey accepts. Names are case
 * sensitive. An empty string is the unbound chord.
 *
 * @return The chord, or ErrorCategory::kConfig naming text when the key is unknown, missing or a
 *         bare modifier.
 */
[[nodiscard]] REX_HID_API Result<KeyChord> ParseKeyChord(std::string_view text);

/**
 * Parses a comma separated list of chords such as "Q,I" or "Shift+W, A". Spaces and tabs around
 * an item are discarded and empty items are skipped.
 *
 * @return The chords in order, or the error of the first item ParseKeyChord rejects.
 */
[[nodiscard]] REX_HID_API Result<std::vector<KeyChord>> ParseKeyChordList(std::string_view text);

/**
 * Writes a chord as text, modifiers in the order Shift, Ctrl, Alt and then the key name.
 *
 * @return The text ParseKeyChord reads back, or empty when the key is VirtualKey::kNone or has
 *         no name.
 */
REX_HID_API std::string KeyChordToString(KeyChord chord);

}  // namespace rex::hid
