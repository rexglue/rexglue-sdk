/**
 * @file        hid/assignment.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>

#include "hid/input_internal.h"

namespace rex::hid::detail {

void AssignOrdinals(const std::vector<Device>& previous, std::vector<Device>& seen) {
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); ++i) {
    const auto existing = std::ranges::find_if(
        previous, [&](const Device& device) { return device.key == seen[i].key; });
    if (existing != previous.end()) {
      seen[i].ordinal = existing->ordinal;
    } else {
      fresh[i] = true;
    }
  }

  auto taken = [&](u32 candidate) {
    for (size_t j = 0; j < seen.size(); ++j) {
      if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
        return true;
      }
    }
    return false;
  };

  // A pad that has an XInput slot keeps it as its user, whatever order a driver lists it in.
  for (size_t i = 0; i < seen.size(); ++i) {
    const i32 slot = seen[i].pad.identity.xinput_slot;
    if (!fresh[i] || seen[i].synthetic || slot < 0 || slot >= static_cast<i32>(kMaxUsers) ||
        taken(static_cast<u32>(slot))) {
      continue;
    }
    seen[i].ordinal = static_cast<u32>(slot);
    fresh[i] = false;
  }

  for (size_t i = 0; i < seen.size(); ++i) {
    if (!fresh[i]) {
      continue;
    }
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
    } else {
      u32 ordinal = 0;
      while (taken(ordinal)) {
        ++ordinal;
      }
      seen[i].ordinal = ordinal;
    }
    fresh[i] = false;
  }

  std::ranges::stable_sort(seen,
                           [](const Device& a, const Device& b) { return a.ordinal < b.ordinal; });
}

std::array<std::vector<DeviceKey>, kMaxUsers> AssignUsers(const std::vector<Device>& devices,
                                                          bool shared) {
  std::array<std::vector<DeviceKey>, kMaxUsers> users;
  for (const Device& device : devices) {
    if (shared || device.synthetic) {
      users[0].push_back(device.key);
    } else if (device.ordinal < kMaxUsers) {
      users[device.ordinal].push_back(device.key);
    }
  }
  return users;
}

}  // namespace rex::hid::detail
