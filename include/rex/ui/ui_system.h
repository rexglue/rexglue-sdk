/**
 * @file        ui/ui_system.h
 * @brief       UI system, overlays and dialogs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 */

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/input_keys.h>
#include <rex/module.h>
#include <rex/ui/dialog.h>
#include <rex/platform/native_window.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>
#include <rex/utility.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/title.h>
#include <rex/types.h>
#include <rex/overlay_renderer.h>

struct ImGuiIO;

namespace rex::ui {

/**
 * A window the UI system draws with ImGui. Add it with IUISystem::AddDialog. Every hook runs on the
 * UI thread. Derive from DialogBase to take the defaults.
 */
class IDialog {
 public:
  virtual ~IDialog() = default;

  /**
   * @param io The IO of the current ImGui context.
   * @return False to close the dialog. The system then calls OnClose once and removes the dialog.
   */
  virtual bool Draw(ImGuiIO& io) = 0;

  /** Called once after Draw returned false, before the system removes the dialog. */
  virtual void OnClose() = 0;

  /**
   * Reports whether the dialog needs a repaint after every frame.
   *
   * @return True when the dialog changes without outside events.
   */
  virtual bool NeedsContinuousPaint() const = 0;

  /**
   * Reports whether the dialog takes all title input while it shows, the pads included.
   *
   * @return True to block the title input.
   */
  virtual bool WantsExclusiveInput() const = 0;

  virtual void OnShow() = 0;

  /**
   * Lets an overlay handle its toggle bind while hidden.
   *
   * @return True when the dialog handled the toggle and stays hidden.
   */
  virtual bool OnToggle() = 0;
};

/** An IDialog with default hooks. Draw stays pure. */
class DialogBase : public IDialog {
 public:
  void OnClose() override {}

  bool NeedsContinuousPaint() const override { return true; }

  bool WantsExclusiveInput() const override { return false; }

  void OnShow() override {}

  bool OnToggle() override { return false; }
};

}  // namespace rex::ui

struct ImDrawData;
struct ImFontAtlas;
struct ImGuiIO;
struct ImGuiStyle;

namespace rex::ui {

class IUISystem;
struct Style;

/** One overlay a program adds. The name sets its toggle hotkey setting ui.bind.<name>. */
struct OverlayConfig {
  std::string_view name; /**< Unique name. */
  KeyChord default_key;  /**< Default toggle chord, kNone registers no hotkey. */
  /** Creates the overlay. Called on the UI thread. */
  std::unique_ptr<ui::IDialog> (*create)(IUISystem& ui);
};

class REX_UI_API UISettings final : public SettingOwner {
 public:
  UISettings();

  REX_SETTING(std::string, driver, "sdl3",
              "UI driver, none for headless in an app that does not require a UI", kStartupOnly);
  REX_SETTING(std::string, title, "", "Window title, the app name when empty");
  REX_SETTING(i32, window_width, 0, "Window width, 0 for the video mode width", Range{0, 16384});
  REX_SETTING(i32, window_height, 0, "Window height, 0 for the video mode height", Range{0, 16384});
  REX_SETTING(bool, fullscreen, true, "Fullscreen");
  REX_SETTING(bool, fullscreen_exclusive, false, "Exclusive fullscreen");
  REX_SETTING(i32, monitor, 0, "Display to open on", Range{0, 16});
  REX_SETTING(i32, min_width, 0, "Smallest window width", Range{0, 16384});
  REX_SETTING(i32, min_height, 0, "Smallest window height", Range{0, 16384});
  REX_SETTING(std::string, aspect_ratio, "stretch", "Window shape",
              Allowed("stretch", "4:3", "5:4", "3:2", "16:10", "16:9", "21:9", "32:9"));
  REX_SETTING(std::string, resolution, "",
              "Video mode preset or WxH, empty to use the ui.video_mode_* settings");
  REX_SETTING(i32, video_mode_width, 1280, "Video mode width the title sees", Range{640, 4095},
              kRestart);
  REX_SETTING(i32, video_mode_height, 720, "Video mode height the title sees", Range{480, 4095},
              kRestart);
  REX_SETTING(f64, video_mode_refresh_rate, 60.0, "Refresh rate the title sees", Range{24.0, 240.0},
              kRestart);
  REX_SETTING(f64, cursor_timeout, 0.0, "Seconds before an idle cursor hides, 0 never hides",
              Range{0.0, 3600.0});
};

/**
 * Owns the window, the UI thread and frame loop, ImGui, the overlays, dialogs,
 * prompts and message boxes. With no driver, or a driver with no display, it is headless. The
 * frame loop runs, message boxes only log and prompts cancel at once.
 *
 * Window, ImGui, overlay and dialog calls belong on the UI thread. Calls marked "any thread" are
 * safe everywhere.
 */
REX_DECLARE_INTERFACE(IUISystem, 1)
/** Returns the player's UI settings. */
  virtual UISettings& GetSettings() = 0;

  /**
   * Reports whether there is no display. Any thread.
   *
   * @return True with no driver or a driver with no display.
   */
  virtual bool IsHeadless() const = 0;

  /** Reports whether the window is open. Any thread. */
  virtual bool HasWindow() const = 0;

  /**
   * Reports whether the window has focus. Any thread.
   *
   * @return True when it does, false when headless.
   */
  virtual bool IsFocused() const = 0;

  /** Returns the video mode built from ui.resolution and ui.video_mode_*. Any thread. */
  virtual VideoMode GetVideoMode() const = 0;

  /**
   * Returns the window's native handles, for a project that presents with its own renderer, or
   * kNotFound when the window is not open.
   */
  [[nodiscard]] virtual Result<NativeWindow> GetNativeWindow() const = 0;

  virtual void RaiseWindow() = 0;

  /** Reports whether the caller is on the UI thread. Any thread. */
  virtual bool IsUIThread() const = 0;

  /** Queues a function to run on the UI thread between frames. Any thread. */
  virtual void PostTask(MoveOnlyFunction<void()> fn) = 0;

  /**
   * Runs a function on the UI thread and waits for it, at once when already there. Any thread.
   *
   * @return False when the UI system stopped and the function did not run.
   */
  [[nodiscard]] virtual bool RunSynchronous(MoveOnlyFunction<void()> fn) = 0;

  /**
   * Adds a listener for window and device events.
   *
   * @param listener The listener, which must outlive the returned guard.
   * @return The guard. Destroying it removes the listener and waits for a delivery in progress.
   */
  [[nodiscard]] virtual ScopeGuard AddEventListener(IUIEventListener& listener) = 0;

  /** Returns the font atlas. Fonts added to it reach the renderer with the next frame. */
  virtual ImFontAtlas& GetFonts() = 0;

  /**
   * Returns ImGui's style.
   */
  virtual ImGuiStyle& GetImGuiStyle() = 0;

  /**
   * Returns ImGui's IO, such as whether ImGui wants the keyboard.
   */
  virtual ImGuiIO& GetImGuiIO() = 0;

  /**
   * Returns the SDK's dialog style.
   */
  virtual ui::Style& GetStyle() = 0;

  /**
   * Adds overlays and registers their hotkey settings.
   *
   * @return Ok, or an error when a name is already added.
   */
  [[nodiscard]] virtual Result<void> AddOverlays(std::span<const OverlayConfig> overlays) = 0;

  virtual void ShowOverlay(std::string_view name, bool visible) = 0;

  virtual void ToggleOverlay(std::string_view name) = 0;

  /**
   * @return The overlay, or kNotFound.
   */
  [[nodiscard]] virtual Result<ui::IDialog*> GetOverlay(std::string_view name) = 0;

  /** Reports whether any overlay is shown. Any thread. */
  virtual bool IsAnyOverlayVisible() const = 0;

  /**
   * Adds a dialog drawn with the overlays.
   *
   * @param dialog The dialog, which must outlive the guard.
   * @return A guard that removes the dialog when released, taking effect at once even inside the
   *         dialog's own Draw.
   */
  [[nodiscard]] virtual ScopeGuard AddDialog(ui::IDialog& dialog) = 0;

  /**
   * Adds a dialog the UI system owns and destroys when Draw returns false or the system stops.
   */
  virtual void AddDialog(std::unique_ptr<ui::IDialog> dialog) = 0;

  /** Reports whether any dialog or prompt is open. Any thread. */
  virtual bool HasDialogs() const = 0;

  /**
   * Builds one overlay frame on the UI thread and has the renderer draw it on the calling thread.
   * Called once per presented frame. Any thread.
   */
  virtual void DrawOverlays(IOverlayRenderer& renderer) = 0;

  /**
   * Logs a message and, unless headless, shows it in a message box until the player closes it. Any
   * thread.
   *
   * @param type Information, warning or error.
   */
  virtual void ShowMessage(MessageType type, std::string_view heading, std::string_view body) = 0;

  /**
   * Shows a message box with buttons and waits for a choice. Any thread.
   *
   * @param config The text and the buttons.
   * @return The index of the button chosen, or an error when headless or closed without a choice.
   */
  [[nodiscard]] virtual Result<u32> ShowChoice(const ui::ChoiceConfig& config) = 0;

  /**
   * Shows the host's open or save dialog and waits. Any thread.
   *
   * @param config The mode, filters and start folder.
   * @return The paths chosen, empty when canceled, or an error when headless.
   */
  [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const ui::FileDialogConfig& config) = 0;

  /**
   * Shows a prompt drawn in the window, such as a title's keyboard. Returns at once. Any thread.
   *
   * @param config The text, buttons and text input.
   * @param done Called once on the UI thread with the answer, canceled when there is no window or
   *        when CancelPrompts runs.
   */
  virtual void ShowPrompt(ui::PromptConfig config,
                          MoveOnlyFunction<void(ui::PromptResult)> done) = 0;

  /** Cancels every open prompt. Any thread. */
  virtual void CancelPrompts() = 0;

  /**
   * @return The displays, empty when headless.
   */
  virtual std::vector<ui::Monitor> GetDisplays() = 0;

  /**
   * Lists the host's preferred locales, most preferred first. Any thread.
   *
   * @return The locales, empty when headless.
   */
  virtual std::vector<ui::HostLocale> GetPreferredLocales() = 0;
};

}  // namespace rex::ui
