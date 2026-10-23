#include "hid/idle_pad_driver.h"

#include "core/hid/idle_pad_caps.h"

namespace rex::hid {

namespace {

constexpr DeviceId kIdleDevice = static_cast<DeviceId>(0x4E4F5000);

rex::Error NotTheIdlePad() {
  return BuildError(HIDError::kNotConnected, "not the idle pad");
}

}  // namespace

void IdlePadDriver::EnumerateDevices(std::vector<Device>& out) {
  Device info;
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
  return detail::MakeIdlePadCapabilities();
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
