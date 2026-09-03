#pragma once
/**
 * @file        rex/input/state_merge.h
 * @brief       Combining several devices into one guest controller state.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>

#include <rex/input/device.h>
#include <rex/input/input.h>

namespace rex::input {

constexpr int32_t kThumbDeadzone = X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
constexpr uint8_t kTriggerThreshold = X_INPUT_GAMEPAD_TRIGGER_THRESHOLD;

/// Folds src into dst: buttons OR, triggers max, stick axes larger magnitude,
/// packet number newest.
void MergeInto(X_INPUT_STATE& dst, const X_INPUT_STATE& src);

bool IsNeutral(const X_INPUT_GAMEPAD& gamepad);

/// Tracks which device most recently produced real input, per guest user, so
/// button glyphs follow the pad in the player's hands.
class ActiveDeviceTracker {
 public:
  /// Neutral gamepads never take over, so stick drift on an idle pad cannot
  /// steal the slot.
  void Observe(uint32_t user_index, DeviceId id, const X_INPUT_GAMEPAD& gamepad);

  DeviceId Active(uint32_t user_index) const;
  void Forget(DeviceId id);

 private:
  // Zero-initialized, which is DeviceId::kInvalid.
  std::array<DeviceId, kMaxGuestUsers> active_ = {};
};

}  // namespace rex::input
