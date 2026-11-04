/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Sanjay Govind, 2026 - Adapted for ReXGlue runtime
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "hid/xinput/xinput_input_driver.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>

namespace rex::hid::xinput {

namespace {

constexpr u32 kSlotCount = 4;
constexpr u64 kDeviceBase = 0x58490000ull;
constexpr auto kPollInterval = std::chrono::milliseconds(500);
constexpr u16 kGuideButton = 0x0400;

constexpr PadDeviceID DeviceForSlot(u32 slot) {
  return static_cast<PadDeviceID>(kDeviceBase | slot);
}

bool SlotForDevice(PadDeviceID id, u32* out_slot) {
  const u64 raw = static_cast<u64>(id);
  if ((raw & ~0xFFull) != kDeviceBase || (raw & 0xFF) >= kSlotCount) {
    return false;
  }
  *out_slot = static_cast<u32>(raw & 0xFF);
  return true;
}

Error NotConnected() {
  return Error(ErrorCategory::kNotFound, "the pad disconnected");
}

}  // namespace

std::string_view XInputInputDriver::GetName() const {
  return "xinput";
}

Result<void> XInputInputDriver::Initialize(IDriverContext&) {
  if (!library_.Load("xinput1_4.dll")) {
    return Err(ErrorCategory::kPlatform,
               std::format("xinput1_4.dll cannot be loaded, {}", library_.GetLastError()));
  }
  get_capabilities_ = library_.GetSymbol<decltype(get_capabilities_)>("XInputGetCapabilities");
  get_state_ = library_.GetSymbol<decltype(get_state_)>("XInputGetState");
  get_state_ex_ = library_.GetSymbol<decltype(get_state_ex_)>(
      // NOLINTNEXTLINE(performance-no-int-to-ptr): GetProcAddress takes an ordinal as a pointer
      reinterpret_cast<const char*>(std::uintptr_t{100}));
  set_state_ = library_.GetSymbol<decltype(set_state_)>("XInputSetState");
  if (!get_capabilities_ || !get_state_ || !set_state_) {
    library_.Close();
    return Err(ErrorCategory::kPlatform, "xinput1_4.dll lacks a required export");
  }
  Poll();
  plug_poll_ = std::jthread([this](std::stop_token stop) {
    std::unique_lock lock(wake_mutex_);
    while (!stop.stop_requested()) {
      wake_.wait_for(lock, stop, kPollInterval, [] { return false; });
      if (stop.stop_requested()) {
        break;
      }
      lock.unlock();
      Poll();
      lock.lock();
    }
  });
  return {};
}

void XInputInputDriver::Shutdown() {
  if (plug_poll_.joinable()) {
    plug_poll_.request_stop();
    plug_poll_.join();
  }
  get_capabilities_ = nullptr;
  get_state_ = nullptr;
  get_state_ex_ = nullptr;
  set_state_ = nullptr;
  library_.Close();
}

void XInputInputDriver::Poll() {
  for (u32 slot = 0; slot < kSlotCount; ++slot) {
    XINPUT_CAPABILITIES native_caps;
    const bool present = get_capabilities_(slot, 0, &native_caps) == ERROR_SUCCESS;
    if (connected_[slot].exchange(present) != present) {
      ++revision_;
    }
  }
}

u64 XInputInputDriver::GetDeviceRevision() const {
  return revision_.load();
}

void XInputInputDriver::EnumerateDevices(std::vector<PadDevice>& out) {
  out.clear();
  for (u32 slot = 0; slot < kSlotCount; ++slot) {
    if (!connected_[slot]) {
      continue;
    }
    PadDevice device;
    device.id = DeviceForSlot(slot);
    device.name = "XInput Controller";
    device.info.type = PadType::kX360;
    device.identity.xinput_slot = static_cast<i32>(slot);
    out.push_back(std::move(device));
  }
}

Result<PadState> XInputInputDriver::GetDeviceState(PadDeviceID id) {
  u32 slot = 0;
  if (!SlotForDevice(id, &slot) || !connected_[slot]) {
    return std::unexpected(NotConnected());
  }
  struct {
    XINPUT_STATE state;
    unsigned int reserved;
  } native;
  const auto read = get_state_ex_ ? get_state_ex_ : get_state_;
  if (read(slot, &native.state) != ERROR_SUCCESS) {
    if (connected_[slot].exchange(false)) {
      ++revision_;
    }
    return std::unexpected(NotConnected());
  }
  PadState state;
  state.buttons = native.state.Gamepad.wButtons;
  state.left_trigger = native.state.Gamepad.bLeftTrigger;
  state.right_trigger = native.state.Gamepad.bRightTrigger;
  state.thumb_lx = native.state.Gamepad.sThumbLX;
  state.thumb_ly = native.state.Gamepad.sThumbLY;
  state.thumb_rx = native.state.Gamepad.sThumbRX;
  state.thumb_ry = native.state.Gamepad.sThumbRY;
  return state;
}

Result<PadCapabilities> XInputInputDriver::GetDeviceCapabilities(PadDeviceID id) {
  u32 slot = 0;
  if (!SlotForDevice(id, &slot) || !connected_[slot]) {
    return std::unexpected(NotConnected());
  }
  XINPUT_CAPABILITIES native;
  if (get_capabilities_(slot, 0, &native) != ERROR_SUCCESS) {
    return std::unexpected(NotConnected());
  }
  PadCapabilities caps;
  caps.subtype = static_cast<u8>(native.SubType);
  caps.flags = native.Flags;
  caps.supported.buttons = static_cast<u16>(native.Gamepad.wButtons | kGuideButton);
  caps.supported.left_trigger = native.Gamepad.bLeftTrigger;
  caps.supported.right_trigger = native.Gamepad.bRightTrigger;
  caps.supported.thumb_lx = native.Gamepad.sThumbLX;
  caps.supported.thumb_ly = native.Gamepad.sThumbLY;
  caps.supported.thumb_rx = native.Gamepad.sThumbRX;
  caps.supported.thumb_ry = native.Gamepad.sThumbRY;
  caps.motors.left_motor = native.Vibration.wLeftMotorSpeed;
  caps.motors.right_motor = native.Vibration.wRightMotorSpeed;
  return caps;
}

Result<void> XInputInputDriver::SetDeviceVibration(PadDeviceID id, const PadVibration& vibration) {
  u32 slot = 0;
  if (!SlotForDevice(id, &slot) || !connected_[slot]) {
    return std::unexpected(NotConnected());
  }
  XINPUT_VIBRATION native;
  native.wLeftMotorSpeed = vibration.left_motor;
  native.wRightMotorSpeed = vibration.right_motor;
  if (set_state_(slot, &native) != ERROR_SUCCESS) {
    return std::unexpected(NotConnected());
  }
  return {};
}

std::string_view XInputInputProvider::GetName() const {
  return "rexhid-xinput";
}

Result<void> XInputInputProvider::Connect(IInterfaceFactory&) {
  rex::memory::SetTagName(rex::hid::kHID, "HID");
  return {};
}

void XInputInputProvider::Disconnect() {}

Result<void> XInputInputProvider::Init() {
  return Ok();
}

void XInputInputProvider::Shutdown() {}

namespace {

std::unique_ptr<IDriver> CreateXInput() {
  return std::make_unique<XInputInputDriver>();
}

constexpr std::array<DriverRegistration, 1> kDrivers = {
    DriverRegistration{DriverType::kInput, "xinput", {}, &CreateXInput}};

}  // namespace

std::span<const DriverRegistration> XInputInputProvider::GetDrivers() const {
  return kDrivers;
}

}  // namespace rex::hid::xinput

REX_DECLARE_MODULE(rex::hid::xinput::XInputInputProvider)
