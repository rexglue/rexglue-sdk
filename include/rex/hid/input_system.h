/**
 * @file        hid/input_system.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <rex/hid/binding.h>
#include <rex/hid/input_keys.h>
#include <rex/hid/pad.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::hid {

class REX_HID_API InputSettings final : public SettingOwner {
 public:
  InputSettings();

  REX_SETTING(std::string, drivers, "sdl3", "Input drivers in priority order, comma separated",
              kStartupOnly);
  REX_SETTING(bool, shared_pads, false, "Every pad feeds user 0", kStartupOnly);
  REX_SETTING(bool, vibration, true, "Allow vibration");
  REX_SETTING(bool, guide_button, false, "Pass the guide button to the title");
  REX_SETTING(f64, left_stick_deadzone, 0.0, "Left stick deadzone", Range{0.0, 1.0});
  REX_SETTING(f64, right_stick_deadzone, 0.0, "Right stick deadzone", Range{0.0, 1.0});
  REX_SETTING(bool, mnk_mode, false, "Keyboard and mouse act as a pad");
  REX_SETTING(bool, mnk_mouse, false, "The mouse moves the right stick");
  REX_SETTING(f64, mnk_sensitivity, 1.0, "Mouse to stick sensitivity", Range{0.01, 100.0});
  REX_SETTING(bool, mnk_passthrough, false, "Keys reach the title as keystrokes");
};

/**
 * Merges the selected drivers into four users. A pad seen by two drivers
 * counts once, from the driver listed first in hid.drivers. User 0 always has a pad, a neutral
 * one when nothing else is present. Without input capture only the keyboard and mouse source reads
 * neutral, and AcquireUIBlock makes every user neutral. Any thread.
 */
REX_DECLARE_INTERFACE(IInputSystem, 1)
  static constexpr u32 kMaxUsers = 4;

  /** The user value for GetKeystroke that takes a keystroke from any user. */
  static constexpr u32 kAnyUser = 0xFF;

  struct PadReading {
    u32 packet = 0; /**< Increases whenever the state changes. */
    PadState state; /**< After deadzone and guide masking, neutral while blocked or not captured. */
  };

  virtual InputSettings& GetSettings() = 0;

  /**
   * Reads a user's pad.
   *
   * @param user Slot 0 to 3.
   * @return The reading, or kNotFound when no pad is assigned to the user.
   */
  [[nodiscard]] virtual Result<PadReading> GetState(u32 user) = 0;

  /**
   * Returns the capabilities of a user's pad, with the guide bit cleared when guide_button is off,
   * or kNotFound.
   *
   * @param user Slot 0 to 3.
   */
  [[nodiscard]] virtual Result<PadCapabilities> GetCapabilities(u32 user) = 0;

  /**
   * Sets a user's motors. Sends zero speeds while vibration is off.
   *
   * @param user Slot 0 to 3.
   * @return Ok, or an error when no pad is assigned to the user.
   */
  [[nodiscard]] virtual Result<void> SetVibration(u32 user, const PadVibration& motors) = 0;

  /**
   * Takes the next keystroke.
   *
   * @param user 0 to 3, or kAnyUser.
   * @return The keystroke, or kNotFound when none is waiting.
   */
  [[nodiscard]] virtual Result<PadKeystroke> GetKeystroke(u32 user) = 0;

  /**
   * Returns which pad a user holds, or kNotFound.
   *
   * @param user Slot 0 to 3.
   */
  [[nodiscard]] virtual Result<PadInfo> GetPadInfo(u32 user) = 0;

  /**
   * Calls a function when a user's pad connects or disconnects, on the detecting thread with no
   * lock held.
   *
   * @param fn Receives the user and whether a pad is now connected.
   * @return The guard, destroyed to stop the calls and wait for one in progress.
   */
  [[nodiscard]] virtual ScopeGuard OnConnectionChange(
      MoveOnlyFunction<void(u32 user, bool connected)> fn) = 0;

  /**
   * Hides pads from the title while a dialog or debug window is open. Calls nest.
   */
  virtual void AcquireUIBlock() = 0;

  virtual void ReleaseUIBlock() = 0;

  virtual bool IsKeyDown(hid::VirtualKey key) const = 0;

  virtual bool IsAnyKeyDown() const = 0;

  virtual hid::KeyModifiers GetModifiers() const = 0;

  /**
   * @return The pointer position and the window size.
   */
  virtual hid::PointerState GetPointer() const = 0;

  virtual bool IsButtonDown(hid::MouseButton button) const = 0;

  virtual bool IsAnyButtonDown() const = 0;

  /** Reports whether the pointer moved since the last call, and clears it. */
  virtual bool TakePointerMoved() = 0;

  /**
   * Returns the motion since the last call, and clears it.
   */
  virtual hid::PointerDelta TakeMotion() = 0;

  /**
   * Returns the motion since the last TakeMotion.
   */
  virtual hid::PointerDelta PeekMotion() const = 0;

  /**
   * Returns the wheel turns since the last call, and clears them, positive away from the player.
   */
  virtual i32 TakeWheel() = 0;

  virtual i32 GetWheel() const = 0;

  /**
   * Sets the pointer mode. The UI system applies it on the UI thread.
   */
  virtual void SetPointerMode(hid::PointerMode mode) = 0;

  /**
   * Receives a window event from the UI system.
   *
   * @param event The event, which the call may change.
   */
  virtual void OnInputEvent(hid::InputEvent& event) = 0;

  virtual void SetInputCaptured(bool captured) = 0;

  /**
   * Returns the pointer mode the UI applies.
   */
  virtual hid::PointerMode GetPointerModeToApply() const = 0;

  /**
   * Sets the function called when the pointer mode to apply may have changed, replacing the earlier
   * one.
   *
   * @return The guard, destroyed to remove the listener and wait for a call in progress.
   */
  [[nodiscard]] virtual ScopeGuard SetPointerModeListener(MoveOnlyFunction<void()> listener) = 0;

  /**
   * Returns the 25 bindings of the hid.keybind_* settings, in button and stick order, used on the
   * UI thread.
   */
  virtual hid::IBindingSet& GetKeyBindings() = 0;

  /**
   * Creates an unregistered set of the bindings the application declares.
   *
   * @param configs The bindings, addressed by index. The descriptions must outlive the set.
   * @param scope The setting scope, a name without a dot.
   * @return The set, which must not outlive the input system.
   */
  [[nodiscard]] virtual std::unique_ptr<hid::IBindingSet> CreateBindingSet(
      std::span<const hid::BindingConfig> configs, std::string_view scope) = 0;

  /**
   * Creates a capture of the next keyboard or mouse input for a binding.
   *
   * @param config How the capture is canceled.
   * @return The capture, which must not outlive the input system.
   */
  [[nodiscard]] virtual std::unique_ptr<hid::IKeyCapture> CreateKeyCapture(
      hid::KeyCaptureConfig config) = 0;

  /**
   * Parses the text of a key chord such as "Ctrl+F3".
   *
   * @return The chord, or the reason the text does not parse.
   */
  [[nodiscard]] virtual Result<hid::KeyChord> ParseKeyChord(std::string_view text) const = 0;

  /**
   * Writes a key chord as text, the form ParseKeyChord reads.
   *
   * @return The text, empty for a chord with no key.
   */
  [[nodiscard]] virtual std::string FormatKeyChord(hid::KeyChord chord) const = 0;

  /**
   * Parses a key name.
   *
   * @param name A key name such as "F3", "Backtick", "A" or "Escape".
   * @return The key, or VirtualKey::kNone for an unknown name.
   */
  [[nodiscard]] virtual hid::VirtualKey ParseVirtualKey(std::string_view name) const = 0;

  /**
   * @return The name ParseVirtualKey accepts, or empty for a key with no name.
   */
  [[nodiscard]] virtual std::string VirtualKeyToString(hid::VirtualKey key) const = 0;

  /**
   * Parses one input such as "Shift+W", "LMB", "WheelUp", "PadA" or "LStick". Key text follows
   * ParseKeyChord and pad names take no modifiers. Names are case sensitive.
   *
   * @return The source, or ErrorCategory::kConfig naming text when it is empty, a bare modifier or
   *         names nothing.
   */
  [[nodiscard]] virtual Result<hid::InputSource> ParseInputSource(std::string_view text) const = 0;

  /**
   * Writes a source as binding text, modifiers in the order Shift, Ctrl, Alt and then the name.
   *
   * @return The text ParseInputSource reads back, or empty when the source names nothing.
   */
  [[nodiscard]] virtual std::string InputSourceToString(const hid::InputSource& source) const = 0;
};

}  // namespace rex::hid
