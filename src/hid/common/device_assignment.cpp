#include "hid/common/device_assignment.h"

namespace rex::hid {

void SlotAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  for (auto& user : users_) {
    user.clear();
  }

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
