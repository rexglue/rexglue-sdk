/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>

namespace rex::hid {

/**
 * Windows and XInput virtual key enumeration. Platform keys translate to it for HID keystroke
 * emulation and UI events. The type is u16 as WPARAM and XINPUT_KEYSTROKE store the key as a WORD,
 * though kPacket can use bits above 16. VK_ codes on Windows are defined up to 0xFF and XInput
 * codes are 16 bit. Base codes follow _WIN32_WINNT 0x0500, later codes are marked as such.
 */
enum class VirtualKey : u16 {
  kNone, /**< Not a valid key, MapVirtualKey has no translation. */

  kLButton = 0x01, /**< Left mouse button. */
  kRButton = 0x02, /**< Right mouse button. */
  kCancel = 0x03,  /**< Control-break. */
  kMButton = 0x04, /**< Middle mouse button. */

  kXButton1 = 0x05, /**< First extra mouse button. */
  kXButton2 = 0x06, /**< Second extra mouse button. */

  kBack = 0x08, /**< Backspace. */
  kTab = 0x09,  /**< Tab key. */

  kClear = 0x0C,     /**< Clear key. */
  kReturn = 0x0D,    /**< Enter. */
  kWheelUp = 0x0E,   /**< Mouse wheel scrolled up. */
  kWheelDown = 0x0F, /**< Mouse wheel scrolled down. */

  kShift = 0x10,   /**< Either Shift key. */
  kControl = 0x11, /**< Ctrl. */
  kMenu = 0x12,    /**< Alt. */
  kPause = 0x13,   /**< Pause key. */
  kCapital = 0x14, /**< Caps Lock. */

  kKana = 0x15,    /**< IME Kana mode, Japanese keyboards. */
  kHangeul = 0x15, /**< IME Hangul mode, same code as kKana. */
  kHangul = 0x15,  /**< IME Hangul mode, same code as kKana. */
  kImeOn = 0x16,   /**< IME on. */
  kJunja = 0x17,   /**< IME Junja mode. */
  kFinal = 0x18,   /**< IME final mode. */
  kHanja = 0x19,   /**< IME Hanja mode, Korean keyboards. */
  kKanji = 0x19,   /**< IME Kanji mode, same code as kHanja. */
  kImeOff = 0x1A,  /**< IME off. */

  kEscape = 0x1B, /**< Esc key. */

  kConvert = 0x1C,    /**< IME convert. */
  kNonConvert = 0x1D, /**< IME nonconvert. */
  kAccept = 0x1E,     /**< IME accept. */
  kModeChange = 0x1F, /**< IME mode change request. */

  kSpace = 0x20,    /**< Space bar. */
  kPrior = 0x21,    /**< Page Up. */
  kNext = 0x22,     /**< Page Down. */
  kEnd = 0x23,      /**< End key. */
  kHome = 0x24,     /**< Home key. */
  kLeft = 0x25,     /**< Left arrow key. */
  kUp = 0x26,       /**< Up arrow key. */
  kRight = 0x27,    /**< Right arrow key. */
  kDown = 0x28,     /**< Down arrow key. */
  kSelect = 0x29,   /**< Select key. */
  kPrint = 0x2A,    /**< Print key. */
  kExecute = 0x2B,  /**< Execute key. */
  kSnapshot = 0x2C, /**< Print Screen key. */
  kInsert = 0x2D,   /**< Ins key. */
  kDelete = 0x2E,   /**< Del key. */
  kHelp = 0x2F,     /**< Help key. */

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

  kLWin = 0x5B, /**< Left Windows key. */
  kRWin = 0x5C, /**< Right Windows key. */
  kApps = 0x5D, /**< Applications key that opens the context menu. */

  kSleep = 0x5F, /**< Computer Sleep key. */

  kNumpad0 = 0x60,   /**< Numeric keypad 0 key. */
  kNumpad1 = 0x61,   /**< Numeric keypad 1 key. */
  kNumpad2 = 0x62,   /**< Numeric keypad 2 key. */
  kNumpad3 = 0x63,   /**< Numeric keypad 3 key. */
  kNumpad4 = 0x64,   /**< Numeric keypad 4 key. */
  kNumpad5 = 0x65,   /**< Numeric keypad 5 key. */
  kNumpad6 = 0x66,   /**< Numeric keypad 6 key. */
  kNumpad7 = 0x67,   /**< Numeric keypad 7 key. */
  kNumpad8 = 0x68,   /**< Numeric keypad 8 key. */
  kNumpad9 = 0x69,   /**< Numeric keypad 9 key. */
  kMultiply = 0x6A,  /**< Numeric keypad multiply key. */
  kAdd = 0x6B,       /**< Numeric keypad add key. */
  kSeparator = 0x6C, /**< Numeric keypad separator key. */
  kSubtract = 0x6D,  /**< Numeric keypad subtract key. */
  kDecimal = 0x6E,   /**< Numeric keypad decimal key. */
  kDivide = 0x6F,    /**< Numeric keypad divide key. */
  kF1 = 0x70,        /**< Function key F1. */
  kF2 = 0x71,        /**< Function key F2. */
  kF3 = 0x72,        /**< Function key F3. */
  kF4 = 0x73,        /**< Function key F4. */
  kF5 = 0x74,        /**< Function key F5. */
  kF6 = 0x75,        /**< Function key F6. */
  kF7 = 0x76,        /**< Function key F7. */
  kF8 = 0x77,        /**< Function key F8. */
  kF9 = 0x78,        /**< Function key F9. */
  kF10 = 0x79,       /**< Function key F10. */
  kF11 = 0x7A,       /**< Function key F11. */
  kF12 = 0x7B,       /**< Function key F12. */
  kF13 = 0x7C,       /**< Function key F13. */
  kF14 = 0x7D,       /**< Function key F14. */
  kF15 = 0x7E,       /**< Function key F15. */
  kF16 = 0x7F,       /**< Function key F16. */
  kF17 = 0x80,       /**< Function key F17. */
  kF18 = 0x81,       /**< Function key F18. */
  kF19 = 0x82,       /**< Function key F19. */
  kF20 = 0x83,       /**< Function key F20. */
  kF21 = 0x84,       /**< Function key F21. */
  kF22 = 0x85,       /**< Function key F22. */
  kF23 = 0x86,       /**< Function key F23. */
  kF24 = 0x87,       /**< Function key F24. */

  /** VK_NAVIGATION_* added in _WIN32_WINNT 0x0604, reserved in WinUser.h. */
  kNavigationView = 0x88,   /**< VK_NAVIGATION_VIEW. */
  kNavigationMenu = 0x89,   /**< VK_NAVIGATION_MENU. */
  kNavigationUp = 0x8A,     /**< VK_NAVIGATION_UP. */
  kNavigationDown = 0x8B,   /**< VK_NAVIGATION_DOWN. */
  kNavigationLeft = 0x8C,   /**< VK_NAVIGATION_LEFT. */
  kNavigationRight = 0x8D,  /**< VK_NAVIGATION_RIGHT. */
  kNavigationAccept = 0x8E, /**< VK_NAVIGATION_ACCEPT. */
  kNavigationCancel = 0x8F, /**< VK_NAVIGATION_CANCEL. */

  kNumLock = 0x90, /**< Num Lock key. */
  kScroll = 0x91,  /**< Scroll Lock key. */

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
  kLShift = 0xA0,   /**< Left Shift key. */
  kRShift = 0xA1,   /**< Right Shift key. */
  kLControl = 0xA2, /**< Left Ctrl key. */
  kRControl = 0xA3, /**< Right Ctrl key. */
  kLMenu = 0xA4,    /**< Left Alt key. */
  kRMenu = 0xA5,    /**< Right Alt key. */

  kBrowserBack = 0xA6,      /**< Browser Back key. */
  kBrowserForward = 0xA7,   /**< Browser Forward key. */
  kBrowserRefresh = 0xA8,   /**< Browser Refresh key. */
  kBrowserStop = 0xA9,      /**< Browser Stop key. */
  kBrowserSearch = 0xAA,    /**< Browser Search key. */
  kBrowserFavorites = 0xAB, /**< Browser Favorites key. */
  kBrowserHome = 0xAC,      /**< Browser Start and Home key. */

  kVolumeMute = 0xAD,        /**< Volume Mute key. */
  kVolumeDown = 0xAE,        /**< Volume Down key. */
  kVolumeUp = 0xAF,          /**< Volume Up key. */
  kMediaNextTrack = 0xB0,    /**< Next Track key. */
  kMediaPrevTrack = 0xB1,    /**< Previous Track key. */
  kMediaStop = 0xB2,         /**< Stop Media key. */
  kMediaPlayPause = 0xB3,    /**< Play and Pause Media key. */
  kLaunchMail = 0xB4,        /**< Start Mail key. */
  kLaunchMediaSelect = 0xB5, /**< Select Media key. */
  kLaunchApp1 = 0xB6,        /**< Start Application 1 key. */
  kLaunchApp2 = 0xB7,        /**< Start Application 2 key. */

  kOem1 = 0xBA,      /**< ';:' for the US. */
  kOemPlus = 0xBB,   /**< '+' for any country. */
  kOemComma = 0xBC,  /**< ',' for any country. */
  kOemMinus = 0xBD,  /**< '-' for any country. */
  kOemPeriod = 0xBE, /**< '.' for any country. */
  kOem2 = 0xBF,      /**< '/?' for the US. */
  kOem3 = 0xC0,      /**< '`~' for the US. */

  /**
   * VK_GAMEPAD_* codes added in _WIN32_WINNT 0x0604, reserved in WinUser.h. VK_PAD_*
   * (kXInputPad*) serve HID emulation because XInput is the controller API. The 2 name prefixes
   * tell VK_GAMEPAD_* and VK_PAD_* apart.
   */
  kXboxOneGamepadA = 0xC3,                     /**< VK_GAMEPAD_A. */
  kXboxOneGamepadB = 0xC4,                     /**< VK_GAMEPAD_B. */
  kXboxOneGamepadX = 0xC5,                     /**< VK_GAMEPAD_X. */
  kXboxOneGamepadY = 0xC6,                     /**< VK_GAMEPAD_Y. */
  kXboxOneGamepadRightShoulder = 0xC7,         /**< VK_GAMEPAD_RIGHT_SHOULDER. */
  kXboxOneGamepadLeftShoulder = 0xC8,          /**< VK_GAMEPAD_LEFT_SHOULDER. */
  kXboxOneGamepadLeftTrigger = 0xC9,           /**< VK_GAMEPAD_LEFT_TRIGGER. */
  kXboxOneGamepadRightTrigger = 0xCA,          /**< VK_GAMEPAD_RIGHT_TRIGGER. */
  kXboxOneGamepadDpadUp = 0xCB,                /**< VK_GAMEPAD_DPAD_UP. */
  kXboxOneGamepadDpadDown = 0xCC,              /**< VK_GAMEPAD_DPAD_DOWN. */
  kXboxOneGamepadDpadLeft = 0xCD,              /**< VK_GAMEPAD_DPAD_LEFT. */
  kXboxOneGamepadDpadRight = 0xCE,             /**< VK_GAMEPAD_DPAD_RIGHT. */
  kXboxOneGamepadMenu = 0xCF,                  /**< VK_GAMEPAD_MENU. */
  kXboxOneGamepadView = 0xD0,                  /**< VK_GAMEPAD_VIEW. */
  kXboxOneGamepadLeftThumbstickButton = 0xD1,  /**< VK_GAMEPAD_LEFT_THUMBSTICK_BUTTON. */
  kXboxOneGamepadRightThumbstickButton = 0xD2, /**< VK_GAMEPAD_RIGHT_THUMBSTICK_BUTTON. */
  kXboxOneGamepadLeftThumbstickUp = 0xD3,      /**< VK_GAMEPAD_LEFT_THUMBSTICK_UP. */
  kXboxOneGamepadLeftThumbstickDown = 0xD4,    /**< VK_GAMEPAD_LEFT_THUMBSTICK_DOWN. */
  kXboxOneGamepadLeftThumbstickRight = 0xD5,   /**< VK_GAMEPAD_LEFT_THUMBSTICK_RIGHT. */
  kXboxOneGamepadLeftThumbstickLeft = 0xD6,    /**< VK_GAMEPAD_LEFT_THUMBSTICK_LEFT. */
  kXboxOneGamepadRightThumbstickUp = 0xD7,     /**< VK_GAMEPAD_RIGHT_THUMBSTICK_UP. */
  kXboxOneGamepadRightThumbstickDown = 0xD8,   /**< VK_GAMEPAD_RIGHT_THUMBSTICK_DOWN. */
  kXboxOneGamepadRightThumbstickRight = 0xD9,  /**< VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT. */
  kXboxOneGamepadRightThumbstickLeft = 0xDA,   /**< VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT. */

  kOem4 = 0xDB, /**< '[{' for the US. */
  kOem5 = 0xDC, /**< '\|' for the US. */
  kOem6 = 0xDD, /**< ']}' for the US. */
  kOem7 = 0xDE, /**< ''"' for the US. */
  kOem8 = 0xDF, /**< Miscellaneous character key, varies by keyboard. */

  kOemAx = 0xE1,   /**< 'AX' key on the Japanese AX keyboard. */
  kOem102 = 0xE2,  /**< "<>" or "\|" on the RT 102-key keyboard. */
  kIcoHelp = 0xE3, /**< Help key on the Olivetti keyboard (ICO). */
  kIco00 = 0xE4,   /**< 00 key on the ICO. */

  kProcessKey = 0xE5, /**< IME Process key. */

  kIcoClear = 0xE6, /**< Clear key on the ICO. */

  /**
   * Passes Unicode characters as keystrokes. The low word of a 32 bit key value for non keyboard
   * input methods.
   */
  kPacket = 0xE7,

  /** Nokia/Ericsson. */
  kOemReset = 0xE9,   /**< OEM reset key. */
  kOemJump = 0xEA,    /**< OEM jump key. */
  kOemPa1 = 0xEB,     /**< OEM PA1 key. */
  kOemPa2 = 0xEC,     /**< OEM PA2 key. */
  kOemPa3 = 0xED,     /**< OEM PA3 key. */
  kOemWsCtrl = 0xEE,  /**< OEM WsCtrl key. */
  kOemCuSel = 0xEF,   /**< OEM CuSel key. */
  kOemAttn = 0xF0,    /**< OEM Attn key. */
  kOemFinish = 0xF1,  /**< OEM Finish key. */
  kOemCopy = 0xF2,    /**< OEM Copy key. */
  kOemAuto = 0xF3,    /**< OEM Auto key. */
  kOemEnlW = 0xF4,    /**< OEM Enlarge Window key. */
  kOemBackTab = 0xF5, /**< OEM Back Tab key. */

  kAttn = 0xF6,     /**< Attn key. */
  kCrSel = 0xF7,    /**< CrSel key. */
  kExSel = 0xF8,    /**< ExSel key. */
  kErEof = 0xF9,    /**< Erase EOF key. */
  kPlay = 0xFA,     /**< Play key. */
  kZoom = 0xFB,     /**< Zoom key. */
  kNoName = 0xFC,   /**< Reserved code with no key. */
  kPa1 = 0xFD,      /**< PA1 key. */
  kOemClear = 0xFE, /**< Clear key, OEM specific. */

  /**
   * VK_PAD_* from XInput.h for XInputGetKeystroke. The kXInput prefix tells them apart from
   * VK_GAMEPAD_*.
   */

  kXInputPadA = 0x5800,         /**< VK_PAD_A. */
  kXInputPadB = 0x5801,         /**< VK_PAD_B. */
  kXInputPadX = 0x5802,         /**< VK_PAD_X. */
  kXInputPadY = 0x5803,         /**< VK_PAD_Y. */
  kXInputPadRShoulder = 0x5804, /**< Numbered before kXInputPadLShoulder. */
  kXInputPadLShoulder = 0x5805, /**< VK_PAD_LSHOULDER. */
  kXInputPadLTrigger = 0x5806,  /**< VK_PAD_LTRIGGER. */
  kXInputPadRTrigger = 0x5807,  /**< VK_PAD_RTRIGGER. */

  kXInputPadDpadUp = 0x5810,      /**< VK_PAD_DPAD_UP. */
  kXInputPadDpadDown = 0x5811,    /**< VK_PAD_DPAD_DOWN. */
  kXInputPadDpadLeft = 0x5812,    /**< VK_PAD_DPAD_LEFT. */
  kXInputPadDpadRight = 0x5813,   /**< VK_PAD_DPAD_RIGHT. */
  kXInputPadStart = 0x5814,       /**< VK_PAD_START. */
  kXInputPadBack = 0x5815,        /**< VK_PAD_BACK. */
  kXInputPadLThumbPress = 0x5816, /**< VK_PAD_LTHUMB_PRESS. */
  kXInputPadRThumbPress = 0x5817, /**< VK_PAD_RTHUMB_PRESS. */

  kXInputPadLThumbUp = 0x5820,        /**< VK_PAD_LTHUMB_UP. */
  kXInputPadLThumbDown = 0x5821,      /**< VK_PAD_LTHUMB_DOWN. */
  kXInputPadLThumbRight = 0x5822,     /**< VK_PAD_LTHUMB_RIGHT. */
  kXInputPadLThumbLeft = 0x5823,      /**< VK_PAD_LTHUMB_LEFT. */
  kXInputPadLThumbUpLeft = 0x5824,    /**< VK_PAD_LTHUMB_UPLEFT. */
  kXInputPadLThumbUpRight = 0x5825,   /**< VK_PAD_LTHUMB_UPRIGHT. */
  kXInputPadLThumbDownRight = 0x5826, /**< VK_PAD_LTHUMB_DOWNRIGHT. */
  kXInputPadLThumbDownLeft = 0x5827,  /**< VK_PAD_LTHUMB_DOWNLEFT. */

  kXInputPadRThumbUp = 0x5830,        /**< VK_PAD_RTHUMB_UP. */
  kXInputPadRThumbDown = 0x5831,      /**< VK_PAD_RTHUMB_DOWN. */
  kXInputPadRThumbRight = 0x5832,     /**< VK_PAD_RTHUMB_RIGHT. */
  kXInputPadRThumbLeft = 0x5833,      /**< VK_PAD_RTHUMB_LEFT. */
  kXInputPadRThumbUpLeft = 0x5834,    /**< VK_PAD_RTHUMB_UPLEFT. */
  kXInputPadRThumbUpRight = 0x5835,   /**< VK_PAD_RTHUMB_UPRIGHT. */
  kXInputPadRThumbDownRight = 0x5836, /**< VK_PAD_RTHUMB_DOWNRIGHT. */
  kXInputPadRThumbDownLeft = 0x5837,  /**< VK_PAD_RTHUMB_DOWNLEFT. */
};

/**
 * Parses a key name.
 *
 * @param name A key name such as "F3", "Backtick", "A" or "Escape".
 * @return The key, or VirtualKey::kNone for an unknown name.
 */
REX_API VirtualKey ParseVirtualKey(std::string_view name);

/**
 * Returns the name of a key.
 *
 * @param key The key.
 * @return The name ParseVirtualKey accepts, or empty for a key with no name.
 */
REX_API std::string VirtualKeyToString(VirtualKey key);

}  // namespace rex::hid
