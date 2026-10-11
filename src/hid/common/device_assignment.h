/**
 * @file        hid/common/device_assignment.h
 * @brief       Policy assigning physical input devices to guest user indices.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <rex/hid/device.h>

namespace rex::hid {

class DeviceAssignment {
 public:
  virtual ~DeviceAssignment() = default;

  /**
   * Replaces the device list. Devices arrive sorted by ordinal.
   *
   * @param devices  Connected devices.
   */
  virtual void OnDevicesChanged(const std::vector<DeviceInfo>& devices) = 0;

  /**
   * Copies the devices feeding a title user. An empty result means the title sees
   * X_ERROR_DEVICE_NOT_CONNECTED.
   *
   * @param user_index  Title user index.
   * @param out         Receives the device ids.
   */
  virtual void DevicesForUser(u32 user_index, std::vector<DeviceId>& out) const = 0;
};

/**
 * Device ordinal N feeds title user N, keyed by ordinal and not list position so
 * unplugging pad 1 does not promote pad 2. Synthetic devices feed user 0.
 */
class SlotAssignment final : public DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override;
  void DevicesForUser(u32 user_index, std::vector<DeviceId>& out) const override;

 private:
  std::vector<std::vector<DeviceId>> users_ = std::vector<std::vector<DeviceId>>(kMaxTitleUsers);
};

/**
 * Every device feeds guest user 0. Users 1 and up report not connected. Serves
 * single player titles that only poll user 0.
 */
class SharedAssignment final : public DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override;
  void DevicesForUser(u32 user_index, std::vector<DeviceId>& out) const override;

 private:
  std::vector<DeviceId> user0_;
};

}  // namespace rex::hid
