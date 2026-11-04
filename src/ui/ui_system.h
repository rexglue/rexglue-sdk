/**
 * @file        ui/ui_system.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <memory>

#include <rex/ui/ui_system.h>

#include <rex/logging/log_category.h>
#include <rex/ui/ui_thread.h>
#include <rex/ui/overlay_renderer.h>

#include "ui/ui_control.h"

REXLOG_TABLE(kUILog, ui, console)

namespace rex {
class ILogger;
class ISettingsRegistry;
}  // namespace rex

namespace rex::apu {
class IAudioSystem;
}  // namespace rex::apu

namespace rex::gpu {
class IGraphicsSystem;
}  // namespace rex::gpu

namespace rex::hid {
class IInputSystem;
}  // namespace rex::hid

namespace rex::ui {
class IUIDriver;
}  // namespace rex::ui

namespace rex::ui {

/** Owns the window, the frame loop and the overlays. The app starts and stops it. */
class UISystem final : public rex::ui::IUISystem, public IUIControl, public IUIThread {
 public:
  UISystem();
  ~UISystem() override;

  rex::ui::UISettings& GetSettings() override { return settings_; }
  bool IsHeadless() const override;
  bool HasWindow() const override;
  bool IsFocused() const override;
  VideoMode GetVideoMode() const override;
  [[nodiscard]] Result<NativeWindow> GetNativeWindow() const override;
  void RaiseWindow() override;
  bool IsUIThread() const override;
  void PostTask(MoveOnlyFunction<void()> fn) override;
  [[nodiscard]] bool RunSynchronous(MoveOnlyFunction<void()> fn) override;
  [[nodiscard]] ScopeGuard AddEventListener(rex::ui::IUIEventListener& listener) override;
  ImFontAtlas& GetFonts() override;
  ImGuiStyle& GetImGuiStyle() override;
  ImGuiIO& GetImGuiIO() override;
  ui::Style& GetStyle() override;
  [[nodiscard]] Result<void> AddOverlays(std::span<const rex::ui::OverlayConfig> overlays) override;
  void ShowOverlay(std::string_view name, bool visible) override;
  void ToggleOverlay(std::string_view name) override;
  [[nodiscard]] Result<ui::IDialog*> GetOverlay(std::string_view name) override;
  bool IsAnyOverlayVisible() const override;
  [[nodiscard]] ScopeGuard AddDialog(ui::IDialog& dialog) override;
  void AddDialog(std::unique_ptr<ui::IDialog> dialog) override;
  bool HasDialogs() const override;
  void DrawOverlays(IOverlayRenderer& renderer) override;
  void ShowMessage(MessageType type, std::string_view heading, std::string_view body) override;
  [[nodiscard]] Result<u32> ShowChoice(const ui::ChoiceConfig& config) override;
  [[nodiscard]] Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const ui::FileDialogConfig& config) override;
  void ShowPrompt(ui::PromptConfig config, MoveOnlyFunction<void(ui::PromptResult)> done) override;
  void CancelPrompts() override;
  std::vector<ui::Monitor> GetDisplays() override;
  std::vector<ui::HostLocale> GetPreferredLocales() override;

  [[nodiscard]] Result<void> AttachTitle(const TitleInfo& info,
                                         IAchievementRegistry& achievements) override;
  void DetachTitle() override;
  void EndFrameLoop() override;
  void SetCloseHandler(MoveOnlyFunction<void()> handler) override;
  void SetResizeHandler(MoveOnlyFunction<void(u32, u32)> handler) override;
  bool ServePosts(std::chrono::milliseconds timeout) override;

  /**
   * Starts the system on the calling thread, which becomes the UI thread. Without an input system
   * the UI shows binding text unparsed and does not edit chords.
   *
   * @param ui_driver The driver, or null to run headless.
   * @param input The input system, or null for none.
   * @param settings The settings registry the console and the overlays use.
   * @param logger The logger the log capture attaches to.
   * @return Success, or the reason the system cannot start.
   */
  [[nodiscard]] Result<void> Start(rex::ui::IUIDriver* ui_driver, rex::hid::IInputSystem* input,
                                   rex::apu::IAudioSystem& audio,
                                   rex::gpu::IGraphicsSystem& graphics, ISettingsRegistry& settings,
                                   ILogger& logger);

  void BeginStop() override;

  void Stop();

  bool RunFrame(u32 timeout_ms) override;

 private:
  rex::ui::UISettings settings_;
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::ui
