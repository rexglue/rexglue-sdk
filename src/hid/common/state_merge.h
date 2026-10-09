/**
 * @file        hid/common/state_merge.h
 * @brief       Combining several devices into one guest controller state.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>

#include <rex/hid/device.h>
#include <rex/hid/xinput.h>

namespace rex::hid {

constexpr i32 kThumbDeadzone = X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
constexpr u8 kTriggerThreshold = X_INPUT_GAMEPAD_TRIGGER_THRESHOLD;

/**
 * Folds src into dst. Buttons OR, triggers max, stick axes larger magnitude,
 * packet number newest.
 *
 * @param dst  Receives the merged state.
 * @param src  State folded in.
 */
void MergeInto(X_INPUT_STATE& dst, const X_INPUT_STATE& src);

bool IsNeutral(const X_INPUT_GAMEPAD& gamepad);

/**
 * Tracks which device most recently produced real input, per guest user, so
 * button glyphs follow the pad in the player's hands.
 */
class ActiveDeviceTracker {
 public:
  /**
   * Takes input from a device. Neutral gamepads never take over, so stick drift on
   * an idle pad cannot steal the slot.
   *
   * @param user_index  Title user index.
   * @param id          Device that produced the input.
   * @param gamepad     Gamepad report.
   */
  void Observe(u32 user_index, DeviceId id, const X_INPUT_GAMEPAD& gamepad);

  DeviceId Active(u32 user_index) const;
  void Forget(DeviceId id);

 private:
  std::array<DeviceId, kMaxGuestUsers> active_ = {};
};

}  // namespace rex::hid
