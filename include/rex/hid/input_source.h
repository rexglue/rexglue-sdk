/**
 * @file        hid/input_source.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/types.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/hid/event.h>
#include <rex/hid/virtual_key.h>

namespace rex::hid {

/** The kinds of input a binding can name. Mouse buttons are keys with their VirtualKey. */
enum class InputSourceKind : u8 {
  kKey,        /**< A keyboard key or mouse button, code is a VirtualKey. */
  kMouseWheel, /**< A mouse wheel direction, sign gives the direction. */
  kPadButton,  /**< A pad button or a stick direction as a button, code is a PadButton. */
  kPadAxes,    /**< A whole pad stick, code is a PadAxes. */
};

/** Pad buttons in the order input sources and capture masks number them. */
enum class PadButton : u8 {
  kDpadUp,          /**< D-pad up. */
  kDpadDown,        /**< D-pad down. */
  kDpadLeft,        /**< D-pad left. */
  kDpadRight,       /**< D-pad right. */
  kStart,           /**< Start. */
  kBack,            /**< Back. */
  kLeftStickPress,  /**< Left stick press. */
  kRightStickPress, /**< Right stick press. */
  kA,               /**< A. */
  kB,               /**< B. */
  kX,               /**< X. */
  kY,               /**< Y. */
  kLeftTrigger,     /**< Left trigger. */
  kRightTrigger,    /**< Right trigger. */
  kLeftShoulder,    /**< Left shoulder. */
  kRightShoulder,   /**< Right shoulder. */
  kLeftStickUp,     /**< Left stick pushed up. */
  kLeftStickDown,   /**< Left stick pushed down. */
  kLeftStickRight,  /**< Left stick pushed right. */
  kLeftStickLeft,   /**< Left stick pushed left. */
  kRightStickUp,    /**< Right stick pushed up. */
  kRightStickDown,  /**< Right stick pushed down. */
  kRightStickRight, /**< Right stick pushed right. */
  kRightStickLeft,  /**< Right stick pushed left. */
};

/** Number of PadButton values. */
inline constexpr u8 kPadButtonCount = 24;

/** The pad sticks an axis binding can name. */
enum class PadAxes : u8 {
  kNone,  /**< No stick. */
  kLeft,  /**< The left stick. */
  kRight, /**< The right stick. */
};

/** One input a binding names. */
struct InputSource {
  /** What the input is. */
  InputSourceKind kind = InputSourceKind::kKey;
  /** A VirtualKey, PadButton or PadAxes by kind, 0 for a wheel. */
  u16 code = 0;
  /** The modifiers held with a key or wheel input, ignored for pad inputs. */
  KeyModifiers modifiers = KeyModifiers::kNone;
  /** The wheel direction, 1 for up and -1 for down, 0 for other kinds. */
  i8 sign = 0;
  /** The 1 based key half of a stick row this input fills, 0 outside a stick row. */
  u8 axis_slot = 0;

  /**
   * Compares 2 sources field by field.
   *
   * @param other The source to compare with.
   * @return True when every field is equal.
   */
  constexpr bool operator==(const InputSource& other) const = default;
};

/**
 * Compares what 2 sources read, ignoring the stick row slot.
 *
 * @param left The first source.
 * @param right The second source.
 * @return True when kind, code, modifiers and sign are equal.
 */
constexpr bool IsSameInput(const InputSource& left, const InputSource& right) {
  return left.kind == right.kind && left.code == right.code && left.modifiers == right.modifiers &&
         left.sign == right.sign;
}

/**
 * Returns the name of a pad button in binding text.
 *
 * @param button The pad button.
 * @return The name such as "PadA" or "LStickUp", or empty for a value outside PadButton.
 */
REX_API std::string_view GetPadButtonName(PadButton button);

/**
 * Parses 1 input such as "Shift+W", "LMB", "WheelUp", "PadA" or "LStick". Key text follows
 * ParseKeyChord and pad names take no modifiers. Names are case sensitive.
 *
 * @param text The input text.
 * @return The source, or ErrorCategory::kConfig naming text when it is empty, a bare modifier or
 *         names nothing.
 */
[[nodiscard]] REX_API Result<InputSource> ParseInputSource(std::string_view text);

/**
 * Writes a source as binding text, modifiers in the order Shift, Ctrl, Alt and then the name.
 *
 * @param source The source.
 * @return The text ParseInputSource reads back, or empty when the source names nothing.
 */
REX_API std::string InputSourceToString(const InputSource& source);

}  // namespace rex::hid
