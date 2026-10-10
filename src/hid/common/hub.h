/**
 * @file        hid/common/hub.h
 * @brief       The HID backend every hid module serves, aggregating its pad drivers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/hid/backend.h>
#include <rex/hid/driver.h>
#include <rex/move_only_function.h>

#include "hid/common/device_assignment.h"
#include "hid/common/state_merge.h"

namespace rex::cvar {
class ICvarRegistry;
}  // namespace rex::cvar

namespace rex::hid {

class KeyboardMouseDriver;

/**
 * Assigns the devices of its drivers to title users, merges their state, keeps the keyboard and
 * pointer state and applies the pointer mode through the input host. Holds 1 keyboard and mouse
 * driver and 1 idle pad driver behind the pad drivers a module adds.
 */
class Hub final : public IBackend, public NonCopyable {
 public:
  Hub();
  ~Hub() override;

  /**
   * Adds a pad driver ahead of the keyboard and mouse driver.
   *
   * @param driver The driver.
   */
  void AddPadDriver(std::unique_ptr<Driver> driver);

  /** Removes every pad driver. */
  void ClearPadDrivers();

  /**
   * Replaces the device assignment. Slot assignment is the default.
   *
   * @param assignment The assignment.
   */
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  /**
   * Starts the keyboard and mouse driver parsing the hid.keybind_* cvars and following their
   * changes.
   *
   * @param cvars The registry that serves the hid cvars. It outlives the attachment.
   */
  void AttachCvars(cvar::ICvarRegistry& cvars);

  /** Stops the keyboard and mouse driver following the hid.keybind_* cvars. */
  void DetachCvars();

  /** Asks the UI thread to reapply the pointer mode. Safe on any thread. */
  void RefreshPointerMode();

  [[nodiscard]] Result<X_INPUT_CAPABILITIES> GetCapabilities(u32 user, u32 flags) override;
  [[nodiscard]] Result<X_INPUT_STATE> GetState(u32 user) override;
  [[nodiscard]] Result<void> SetVibration(u32 user, const X_INPUT_VIBRATION& vibration) override;
  [[nodiscard]] Result<X_INPUT_KEYSTROKE> GetKeystroke(u32 user, u32 flags) override;
  void AcquireUIBlock() override;
  void ReleaseUIBlock() override;
  void SetConnectionListener(rex::MoveOnlyFunction<void(u32, bool)> listener) override;
  void SetHost(IInputHost* host) override;
  void OnEvent(const InputEvent& event) override;
  bool IsKeyDown(VirtualKey key) const override;
  bool AnyKeyDown() const override;
  KeyModifiers modifiers() const override;
  bool focused() const override;
  PointerState pointer() const override;
  bool IsButtonDown(MouseButton button) const override;
  bool AnyButtonDown() const override;
  bool TakePointerMoved() override;
  PointerDelta TakeMotion() override;
  PointerDelta PeekMotion() const override;
  i32 TakeWheel() override;
  i32 wheel() const override;
  void SetPointerMode(PointerMode mode) override;
  PointerMode pointer_mode() const override;

 private:
  using JoystickValue = std::pair<u16, u16>;

  void RefreshDevicesLocked();
  Driver* DriverForDeviceLocked(DeviceId id);
  const DeviceInfo* DeviceInfoForLocked(DeviceId id) const;
  DeviceId ChooseDeviceForUserLocked(u32 user) const;
  [[nodiscard]] Result<X_INPUT_STATE> ReadStateLocked(u32 user);
  void UpdateConnectedUserLocked(u32 user, bool connected);
  void AdjustDeadzoneLevels(u32 user, X_INPUT_GAMEPAD& gamepad) const;
  PointerMode EffectiveMode() const;
  void TrackCapture();
  void ApplyPointerModeFromUIThread();
  InputEvent TrackMotion(const InputEvent& event);
  void Recenter(f32 x, f32 y);
  void ResetOnFocusLoss();
  void SetKey(VirtualKey key, bool down);

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Driver>> drivers_;
  size_t pad_drivers_ = 0;
  KeyboardMouseDriver* keyboard_mouse_ = nullptr;
  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;
  std::vector<DeviceInfo> devices_;
  std::vector<Driver*> device_owners_;
  std::bitset<kMaxGuestUsers> connected_users_;
  std::array<std::pair<JoystickValue, JoystickValue>, kMaxGuestUsers> user_max_joystick_value_{};
  std::array<u16, kMaxGuestUsers> consumed_buttons_{};
  std::atomic<i32> ui_blocks_{0};
  rex::MoveOnlyFunction<void(u32, bool)> connection_listener_;

  std::atomic<IInputHost*> host_{nullptr};
  std::atomic<PointerMode> requested_mode_{PointerMode::Free};
  std::atomic<bool> pointer_update_queued_{false};
  PointerMode applied_mode_ = PointerMode::Free;
  bool relative_ = false;
  f32 lock_x_ = 0.0f;
  f32 lock_y_ = 0.0f;

  std::array<std::atomic<u64>, 4> keys_{};
  std::atomic<bool> focused_{true};
  std::atomic<bool> capture_{true};
  std::atomic<f32> x_{0.0f};
  std::atomic<f32> y_{0.0f};
  std::atomic<u32> width_{0};
  std::atomic<u32> height_{0};
  std::atomic<bool> has_position_{false};
  std::atomic<bool> moved_{false};
  std::atomic<i32> wheel_accum_{0};
  std::atomic<i32> wheel_taken_{0};
  std::atomic<f32> motion_dx_{0.0f};
  std::atomic<f32> motion_dy_{0.0f};
  std::atomic<u32> buttons_{0};
};

}  // namespace rex::hid
