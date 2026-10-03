/**
 * @file        hid/common/state_merge.cpp
 * @brief       Combining several devices into one guest controller state.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "hid/common/state_merge.h"

#include <algorithm>
#include <cstdlib>

namespace rex::hid {

namespace {

int16_t LargerMagnitude(int16_t a, int16_t b) {
  return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b;
}

bool PastDeadzone(int16_t axis) {
  return std::abs(static_cast<int>(axis)) >= kThumbDeadzone;
}

}  // namespace

void MergeInto(X_INPUT_STATE& dst, const X_INPUT_STATE& src) {
  dst.gamepad.buttons =
      static_cast<uint16_t>(dst.gamepad.buttons) | static_cast<uint16_t>(src.gamepad.buttons);
  dst.gamepad.left_trigger = std::max(dst.gamepad.left_trigger, src.gamepad.left_trigger);
  dst.gamepad.right_trigger = std::max(dst.gamepad.right_trigger, src.gamepad.right_trigger);
  dst.gamepad.thumb_lx = LargerMagnitude(dst.gamepad.thumb_lx, src.gamepad.thumb_lx);
  dst.gamepad.thumb_ly = LargerMagnitude(dst.gamepad.thumb_ly, src.gamepad.thumb_ly);
  dst.gamepad.thumb_rx = LargerMagnitude(dst.gamepad.thumb_rx, src.gamepad.thumb_rx);
  dst.gamepad.thumb_ry = LargerMagnitude(dst.gamepad.thumb_ry, src.gamepad.thumb_ry);

  if (static_cast<uint32_t>(src.packet_number) > static_cast<uint32_t>(dst.packet_number)) {
    dst.packet_number = src.packet_number;
  }
}

bool IsNeutral(const X_INPUT_GAMEPAD& gamepad) {
  if (static_cast<uint16_t>(gamepad.buttons) != 0) {
    return false;
  }
  if (gamepad.left_trigger >= kTriggerThreshold || gamepad.right_trigger >= kTriggerThreshold) {
    return false;
  }
  return !PastDeadzone(gamepad.thumb_lx) && !PastDeadzone(gamepad.thumb_ly) &&
         !PastDeadzone(gamepad.thumb_rx) && !PastDeadzone(gamepad.thumb_ry);
}

void ActiveDeviceTracker::Observe(uint32_t user_index, DeviceId id,
                                  const X_INPUT_GAMEPAD& gamepad) {
  if (user_index >= kMaxGuestUsers || IsNeutral(gamepad)) {
    return;
  }
  active_[user_index] = id;
}

DeviceId ActiveDeviceTracker::Active(uint32_t user_index) const {
  if (user_index >= kMaxGuestUsers) {
    return DeviceId::kInvalid;
  }
  return active_[user_index];
}

void ActiveDeviceTracker::Forget(DeviceId id) {
  for (auto& active : active_) {
    if (active == id) {
      active = DeviceId::kInvalid;
    }
  }
}

}  // namespace rex::hid
