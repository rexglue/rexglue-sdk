/**
 * @file        hid/backend.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>

#include <rex/hid/device.h>
#include <rex/hid/error.h>
#include <rex/hid/event.h>
#include <rex/hid/input_host.h>
#include <rex/hid/xinput.h>
#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/move_only_function.h>

namespace rex::hid {

/** The hid config, read by the hid modules at Connect. */
struct HIDConfig {
  /** True maps every pad to user 0. False gives each pad its own user. */
  bool shared_pads = false;
  /** SDL gamepad mapping file. A relative path resolves against the executable folder. Empty
   * disables. */
  std::filesystem::path mappings_file = "gamecontrollerdb.txt";
};

/** Pads, keyboard and pointer, served by a hid module. Every method is safe on any thread. */
class IBackend {
 public:
  REX_INTERFACE(IBackend, "rex.hid.backend", 1);

  virtual ~IBackend() = default;

  /**
   * Reads the capabilities of the device a user holds.
   *
   * @param user Title user 0 to 3.
   * @param flags The XINPUT_FLAG bits the title passed.
   * @return The capabilities, or a HID error.
   */
  [[nodiscard]] virtual Result<X_INPUT_CAPABILITIES> GetCapabilities(u32 user, u32 flags) = 0;

  /**
   * Reads the merged pad state of a user. Neutral while a UI block is held.
   *
   * @param user Title user 0 to 3.
   * @return The state, or a HID error.
   */
  [[nodiscard]] virtual Result<X_INPUT_STATE> GetState(u32 user) = 0;

  /**
   * Sets the motor speeds of every pad of a user.
   *
   * @param user Title user 0 to 3.
   * @param vibration The speeds.
   * @return Ok when a pad took them, or a HID error.
   */
  [[nodiscard]] virtual Result<void> SetVibration(u32 user, const X_INPUT_VIBRATION& vibration) = 0;

  /**
   * Takes the next keystroke of a user.
   *
   * @param user Title user 0 to 3.
   * @param flags The XINPUT_FLAG bits the title passed.
   * @return The keystroke, or the HID error Empty or NotConnected.
   */
  [[nodiscard]] virtual Result<X_INPUT_KEYSTROKE> GetKeystroke(u32 user, u32 flags) = 0;

  /** Holds the title pads neutral while a system dialog is up. Counted. */
  virtual void AcquireUIBlock() = 0;

  /** Releases 1 UI block. Buttons held when the last block drops stay masked until released. */
  virtual void ReleaseUIBlock() = 0;

  /**
   * Sets the function told when a user gains or loses its pad.
   *
   * @param listener Receives the user and true for connected, empty to clear.
   */
  virtual void SetConnectionListener(rex::MoveOnlyFunction<void(u32, bool)> listener) = 0;

  /**
   * Attaches the window, null to detach. UI thread only. The host calls it with null before it
   * is destroyed.
   *
   * @param host The host.
   */
  virtual void SetHost(IInputHost* host) = 0;

  /**
   * Receives a window input event. UI thread only.
   *
   * @param event The event.
   */
  virtual void OnEvent(const InputEvent& event) = 0;

  /**
   * Reports whether a key is down.
   *
   * @param key The key.
   * @return True while held.
   */
  virtual bool IsKeyDown(VirtualKey key) const = 0;

  /**
   * Reports whether any key is down.
   *
   * @return True while a key is held.
   */
  virtual bool AnyKeyDown() const = 0;

  /**
   * Returns the held modifiers.
   *
   * @return The modifiers.
   */
  virtual KeyModifiers modifiers() const = 0;

  /**
   * Reports whether the window has focus.
   *
   * @return True while focused.
   */
  virtual bool focused() const = 0;

  /**
   * Returns the pointer position and the window size.
   *
   * @return The state.
   */
  virtual PointerState pointer() const = 0;

  /**
   * Reports whether a mouse button is down.
   *
   * @param button The button.
   * @return True while held.
   */
  virtual bool IsButtonDown(MouseButton button) const = 0;

  /**
   * Reports whether any mouse button is down.
   *
   * @return True while a button is held.
   */
  virtual bool AnyButtonDown() const = 0;

  /**
   * Reports whether the pointer moved since the last call, and clears it.
   *
   * @return True once per run of motion.
   */
  virtual bool TakePointerMoved() = 0;

  /**
   * Returns the Locked mode motion since the last call, and clears it.
   *
   * @return The motion.
   */
  virtual PointerDelta TakeMotion() = 0;

  /**
   * Returns the Locked mode motion since the last TakeMotion.
   *
   * @return The motion.
   */
  virtual PointerDelta PeekMotion() const = 0;

  /**
   * Returns the wheel detents since the last call, and clears them.
   *
   * @return Signed detents.
   */
  virtual i32 TakeWheel() = 0;

  /**
   * Returns the detents the last TakeWheel returned.
   *
   * @return Signed detents.
   */
  virtual i32 wheel() const = 0;

  /**
   * Sets the pointer mode the consumer wants. Applied on the UI thread, last call wins. Mouse look
   * of the keyboard and mouse pad overrides it with Locked while active.
   *
   * @param mode The mode.
   */
  virtual void SetPointerMode(PointerMode mode) = 0;

  /**
   * Returns the pointer mode the consumer set.
   *
   * @return The mode.
   */
  virtual PointerMode pointer_mode() const = 0;
};

/** The null HID backend. 1 idle pad on user 0, no keyboard and no pointer. */
class REX_API NullBackend final : public IBackend, public NonCopyable {
 public:
  /**
   * Reports the gamepad capabilities of the idle pad.
   *
   * @param user Title user 0 to 3.
   * @param flags Ignored.
   * @return The capabilities on user 0, NotConnected on any other.
   */
  [[nodiscard]] Result<X_INPUT_CAPABILITIES> GetCapabilities(u32 user, u32 flags) override;

  /**
   * Reads the idle pad. The first read on user 0 reports it connected.
   *
   * @param user Title user 0 to 3.
   * @return A neutral state on user 0, NotConnected on any other.
   */
  [[nodiscard]] Result<X_INPUT_STATE> GetState(u32 user) override;

  /**
   * Accepts the motor speeds and drops them.
   *
   * @param user Title user 0 to 3.
   * @param vibration Ignored.
   * @return Ok on user 0, NotConnected on any other.
   */
  [[nodiscard]] Result<void> SetVibration(u32 user, const X_INPUT_VIBRATION& vibration) override;

  /**
   * Reports that the idle pad has no keystrokes.
   *
   * @param user Title user 0 to 3.
   * @param flags Ignored.
   * @return Empty on user 0, NotConnected on any other.
   */
  [[nodiscard]] Result<X_INPUT_KEYSTROKE> GetKeystroke(u32 user, u32 flags) override;

  /** Does nothing, the idle pad is always neutral. */
  void AcquireUIBlock() override {}

  /** Does nothing. */
  void ReleaseUIBlock() override {}

  /**
   * Sets the function told when user 0 gains the idle pad.
   *
   * @param listener Receives the user and true for connected, empty to clear.
   */
  void SetConnectionListener(rex::MoveOnlyFunction<void(u32, bool)> listener) override;

  /**
   * Does nothing, there is no window side.
   *
   * @param host Ignored.
   */
  void SetHost(IInputHost* host) override { (void)host; }

  /**
   * Drops the event.
   *
   * @param event Ignored.
   */
  void OnEvent(const InputEvent& event) override { (void)event; }

  /**
   * Reports no key down.
   *
   * @param key Ignored.
   * @return False.
   */
  bool IsKeyDown(VirtualKey key) const override {
    (void)key;
    return false;
  }

  /**
   * Reports no key down.
   *
   * @return False.
   */
  bool AnyKeyDown() const override { return false; }

  /**
   * Reports no modifier held.
   *
   * @return KeyModifiers::None.
   */
  KeyModifiers modifiers() const override { return KeyModifiers::None; }

  /**
   * Reports focus, so pads stay live.
   *
   * @return True.
   */
  bool focused() const override { return true; }

  /**
   * Reports no pointer.
   *
   * @return An empty state.
   */
  PointerState pointer() const override { return {}; }

  /**
   * Reports no button down.
   *
   * @param button Ignored.
   * @return False.
   */
  bool IsButtonDown(MouseButton button) const override {
    (void)button;
    return false;
  }

  /**
   * Reports no button down.
   *
   * @return False.
   */
  bool AnyButtonDown() const override { return false; }

  /**
   * Reports no motion.
   *
   * @return False.
   */
  bool TakePointerMoved() override { return false; }

  /**
   * Reports no motion.
   *
   * @return A zero delta.
   */
  PointerDelta TakeMotion() override { return {}; }

  /**
   * Reports no motion.
   *
   * @return A zero delta.
   */
  PointerDelta PeekMotion() const override { return {}; }

  /**
   * Reports no wheel motion.
   *
   * @return 0.
   */
  i32 TakeWheel() override { return 0; }

  /**
   * Reports no wheel motion.
   *
   * @return 0.
   */
  i32 wheel() const override { return 0; }

  /**
   * Does nothing, there is no pointer.
   *
   * @param mode Ignored.
   */
  void SetPointerMode(PointerMode mode) override { (void)mode; }

  /**
   * Reports the free pointer.
   *
   * @return PointerMode::Free.
   */
  PointerMode pointer_mode() const override { return PointerMode::Free; }

 private:
  std::mutex mutex_;
  rex::MoveOnlyFunction<void(u32, bool)> listener_;
  bool reported_ = false;
};

}  // namespace rex::hid
