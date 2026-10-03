/**
 * @file        hid/common/device_assignment.cpp
 * @brief       Policies assigning input devices to guest user indices.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "hid/common/device_assignment.h"

namespace rex::hid {

void SlotAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  for (auto& user : users_) {
    user.clear();
  }

  // Key off the ordinal rather than list position, so unplugging pad one does
  // not promote pad two to player one.
  for (const auto& device : devices) {
    if (device.synthetic) {
      users_[0].push_back(device.id);
    } else if (device.ordinal < kMaxGuestUsers) {
      users_[device.ordinal].push_back(device.id);
    }
  }
}

void SlotAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index >= kMaxGuestUsers) {
    return;
  }
  out = users_[user_index];
}

void SharedAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  user0_.clear();
  user0_.reserve(devices.size());
  for (const auto& device : devices) {
    user0_.push_back(device.id);
  }
}

void SharedAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index != 0) {
    return;
  }
  out = user0_;
}

}  // namespace rex::hid
