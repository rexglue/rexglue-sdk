/**
 * @file        hid/event.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/bit.h>
#include <rex/hid/virtual_key.h>
#include <rex/types.h>

namespace rex::hid {

using rex::HasFlags;
using rex::operator|;
using rex::operator&;
using rex::operator^;
using rex::operator~;
using rex::operator|=;
using rex::operator&=;
using rex::operator^=;

/** How the window treats the pointer. */
enum class PointerMode : u8 {
  kFree,   /**< The arrow shows and moves freely. */
  kHidden, /**< The arrow hides, the title draws its own. */
  kLocked, /**< The arrow hides and stays in the window, motion arrives as deltas. */
};

/** A mouse button. */
enum class MouseButton : u8 {
  kLeft,   /**< The left button. */
  kRight,  /**< The right button. */
  kMiddle, /**< The middle button. */
  kX1,     /**< The first extra button. */
  kX2,     /**< The second extra button. */
};

/** Keyboard modifiers held during an event. */
enum class KeyModifiers : u8 {
  kNone = 0,  /**< No modifier. */
  kShift = 1, /**< Shift. */
  kCtrl = 2,  /**< Ctrl. */
  kAlt = 4,   /**< Alt. */
};

/** The pointer position and the window size. */
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

/** Pointer motion. */
struct PointerDelta {
  /** Horizontal motion in physical pixels. */
  f32 dx = 0.0f;
  /** Vertical motion in physical pixels. */
  f32 dy = 0.0f;
};

/** 1 window input event, forwarded by the input host after its overlays had it. */
struct InputEvent {
  /** Event kinds. */
  enum class Kind : u8 {
    kKeyDown,     /**< A key went down or repeated. */
    kKeyUp,       /**< A key went up. */
    kKeyChar,     /**< A character was typed. */
    kButtonDown,  /**< A mouse button went down. */
    kButtonUp,    /**< A mouse button went up. */
    kMotion,      /**< The pointer moved. */
    kWheel,       /**< The wheel turned. */
    kFocusGained, /**< The window gained focus. */
    kFocusLost,   /**< The window lost focus. */
    kResize,      /**< The window changed size. */
  };

  /** The kind. */
  Kind kind = Kind::kKeyDown;
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
  /** Signed wheel detents, positive away from the user. */
  i32 wheel = 0;
  /** Window width of Resize in physical pixels. */
  u32 width = 0;
  /** Window height of Resize in physical pixels. */
  u32 height = 0;
};

}  // namespace rex::hid

/** Enables the flag operators on rex::hid::KeyModifiers. */
template <>
inline constexpr bool rex::is_flags_enum<rex::hid::KeyModifiers> = true;
