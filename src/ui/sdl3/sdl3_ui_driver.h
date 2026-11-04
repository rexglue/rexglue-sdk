/**
 * @file        ui/sdl3/sdl3_ui_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/ui/native_window.h>
#include <rex/setting.h>
#include <rex/ui/ui_driver.h>
#include <rex/ui/dialog.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>

namespace rex::ui::sdl3 {

class SDL3UIDriver final : public IUIDriver, public SettingOwner {
 public:
  REX_SETTING(std::string, video_driver, "",
              "SDL video driver, empty for SDL's default; dummy runs with no display",
              kStartupOnly);

  SDL3UIDriver();
  ~SDL3UIDriver() override;

  std::string_view GetName() const override;
  [[nodiscard]] Result<void> Initialize(IDriverContext& context) override;
  void Shutdown() override;

  bool HasDisplay() const override;
  [[nodiscard]] Result<void> OpenWindow(const WindowConfig& config) override;
  void CloseUIWindow() override;
  void SetTitle(std::string_view title) override;
  void SetSize(u32 width, u32 height) override;
  void SetLimits(u32 min_width, u32 min_height, AspectRatio ratio) override;
  void SetFullscreen(bool fullscreen, bool exclusive, i32 monitor, u32 mode_width,
                     u32 mode_height) override;
  void RaiseWindow() override;
  void SetCursorShape(bool visible, CursorShape shape) override;
  void SetPointerMode(hid::PointerMode mode) override;
  void CaptureMouse(bool captured) override;
  f32 GetContentScale() const override;
  WindowSize GetWindowSize() const override;
  bool HasFocus() const override;
  void SetTextInputActive(bool active) override;
  [[nodiscard]] Result<NativeWindow> GetNativeWindow() const override;
  bool WaitEvents(IUIEventListener& listener, u32 timeout_ms) override;
  void WakeUp() override;
  [[nodiscard]] Result<u32> ShowMessageBox(MessageType type, std::string_view title,
                                           std::string_view body,
                                           std::span<const std::string> buttons) override;
  [[nodiscard]] Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const FileDialogConfig& config) override;
  std::vector<Monitor> GetDisplays() override;
  std::vector<HostLocale> GetPreferredLocales() override;
  std::string GetClipboardText() override;
  void SetClipboardText(std::string_view text) override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

class SDL3UIProvider final : public IModule, public IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IDriverModule>(*this, name, version);
  }
  std::span<const DriverRegistration> GetDrivers() const override;
};

}  // namespace rex::ui::sdl3
