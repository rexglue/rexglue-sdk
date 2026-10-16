#include "hid/idle_pad_driver.h"

namespace rex::hid {

namespace {

constexpr DeviceId kIdleDevice = static_cast<DeviceId>(0x4E4F5000);

rex::Error NotTheIdlePad() {
  return BuildError(HIDError::kNotConnected, "not the idle pad");
}

}  // namespace

void IdlePadDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = kIdleDevice;
  info.name = "None";
  info.synthetic = true;
  out.push_back(info);
}

Result<X_INPUT_STATE> IdlePadDriver::GetDeviceState(DeviceId id) {
  if (id != kIdleDevice) {
    return std::unexpected(NotTheIdlePad());
  }
  return X_INPUT_STATE{};
}

Result<X_INPUT_CAPABILITIES> IdlePadDriver::GetDeviceCapabilities(DeviceId id, u32) {
  if (id != kIdleDevice) {
    return std::unexpected(NotTheIdlePad());
  }
  X_INPUT_CAPABILITIES caps{};
  caps.type = kXInputDevTypeGamepad;
  caps.sub_type = kXInputDevSubtypeGamepad;
  caps.gamepad.buttons = 0xFFFF;
  caps.gamepad.left_trigger = 0xFF;
  caps.gamepad.right_trigger = 0xFF;
  caps.gamepad.thumb_lx = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_ly = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_rx = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_ry = static_cast<i16>(0x7FFF);
  caps.vibration.left_motor_speed = 0xFFFF;
  caps.vibration.right_motor_speed = 0xFFFF;
  return caps;
}

Result<void> IdlePadDriver::SetDeviceVibration(DeviceId id, const X_INPUT_VIBRATION&) {
  if (id != kIdleDevice) {
    return std::unexpected(NotTheIdlePad());
  }
  return Ok();
}

Result<X_INPUT_KEYSTROKE> IdlePadDriver::GetDeviceKeystroke(DeviceId id, u32) {
  if (id != kIdleDevice) {
    return std::unexpected(NotTheIdlePad());
  }
  return std::unexpected(BuildError(HIDError::kEmpty, "the idle pad has no keystrokes"));
}

}  // namespace rex::hid
