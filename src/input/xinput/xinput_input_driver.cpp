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
#include <filesystem>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>
#include <xinput.h>  // NOLINT(build/include_order)

namespace {

constexpr uint32_t kXinputSlotCount = 4;

// XInput has four fixed native slots, so the slot index rides inside the handle
// and the per-slot bookkeeping below stays keyed by it.
constexpr rex::input::DeviceId DeviceForSlot(uint32_t slot) {
  return static_cast<rex::input::DeviceId>(0x58490000ull | slot);
}

bool SlotForDevice(rex::input::DeviceId id, uint32_t* out_slot) {
  const uint64_t raw = static_cast<uint64_t>(id);
  if ((raw & ~0xFFull) != 0x58490000ull || (raw & 0xFF) >= kXinputSlotCount) {
    return false;
  }
  *out_slot = static_cast<uint32_t>(raw & 0xFF);
  return true;
}

// Querying an empty slot costs milliseconds, so back off after a miss.
constexpr uint64_t SKIP_INVALID_CONTROLLER_TIME = 1100;
uint64_t last_invalid_time[kXinputSlotCount];

DWORD should_skip(uint32_t user_index) {
  uint64_t time = last_invalid_time[user_index];
  if (time) {
    uint64_t deltatime = rex::chrono::Clock::QueryHostUptimeMillis() - time;

    if (deltatime < SKIP_INVALID_CONTROLLER_TIME) {
      return ERROR_DEVICE_NOT_CONNECTED;
    }
    last_invalid_time[user_index] = 0;
  }
  return 0;
}

void set_skip(uint32_t user_index) {
  last_invalid_time[user_index] = rex::chrono::Clock::QueryHostUptimeMillis();
}

}  // namespace

namespace rex::input::xinput {

XinputInputDriver::XinputInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      module_(nullptr),
      XInputGetCapabilities_(nullptr),
      XInputGetState_(nullptr),
      XInputGetStateEx_(nullptr),
      XInputGetKeystroke_(nullptr),
      XInputSetState_(nullptr),
      XInputEnable_(nullptr) {}

XinputInputDriver::~XinputInputDriver() {
  if (module_) {
    FreeLibrary((HMODULE)module_);
    module_ = nullptr;
    XInputGetCapabilities_ = nullptr;
    XInputGetState_ = nullptr;
    XInputGetStateEx_ = nullptr;
    XInputGetKeystroke_ = nullptr;
    XInputSetState_ = nullptr;
    XInputEnable_ = nullptr;
  }
}

X_STATUS XinputInputDriver::Setup() {
  HMODULE module = LoadLibraryW(L"xinput1_4.dll");
  if (!module) {
    return X_STATUS_DLL_NOT_FOUND;
  }

  // Support guide button with XInput using XInputGetStateEx
  // https://source.winehq.org/git/wine.git/?a=commit;h=de3591ca9803add117fbacb8abe9b335e2e44977
  auto const XInputGetStateEx = (LPCSTR)100;

  // Required.
  auto xigc = GetProcAddress(module, "XInputGetCapabilities");
  auto xigs = GetProcAddress(module, "XInputGetState");
  auto xigsEx = GetProcAddress(module, XInputGetStateEx);
  auto xigk = GetProcAddress(module, "XInputGetKeystroke");
  auto xiss = GetProcAddress(module, "XInputSetState");

  // Not required.
  auto xie = GetProcAddress(module, "XInputEnable");

  // Only fail when we don't have the bare essentials;
  if (!xigc || !xigs || !xigk || !xiss) {
    FreeLibrary(module);
    return X_STATUS_PROCEDURE_NOT_FOUND;
  }

  module_ = module;
  XInputGetCapabilities_ = xigc;
  XInputGetState_ = xigs;
  XInputGetStateEx_ = xigsEx;
  XInputGetKeystroke_ = xigk;
  XInputSetState_ = xiss;
  XInputEnable_ = xie;

  return X_STATUS_SUCCESS;
}

void XinputInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilities_;
  if (!xigc) {
    return;
  }
  for (uint32_t slot = 0; slot < kXinputSlotCount; slot++) {
    if (should_skip(slot)) {
      continue;
    }
    XINPUT_CAPABILITIES native_caps;
    DWORD result = xigc(slot, 0, &native_caps);
    if (result) {
      if (result == ERROR_DEVICE_NOT_CONNECTED) {
        set_skip(slot);
      }
      continue;
    }
    DeviceInfo info;
    info.id = DeviceForSlot(slot);
    info.name = "XInput Controller";
    info.subtype = static_cast<uint8_t>(native_caps.SubType);
    info.synthetic = false;
    out.push_back(info);
  }
}

X_RESULT XinputInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  uint32_t user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  DWORD skipper = should_skip(user_index);
  if (skipper) {
    return skipper;
  }
  XINPUT_CAPABILITIES native_caps;
  auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilities_;
  DWORD result = xigc(user_index, flags & ~X_INPUT_DEVTYPE::XINPUT_DEVTYPE_KEYBOARD, &native_caps);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      set_skip(user_index);
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

X_RESULT XinputInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  uint32_t user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  DWORD skipper = should_skip(user_index);
  if (skipper) {
    return skipper;
  }

  // Added padding in case we are using XInputGetStateEx
  struct {
    XINPUT_STATE state;
    unsigned int dwPaddingReserved;
  } native_state;

  // If the guide button is enabled use XInputGetStateEx, otherwise use the
  // default XInputGetState.
  auto xigs = REXCVAR_GET(guide_button) ? (decltype(&XInputGetState))XInputGetStateEx_
                                        : (decltype(&XInputGetState))XInputGetState_;

  DWORD result = xigs(user_index, &native_state.state);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      set_skip(user_index);
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

X_RESULT XinputInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  uint32_t user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  DWORD skipper = should_skip(user_index);
  if (skipper) {
    return skipper;
  }
  XINPUT_VIBRATION native_vibration;
  native_vibration.wLeftMotorSpeed = vibration->left_motor_speed;
  native_vibration.wRightMotorSpeed = vibration->right_motor_speed;
  auto xiss = (decltype(&XInputSetState))XInputSetState_;
  DWORD result = xiss(user_index, &native_vibration);
  if (result == ERROR_DEVICE_NOT_CONNECTED) {
    set_skip(user_index);
  }
  return result;
}

X_RESULT XinputInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                               X_INPUT_KEYSTROKE* out_keystroke) {
  // We may want to filter flags before sending to native.
  // flags is reserved on desktop.
  uint32_t user_index = 0;
  if (!SlotForDevice(id, &user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // XInputGetKeystroke on Windows has a bug where it will return
  // ERROR_SUCCESS (0) even if the device is not connected:
  // https://stackoverflow.com/questions/23669238/xinputgetkeystroke-returning-error-success-while-controller-is-unplugged
  //
  // So we first check if the device is connected via XInputGetCapabilities, so
  // we are not passing back an uninitialized X_INPUT_KEYSTROKE structure.
  XINPUT_CAPABILITIES caps;
  auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilities_;
  DWORD result = xigc(user_index, 0, &caps);
  if (result) {
    return result;
  }

  XINPUT_KEYSTROKE native_keystroke;
  auto xigk = (decltype(&XInputGetKeystroke))XInputGetKeystroke_;
  result = xigk(user_index, 0, &native_keystroke);
  if (result) {
    return result;
  }

  out_keystroke->virtual_key = native_keystroke.VirtualKey;
  out_keystroke->unicode = native_keystroke.Unicode;
  out_keystroke->flags = native_keystroke.Flags;
  out_keystroke->user_index = native_keystroke.UserIndex;
  out_keystroke->hid_code = native_keystroke.HidCode;
  // X_ERROR_EMPTY if no new keys
  // X_ERROR_DEVICE_NOT_CONNECTED if no device
  // X_ERROR_SUCCESS if key
  return result;
}
}  // namespace rex::input::xinput
