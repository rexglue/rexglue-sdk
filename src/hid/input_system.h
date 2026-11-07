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

#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/hid/input_system.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include <rex/logging/log_category.h>

REXLOG_TABLE(kInputLog, hid)

namespace rex::hid {

/** Merges the pads of the selected drivers. The app starts and stops it. */
class InputSystem final : public rex::hid::IInputSystem {
 public:
  InputSystem();
  ~InputSystem() override;

  rex::hid::InputSettings& GetSettings() override { return settings_; }
  [[nodiscard]] Result<PadReading> GetState(u32 user) override;
  [[nodiscard]] Result<rex::hid::PadCapabilities> GetCapabilities(u32 user) override;
  [[nodiscard]] Result<void> SetVibration(u32 user, const rex::hid::PadVibration& motors) override;
  [[nodiscard]] Result<rex::hid::PadKeystroke> GetKeystroke(u32 user,
                                                            KeystrokeSource sources) override;
  [[nodiscard]] Result<rex::hid::PadInfo> GetPadInfo(u32 user) override;
  [[nodiscard]] ScopeGuard OnConnectionChange(
      MoveOnlyFunction<void(u32 user, bool connected)> fn) override;
  void AcquireUIBlock() override;
  void ReleaseUIBlock() override;
  bool IsKeyDown(VirtualKey key) const override;
  bool IsAnyKeyDown() const override;
  KeyModifiers GetModifiers() const override;
  PointerState GetPointer() const override;
  bool IsButtonDown(MouseButton button) const override;
  bool IsAnyButtonDown() const override;
  bool TakePointerMoved() override;
  PointerDelta TakeMotion() override;
  PointerDelta PeekMotion() const override;
  i32 TakeWheel() override;
  i32 GetWheel() const override;
  void SetPointerMode(PointerMode mode) override;
  hid::IBindingSet& GetKeyBindings() override;
  [[nodiscard]] std::unique_ptr<hid::IBindingSet> CreateBindingSet(
      std::span<const hid::BindingConfig> configs, std::string_view scope) override;
  [[nodiscard]] std::unique_ptr<hid::IKeyCapture> CreateKeyCapture(
      hid::KeyCaptureConfig config) override;
  [[nodiscard]] Result<KeyChord> ParseKeyChord(std::string_view text) const override;
  [[nodiscard]] std::string FormatKeyChord(KeyChord chord) const override;
  [[nodiscard]] VirtualKey ParseVirtualKey(std::string_view name) const override;
  [[nodiscard]] std::string VirtualKeyToString(VirtualKey key) const override;
  [[nodiscard]] Result<InputSource> ParseInputSource(std::string_view text) const override;
  [[nodiscard]] std::string InputSourceToString(const InputSource& source) const override;

  void OnInputEvent(InputEvent& event) override;
  void SetInputCaptured(bool captured) override;
  PointerMode GetPointerModeToApply() const override;
  [[nodiscard]] ScopeGuard SetPointerModeListener(MoveOnlyFunction<void()> listener) override;

  [[nodiscard]] Result<void> RegisterBindings(ISettingsRegistry& settings);
  void UnregisterBindings();

  /**
   * Starts merging the drivers.
   *
   * @param selected The initialized drivers in priority order.
   * @return Success, or the reason the system cannot start.
   */
  [[nodiscard]] Result<void> Start(std::span<rex::hid::IInputDriver* const> selected);

  /** Stops merging and forgets the drivers. */
  void Stop();

 private:
  rex::hid::InputSettings settings_;
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::hid
