/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Sanjay Govind, 2026 - Adapted for ReXGlue runtime
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include <rex/hid/driver.h>
#include <rex/platform/dynlib.h>

#include <windows.h>
#include <xinput.h>

namespace rex::hid::xinput {

class XInputDriver final : public Driver {
 public:
  XInputDriver() = default;
  ~XInputDriver() override = default;

  [[nodiscard]] Result<void> Setup();

  void EnumerateDevices(std::vector<Device>& out) override;
  [[nodiscard]] Result<X_INPUT_STATE> GetDeviceState(DeviceId id) override;
  [[nodiscard]] Result<X_INPUT_CAPABILITIES> GetDeviceCapabilities(DeviceId id, u32 flags) override;
  [[nodiscard]] Result<void> SetDeviceVibration(DeviceId id,
                                                const X_INPUT_VIBRATION& vibration) override;
  [[nodiscard]] Result<X_INPUT_KEYSTROKE> GetDeviceKeystroke(DeviceId id, u32 flags) override;

 private:
  X_RESULT ReadDeviceState(DeviceId id, X_INPUT_STATE* out_state);
  X_RESULT ReadDeviceCapabilities(DeviceId id, u32 flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT WriteDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration);
  /**
   * Reads the next keystroke from a native slot. XInputGetKeystroke returns ERROR_SUCCESS for
   * a disconnected device, so XInputGetCapabilities checks the connection first and no
   * uninitialized X_INPUT_KEYSTROKE is returned.
   *
   * @param id             Device handle.
   * @param out_keystroke  Receives the keystroke.
   * @return X_ERROR_EMPTY when there are no new keys, X_ERROR_DEVICE_NOT_CONNECTED when there
   *         is no device, X_ERROR_SUCCESS when a key was read.
   */
  X_RESULT ReadDeviceKeystroke(DeviceId id, X_INPUT_KEYSTROKE* out_keystroke);

  platform::DynamicLibrary library_;
  decltype(&XInputGetCapabilities) get_capabilities_ = nullptr;
  decltype(&XInputGetState) get_state_ = nullptr;
  decltype(&XInputGetState) get_state_ex_ = nullptr;
  decltype(&XInputGetKeystroke) get_keystroke_ = nullptr;
  decltype(&XInputSetState) set_state_ = nullptr;
};

}  // namespace rex::hid::xinput
