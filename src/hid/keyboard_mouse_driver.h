/**
 * @file        hid/keyboard_mouse_driver.h
 * @brief       Keyboard and mouse pad emulation and raw keyboard passthrough
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
#include <mutex>
#include <queue>
#include <string_view>
#include <vector>

#include <rex/cvar/registry.h>
#include <rex/hid/driver.h>
#include <rex/hid/key_chord.h>

namespace rex::hid {

class Hub;

/** Emulates a pad from the hid.keybind_* keys and the mouse, or passes raw keys through. */
class KeyboardMouseDriver final : public Driver {
 public:
  /**
   * @param hub The hub the driver belongs to, asked to reapply the pointer mode when mouse look
   *        starts or stops.
   */
  explicit KeyboardMouseDriver(Hub& hub);

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  [[nodiscard]] Result<X_INPUT_STATE> GetDeviceState(DeviceId id) override;
  [[nodiscard]] Result<X_INPUT_CAPABILITIES> GetDeviceCapabilities(DeviceId id, u32 flags) override;
  [[nodiscard]] Result<void> SetDeviceVibration(DeviceId id,
                                                const X_INPUT_VIBRATION& vibration) override;
  [[nodiscard]] Result<X_INPUT_KEYSTROKE> GetDeviceKeystroke(DeviceId id, u32 flags) override;
  void OnEvent(const InputEvent& event) override;

  /**
   * Reports whether mouse look wants the pointer locked.
   *
   * @return True while hid.mnk_mouse emulation is active, focused and captured.
   */
  bool WantsPointerLock() const { return pointer_lock_.load(std::memory_order_relaxed); }

  /** Number of hid.keybind_* cvars the driver follows. */
  static constexpr size_t kBindCount = 25;

  /**
   * Parses every hid.keybind_* value once and parses it again after each change. An item that
   * names no key is logged and skipped. A value whose every item fails keeps the previous chords.
   *
   * @param cvars The registry that serves the hid cvars. It outlives the attachment.
   */
  void Attach(cvar::ICvarRegistry& cvars);

  /** Stops following the hid.keybind_* cvars and drops the parsed chords. */
  void Detach();

 private:
  void SetBind(size_t index, std::string_view text);
  bool IsEnabled() const;
  void SetKeyState(u16 key, bool down);
  void EnqueueKeystroke(u16 pad_key, bool down);
  void PushKeystroke(const X_INPUT_KEYSTROKE& keystroke);
  void EnqueueRawKeystroke(const InputEvent& event, bool down);
  void RefreshBoundKeystrokesLocked();
  void OnKey(const InputEvent& event, bool down);
  void OnChar(const InputEvent& event);
  void OnButton(const InputEvent& event, bool down);
  void OnMotion(const InputEvent& event);
  void OnFocusLost();

  Hub& hub_;
  std::atomic<bool> pointer_lock_{false};
  std::mutex state_mutex_;
  bool key_down_[256] = {};
  f32 mouse_dx_ = 0.0f;
  f32 mouse_dy_ = 0.0f;
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;
  std::array<std::vector<KeyChord>, kBindCount> binds_;
  std::array<cvar::CallbackHandle, kBindCount> handles_;
  u32 bound_pressed_ = 0;
  u32 packet_number_ = 0;
};

}  // namespace rex::hid
