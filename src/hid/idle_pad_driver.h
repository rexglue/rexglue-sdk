/**
 * @file        hid/idle_pad_driver.h
 * @brief       1 connected pad that never moves, so a title does not stop on a missing pad
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/hid/driver.h>

namespace rex::hid {

/** 1 synthetic idle pad. It answers on user 0 behind every physical pad. */
class IdlePadDriver final : public Driver {
 public:
  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  [[nodiscard]] Result<X_INPUT_STATE> GetDeviceState(DeviceId id) override;
  [[nodiscard]] Result<X_INPUT_CAPABILITIES> GetDeviceCapabilities(DeviceId id, u32 flags) override;
  [[nodiscard]] Result<void> SetDeviceVibration(DeviceId id,
                                                const X_INPUT_VIBRATION& vibration) override;
  [[nodiscard]] Result<X_INPUT_KEYSTROKE> GetDeviceKeystroke(DeviceId id, u32 flags) override;
};

}  // namespace rex::hid
