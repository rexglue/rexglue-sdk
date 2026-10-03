#pragma once
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

#include <cstdint>
#include <vector>

#include <rex/hid/device.h>

namespace rex::hid {

class DeviceAssignment {
 public:
  virtual ~DeviceAssignment() = default;

  /// Devices arrive sorted by ordinal.
  virtual void OnDevicesChanged(const std::vector<DeviceInfo>& devices) = 0;

  /// An empty result means the guest sees X_ERROR_DEVICE_NOT_CONNECTED.
  virtual void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const = 0;
};

/// Device ordinal N feeds guest user N. Synthetic devices feed user 0.
class SlotAssignment final : public DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override;

 private:
  std::vector<std::vector<DeviceId>> users_ = std::vector<std::vector<DeviceId>>(kMaxGuestUsers);
};

/// Every device feeds guest user 0. Users 1 and up report not connected. For
/// single-player titles that only ever poll user 0.
class SharedAssignment final : public DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override;

 private:
  std::vector<DeviceId> user0_;
};

}  // namespace rex::hid
