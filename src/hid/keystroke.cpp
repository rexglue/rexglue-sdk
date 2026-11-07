/**
 * @file        hid/keystroke.cpp
 * @brief       Keystrokes made from the merged pad state
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>

#include "hid/input_internal.h"

namespace rex::hid::detail {

namespace {

constexpr std::array<VirtualKey, 34> kKeyForBit = {
    VirtualKey::kXInputPadDPadUp,
    VirtualKey::kXInputPadDPadDown,
    VirtualKey::kXInputPadDPadLeft,
    VirtualKey::kXInputPadDPadRight,
    VirtualKey::kXInputPadStart,
    VirtualKey::kXInputPadBack,
    VirtualKey::kXInputPadLThumbPress,
    VirtualKey::kXInputPadRThumbPress,
    VirtualKey::kXInputPadLShoulder,
    VirtualKey::kXInputPadRShoulder,
    VirtualKey::kNone,
    VirtualKey::kNone,
    VirtualKey::kXInputPadA,
    VirtualKey::kXInputPadB,
    VirtualKey::kXInputPadX,
    VirtualKey::kXInputPadY,
    VirtualKey::kXInputPadLTrigger,
    VirtualKey::kXInputPadRTrigger,
    VirtualKey::kXInputPadLThumbUp,
    VirtualKey::kXInputPadLThumbDown,
    VirtualKey::kXInputPadLThumbRight,
    VirtualKey::kXInputPadLThumbLeft,
    VirtualKey::kXInputPadLThumbUpLeft,
    VirtualKey::kXInputPadLThumbUpRight,
    VirtualKey::kXInputPadLThumbDownRight,
    VirtualKey::kXInputPadLThumbDownLeft,
    VirtualKey::kXInputPadRThumbUp,
    VirtualKey::kXInputPadRThumbDown,
    VirtualKey::kXInputPadRThumbRight,
    VirtualKey::kXInputPadRThumbLeft,
    VirtualKey::kXInputPadRThumbUpLeft,
    VirtualKey::kXInputPadRThumbUpRight,
    VirtualKey::kXInputPadRThumbDownRight,
    VirtualKey::kXInputPadRThumbDownLeft,
};

u64 AnalogToKeyfield(const rex::hid::PadState& state) {
  u64 field = 0;
  field |= static_cast<u64>(state.left_trigger > kTriggerKeyThreshold) << 16;
  field |= static_cast<u64>(state.right_trigger > kTriggerKeyThreshold) << 17;

  i32 thumb_x = state.thumb_lx;
  i32 thumb_y = state.thumb_ly;
  for (size_t i = 0; i <= 8; i += 8) {
    u64 up = thumb_y > kThumbKeyThreshold;
    u64 down = thumb_y < ~kThumbKeyThreshold;
    u64 right = thumb_x > kThumbKeyThreshold;
    u64 left = thumb_x < ~kThumbKeyThreshold;
    if (up && left) {
      up = left = 0;
      field |= u64{1} << (22 + i);
    }
    if (up && right) {
      up = right = 0;
      field |= u64{1} << (23 + i);
    }
    if (down && right) {
      down = right = 0;
      field |= u64{1} << (24 + i);
    }
    if (down && left) {
      down = left = 0;
      field |= u64{1} << (25 + i);
    }
    field |= up << (18 + i);
    field |= down << (19 + i);
    field |= right << (20 + i);
    field |= left << (21 + i);

    thumb_x = state.thumb_rx;
    thumb_y = state.thumb_ry;
  }
  return field;
}

rex::hid::PadKeystroke MakeKeystroke(VirtualKey key, u16 flags) {
  rex::hid::PadKeystroke keystroke;
  keystroke.virtual_key = static_cast<u16>(key);
  keystroke.flags = flags;
  return keystroke;
}

}  // namespace

Result<rex::hid::PadKeystroke> KeystrokeSynth::Next(const rex::hid::PadState& state, u32 now_ms) {
  const u64 current = static_cast<u64>(state.buttons) | AnalogToKeyfield(state);

  if (repeat_ == Repeat::kWaiting && repeat_time_ + kRepeatDelayMs < now_ms) {
    repeat_ = Repeat::kRepeating;
  }
  if (repeat_ == Repeat::kRepeating && repeat_time_ + kRepeatRateMs < now_ms) {
    repeat_time_ = now_ms;
    return MakeKeystroke(kKeyForBit[repeat_index_], kKeystrokeKeyDown | kKeystrokeRepeat);
  }

  const u64 changed = current ^ held_;
  if (changed == 0) {
    return Err<rex::hid::PadKeystroke>(ErrorCategory::kNotFound, "no keystroke is waiting");
  }

  for (bool clear_pass : {true, false}) {
    for (u32 i = 0; i < kKeyForBit.size(); ++i) {
      const u64 bit = u64{1} << i;
      if (!(changed & bit) || kKeyForBit[i] == VirtualKey::kNone) {
        continue;
      }
      const bool pressed = (current & bit) != 0;
      if (clear_pass && !pressed) {
        held_ &= ~bit;
        repeat_ = Repeat::kIdle;
        return MakeKeystroke(kKeyForBit[i], kKeystrokeKeyUp);
      }
      if (!clear_pass && pressed) {
        held_ |= bit;
        repeat_ = Repeat::kWaiting;
        repeat_index_ = i;
        repeat_time_ = now_ms;
        return MakeKeystroke(kKeyForBit[i], kKeystrokeKeyDown);
      }
    }
  }
  return Err<rex::hid::PadKeystroke>(ErrorCategory::kNotFound, "no keystroke is waiting");
}

}  // namespace rex::hid::detail
