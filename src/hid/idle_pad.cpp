/**
 * @file        hid/idle_pad.cpp
 * @brief       The pad user 0 reads when nothing else is connected
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "hid/input_internal.h"

namespace rex::hid::detail {

namespace {

constexpr rex::hid::PadDeviceID kIdleDevice = rex::hid::PadDeviceID{0x4E4F5000};

Error NotTheIdlePad() {
  return Error(ErrorCategory::kNotFound, "not the idle pad");
}

}  // namespace

rex::hid::PadCapabilities MakeFullPadCapabilities() {
  rex::hid::PadCapabilities caps;
  caps.subtype = kSubtypeGamepad;
  caps.flags = 0;
  caps.supported.buttons = 0xFFFF;
  caps.supported.left_trigger = 0xFF;
  caps.supported.right_trigger = 0xFF;
  caps.supported.thumb_lx = 0x7FFF;
  caps.supported.thumb_ly = 0x7FFF;
  caps.supported.thumb_rx = 0x7FFF;
  caps.supported.thumb_ry = 0x7FFF;
  caps.motors.left_motor = 0xFFFF;
  caps.motors.right_motor = 0xFFFF;
  return caps;
}

void IdlePad::EnumerateDevices(std::vector<rex::hid::PadDevice>& out) {
  rex::hid::PadDevice device;
  device.id = kIdleDevice;
  device.name = "None";
  out.push_back(std::move(device));
}

Result<rex::hid::PadState> IdlePad::GetDeviceState(rex::hid::PadDeviceID id) {
  if (id != kIdleDevice) {
    return std::unexpected(NotTheIdlePad());
  }
  return rex::hid::PadState{};
}

Result<rex::hid::PadCapabilities> IdlePad::GetDeviceCapabilities(rex::hid::PadDeviceID id) {
  if (id != kIdleDevice) {
    return std::unexpected(NotTheIdlePad());
  }
  return MakeFullPadCapabilities();
}

Result<void> IdlePad::SetDeviceVibration(rex::hid::PadDeviceID id, const rex::hid::PadVibration&) {
  if (id != kIdleDevice) {
    return std::unexpected(NotTheIdlePad());
  }
  return {};
}

}  // namespace rex::hid::detail
