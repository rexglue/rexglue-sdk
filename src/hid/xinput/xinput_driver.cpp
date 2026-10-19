/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Sanjay Govind, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging/log.h>
#include "hid/cvar_table.h"
#include "hid/x_result.h"
#include "hid/xinput/xinput_driver.h"
#include <windows.h>
#include <xinput.h>

namespace {

constexpr u32 kXInputSlotCount = 4;

/**
 * Builds the device handle for a native slot. XInput has 4 fixed slots, so the slot index
 * rides in the low byte of the handle.
 *
 * @param slot  Native slot index.
 * @return Device handle.
 */
constexpr rex::hid::DeviceId DeviceForSlot(u32 slot) {
  return static_cast<rex::hid::DeviceId>(0x58490000ull | slot);
}

bool SlotForDevice(rex::hid::DeviceId id, u32* out_slot) {
  const u64 raw = static_cast<u64>(id);
  if ((raw & ~0xFFull) != 0x58490000ull || (raw & 0xFF) >= kXInputSlotCount) {
    return false;
  }
  *out_slot = static_cast<u32>(raw & 0xFF);
  return true;
}

/** Milliseconds to skip an empty slot after a miss, since querying one is slow. */
constexpr u64 kSkipInvalidControllerTime = 1100;
u64 g_last_invalid_time[kXInputSlotCount];

DWORD SkipError(u32 user_index) {
  u64 time = g_last_invalid_time[user_index];
  if (time) {
    u64 deltatime = rex::Clock::QueryHostUptimeMillis() - time;

    if (deltatime < kSkipInvalidControllerTime) {
      return ERROR_DEVICE_NOT_CONNECTED;
    }
    g_last_invalid_time[user_index] = 0;
  }
  return 0;
}

void SetSkip(u32 user_index) {
  g_last_invalid_time[user_index] = rex::Clock::QueryHostUptimeMillis();
}

}  // namespace

namespace rex::hid::xinput {

Result<void> XInputDriver::Setup() {
  if (!library_.Load("xinput1_4.dll")) {
    return Err(ErrorCategory::kPlatform,
               std::format("xinput1_4.dll cannot be loaded, {}", library_.GetLastError()));
  }
  get_capabilities_ = library_.GetSymbol<decltype(get_capabilities_)>("XInputGetCapabilities");
  get_state_ = library_.GetSymbol<decltype(get_state_)>("XInputGetState");
  get_state_ex_ = library_.GetSymbol<decltype(get_state_ex_)>(
      // NOLINTNEXTLINE(performance-no-int-to-ptr): GetProcAddress takes an ordinal as a pointer
      reinterpret_cast<const char*>(std::uintptr_t{100}));
  get_keystroke_ = library_.GetSymbol<decltype(get_keystroke_)>("XInputGetKeystroke");
  set_state_ = library_.GetSymbol<decltype(set_state_)>("XInputSetState");
  if (!get_capabilities_ || !get_state_ || !get_keystroke_ || !set_state_) {
    library_.Close();
    return Err(ErrorCategory::kPlatform, "xinput1_4.dll lacks a required export");
  }
  return Ok();
}

void XInputDriver::EnumerateDevices(std::vector<Device>& out) {
  auto get_capabilities = get_capabilities_;
  if (!get_capabilities) {
    return;
  }
  for (u32 slot = 0; slot < kXInputSlotCount; slot++) {
    if (SkipError(slot)) {
      continue;
    }
    XINPUT_CAPABILITIES native_caps;
    DWORD result = get_capabilities(slot, 0, &native_caps);
    if (result) {
      if (result == ERROR_DEVICE_NOT_CONNECTED) {
        SetSkip(slot);
      }
      continue;
    }
    Device info;
    info.id = DeviceForSlot(slot);
    info.name = "XInput Controller";
    info.subtype = static_cast<u8>(native_caps.SubType);
    info.synthetic = false;
    out.push_back(info);
  }
}

Result<X_INPUT_STATE> XInputDriver::GetDeviceState(DeviceId id) {
  X_INPUT_STATE state{};
  return ToResult(ReadDeviceState(id, &state), state);
}

Result<X_INPUT_CAPABILITIES> XInputDriver::GetDeviceCapabilities(DeviceId id, u32 flags) {
  X_INPUT_CAPABILITIES caps{};
  return ToResult(ReadDeviceCapabilities(id, flags, &caps), caps);
}

Result<void> XInputDriver::SetDeviceVibration(DeviceId id, const X_INPUT_VIBRATION& vibration) {
  X_INPUT_VIBRATION copy = vibration;
  return ToResult(WriteDeviceVibration(id, &copy));
}

Result<X_INPUT_KEYSTROKE> XInputDriver::GetDeviceKeystroke(DeviceId id,
                                                           [[maybe_unused]] u32 flags) {
  X_INPUT_KEYSTROKE keystroke{};
  return ToResult(ReadDeviceKeystroke(id, &keystroke), keystroke);
}

X_RESULT XInputDriver::ReadDeviceCapabilities(DeviceId id, u32 flags,
                                              X_INPUT_CAPABILITIES* out_caps) {
  u32 user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  DWORD skipper = SkipError(user_index);
  if (skipper) {
    return skipper;
  }
  XINPUT_CAPABILITIES native_caps;
  auto xigc = get_capabilities_;
  DWORD result = xigc(user_index, flags & ~X_INPUT_DEVTYPE::kXInputDevTypeKeyboard, &native_caps);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      SetSkip(user_index);
    }
    return result;
  }

  out_caps->type = native_caps.Type;
  out_caps->sub_type = native_caps.SubType;
  out_caps->flags = native_caps.Flags;
  out_caps->gamepad.buttons = native_caps.Gamepad.wButtons;
  out_caps->gamepad.left_trigger = native_caps.Gamepad.bLeftTrigger;
  out_caps->gamepad.right_trigger = native_caps.Gamepad.bRightTrigger;
  out_caps->gamepad.thumb_lx = native_caps.Gamepad.sThumbLX;
  out_caps->gamepad.thumb_ly = native_caps.Gamepad.sThumbLY;
  out_caps->gamepad.thumb_rx = native_caps.Gamepad.sThumbRX;
  out_caps->gamepad.thumb_ry = native_caps.Gamepad.sThumbRY;
  out_caps->vibration.left_motor_speed = native_caps.Vibration.wLeftMotorSpeed;
  out_caps->vibration.right_motor_speed = native_caps.Vibration.wRightMotorSpeed;

  return result;
}

X_RESULT XInputDriver::ReadDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  u32 user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  DWORD skipper = SkipError(user_index);
  if (skipper) {
    return skipper;
  }

  // Extra padding for the larger XInputGetStateEx output.
  struct {
    XINPUT_STATE state;
    unsigned int dwPaddingReserved;
  } native_state;

  auto get_state_fn = REXCVAR_GET(cvars::guide_button) ? get_state_ex_ : get_state_;

  DWORD result = get_state_fn(user_index, &native_state.state);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      SetSkip(user_index);
    }
    return result;
  }

  out_state->packet_number = native_state.state.dwPacketNumber;
  out_state->gamepad.buttons = native_state.state.Gamepad.wButtons;
  out_state->gamepad.left_trigger = native_state.state.Gamepad.bLeftTrigger;
  out_state->gamepad.right_trigger = native_state.state.Gamepad.bRightTrigger;
  out_state->gamepad.thumb_lx = native_state.state.Gamepad.sThumbLX;
  out_state->gamepad.thumb_ly = native_state.state.Gamepad.sThumbLY;
  out_state->gamepad.thumb_rx = native_state.state.Gamepad.sThumbRX;
  out_state->gamepad.thumb_ry = native_state.state.Gamepad.sThumbRY;

  return result;
}

X_RESULT XInputDriver::WriteDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  u32 user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  DWORD skipper = SkipError(user_index);
  if (skipper) {
    return skipper;
  }
  XINPUT_VIBRATION native_vibration;
  native_vibration.wLeftMotorSpeed = vibration->left_motor_speed;
  native_vibration.wRightMotorSpeed = vibration->right_motor_speed;
  auto set_state_fn = set_state_;
  DWORD result = set_state_fn(user_index, &native_vibration);
  if (result == ERROR_DEVICE_NOT_CONNECTED) {
    SetSkip(user_index);
  }
  return result;
}

X_RESULT XInputDriver::ReadDeviceKeystroke(DeviceId id, X_INPUT_KEYSTROKE* out_keystroke) {
  // flags is reserved on desktop.
  u32 user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  XINPUT_CAPABILITIES caps;
  auto xigc = get_capabilities_;
  DWORD result = xigc(user_index, 0, &caps);
  if (result) {
    return result;
  }

  XINPUT_KEYSTROKE native_keystroke;
  auto get_keystroke_fn = get_keystroke_;
  result = get_keystroke_fn(user_index, 0, &native_keystroke);
  if (result) {
    return result;
  }

  out_keystroke->virtual_key = native_keystroke.VirtualKey;
  out_keystroke->unicode = native_keystroke.Unicode;
  out_keystroke->flags = native_keystroke.Flags;
  out_keystroke->user_index = native_keystroke.UserIndex;
  out_keystroke->hid_code = native_keystroke.HidCode;
  return result;
}
}  // namespace rex::hid::xinput
