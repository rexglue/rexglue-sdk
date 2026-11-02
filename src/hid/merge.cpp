/**
 * @file        hid/merge.cpp
 * @brief       Merging the pads of every driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "hid/input_internal.h"

namespace rex::hid::detail {

namespace {

constexpr f64 kThumbstickMax = 32767.0;

i16 LargerMagnitude(i16 a, i16 b) {
  return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b;
}

bool PastDeadzone(i16 axis) {
  return std::abs(static_cast<int>(axis)) >= kThumbDeadzone;
}

void ApplyRadial(f64 percentage, i16& axis_x, i16& axis_y) {
  if (percentage <= 0.0 || percentage >= 1.0) {
    return;
  }
  const f64 deadzone = kThumbstickMax * percentage;
  const f64 theta = std::atan2(static_cast<f64>(axis_y), static_cast<f64>(axis_x));
  const f64 cutoff_x = std::cos(theta) * deadzone;
  const f64 cutoff_y = std::sin(theta) * deadzone;
  if (axis_x > -cutoff_x && axis_x < cutoff_x) {
    axis_x = 0;
  }
  if (axis_y > -cutoff_y && axis_y < cutoff_y) {
    axis_y = 0;
  }
}

}  // namespace

void MergeInto(rex::hid::PadState& dst, const rex::hid::PadState& src) {
  dst.buttons = static_cast<u16>(dst.buttons | src.buttons);
  dst.left_trigger = std::max(dst.left_trigger, src.left_trigger);
  dst.right_trigger = std::max(dst.right_trigger, src.right_trigger);
  dst.thumb_lx = LargerMagnitude(dst.thumb_lx, src.thumb_lx);
  dst.thumb_ly = LargerMagnitude(dst.thumb_ly, src.thumb_ly);
  dst.thumb_rx = LargerMagnitude(dst.thumb_rx, src.thumb_rx);
  dst.thumb_ry = LargerMagnitude(dst.thumb_ry, src.thumb_ry);
}

bool IsNeutral(const rex::hid::PadState& state) {
  if (state.buttons != 0) {
    return false;
  }
  if (state.left_trigger >= kTriggerThreshold || state.right_trigger >= kTriggerThreshold) {
    return false;
  }
  return !PastDeadzone(state.thumb_lx) && !PastDeadzone(state.thumb_ly) &&
         !PastDeadzone(state.thumb_rx) && !PastDeadzone(state.thumb_ry);
}

void ApplyDeadzones(rex::hid::PadState& state, f64 left, f64 right) {
  ApplyRadial(left, state.thumb_lx, state.thumb_ly);
  ApplyRadial(right, state.thumb_rx, state.thumb_ry);
}

bool IsSamePad(const rex::hid::PadIdentity& a, const rex::hid::PadIdentity& b) {
  if (a.xinput_slot >= 0 && b.xinput_slot >= 0) {
    return a.xinput_slot == b.xinput_slot;
  }
  return !a.serial.empty() && !b.serial.empty() && a.vendor == b.vendor && a.product == b.product &&
         a.serial == b.serial;
}

void ActiveDeviceTracker::Observe(u32 user, const DeviceKey& key, const rex::hid::PadState& state) {
  if (user >= kMaxUsers || IsNeutral(state)) {
    return;
  }
  active_[user] = key;
  has_[user] = true;
}

bool ActiveDeviceTracker::GetActive(u32 user, DeviceKey& out) const {
  if (user >= kMaxUsers || !has_[user]) {
    return false;
  }
  out = active_[user];
  return true;
}

void ActiveDeviceTracker::Forget(const DeviceKey& key) {
  for (u32 user = 0; user < kMaxUsers; ++user) {
    if (has_[user] && active_[user] == key) {
      has_[user] = false;
    }
  }
}

}  // namespace rex::hid::detail
