/**
 * @file        hid/pad.h
 * @brief       Pad state, capabilities and identity
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/types.h>

namespace rex::hid {

/** Button bits of PadState::buttons. The layout matches the title's input API. */
enum PadButton : u16 {
  kPadDPadUp = 0x0001,
  kPadDPadDown = 0x0002,
  kPadDPadLeft = 0x0004,
  kPadDPadRight = 0x0008,
  kPadStart = 0x0010,
  kPadBack = 0x0020,
  kPadLeftThumb = 0x0040,  /**< Left stick press. */
  kPadRightThumb = 0x0080, /**< Right stick press. */
  kPadLeftShoulder = 0x0100,
  kPadRightShoulder = 0x0200,
  kPadGuide = 0x0400,
  kPadA = 0x1000,
  kPadB = 0x2000,
  kPadX = 0x4000,
  kPadY = 0x8000,
};

/** One reading of a pad. Sticks are centered on 0, positive right and up. */
struct PadState {
  u16 buttons = 0;
  u8 left_trigger = 0;  /**< Pressure from 0 to 255. */
  u8 right_trigger = 0; /**< Pressure from 0 to 255. */
  i16 thumb_lx = 0;
  i16 thumb_ly = 0;
  i16 thumb_rx = 0;
  i16 thumb_ry = 0;
};

/** Motor speeds, 0 for off to 65535 for full. */
struct PadVibration {
  u16 left_motor = 0;  /**< Low frequency motor. */
  u16 right_motor = 0; /**< High frequency motor. */
};

struct PadCapabilities {
  u8 subtype = 0x01;   /**< The title API's device subtype, 0x01 for a gamepad. */
  u16 flags = 0;       /**< The title API's capability bits, such as wireless. */
  PadState supported;  /**< The buttons present, and each axis at its full resolution. */
  PadVibration motors; /**< Each motor present at its full resolution. */
};

/** One keystroke made from a pad button, a stick or a key. */
struct PadKeystroke {
  u16 virtual_key = 0; /**< The title API's virtual key. */
  u16 unicode = 0;     /**< The character typed, 0 when none. */
  u16 flags = 0;       /**< Key down, key up, repeat. */
  u8 user = 0;         /**< The user it came from, 0 to 3. */
  u8 hid_code = 0;     /**< The USB HID usage of a key, 0 when none. */
};

/** The pad family, for button glyphs. */
enum class PadType : u8 {
  kUnknown,
  kStandard,
  kX360,
  kXOne,
  kPS3,
  kPS4,
  kPS5,
  kSwitchPro,
  kSwitchJoyCon,
};

/** Which pad a user holds, for button glyphs. */
struct PadInfo {
  PadType type = PadType::kUnknown;
  u16 vendor = 0;  /**< USB vendor, 0 when unknown. */
  u16 product = 0; /**< USB product, 0 when unknown. */
};

/** Identifies a pad inside one driver. */
enum class PadDeviceID : u64 { kInvalid = 0 };

/**
 * Identifies the physical pad, so one pad seen by two drivers is used once. Devices match when
 * both have an XInput slot and the slots match, or both have a serial and vendor, product and
 * serial match.
 */
struct PadIdentity {
  i32 xinput_slot = -1; /**< The XInput user slot, -1 when not an XInput pad. */
  u16 vendor = 0;       /**< USB vendor, 0 when unknown. */
  u16 product = 0;      /**< USB product, 0 when unknown. */
  std::string serial;   /**< Serial number, empty when unknown. */
};

struct PadDevice {
  PadDeviceID id = PadDeviceID::kInvalid; /**< The driver's id for it. */
  std::string name;
  PadInfo info;
  PadIdentity identity;
};

}  // namespace rex::hid
