/**
 * @file        ui/ui_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/driver.h>
#include <rex/hid/input_keys.h>
#include <rex/memory/host.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/platform/process.h>
#include <rex/ui/dialog.h>
#include <rex/platform/native_window.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

namespace rex::ui {

/** The memory tag of windowing and the UI, named "UI". */
inline constexpr memory::Tag kUI{5};

/**
 * A windowing library with one window, the event pump, native dialogs, displays, locales and the
 * clipboard. UISystem makes every call on the UI thread except WakeUp. A driver owns only its own
 * ui.<name>.* settings and never draws.
 */
class IUIDriver : public IDriver {
 public:
  /**
   * Reports whether a display exists.
   *
   * @return False on a dummy video driver, where UISystem is headless.
   */
  virtual bool HasDisplay() const = 0;

  /**
   * @param config The window title, size and fullscreen state.
   * @return Ok, or an error, and UISystem then runs headless.
   */
  [[nodiscard]] virtual Result<void> OpenWindow(const WindowConfig& config) = 0;

  virtual void CloseUIWindow() = 0;

  virtual void SetTitle(std::string_view title) = 0;

  /**
   * Sets the client size of a window not in fullscreen.
   */
  virtual void SetSize(u32 width, u32 height) = 0;

  /** Sets the smallest client size and the shape kept on resize. */
  virtual void SetLimits(u32 min_width, u32 min_height, ui::AspectRatio ratio) = 0;

  /**
   * Enters or leaves fullscreen on a display.
   *
   * @param exclusive True for exclusive fullscreen, false for borderless.
   * @param mode_width Exclusive mode width, 0 for the desktop mode.
   * @param mode_height Exclusive mode height, 0 for the desktop mode.
   */
  virtual void SetFullscreen(bool fullscreen, bool exclusive, i32 monitor, u32 mode_width,
                             u32 mode_height) = 0;

  virtual void RaiseWindow() = 0;

  /**
   * Shows or hides the cursor and sets its shape.
   *
   * @param shape The shape while shown.
   */
  virtual void SetCursorShape(bool visible, CursorShape shape) = 0;

  /**
   * Sets the pointer mode. When the host refuses relative motion the driver recenters the pointer
   * and reports dx and dy.
   */
  virtual void SetPointerMode(hid::PointerMode mode) = 0;

  /** Captures the mouse so a drag keeps reporting outside the window, or releases it. */
  virtual void CaptureMouse(bool captured) = 0;

  /** Returns the ratio of window pixels to layout size, 1 when the host has none. */
  virtual f32 GetContentScale() const = 0;

  /** Returns the window size in pixels, zero when the window is not open. */
  virtual ui::WindowSize GetWindowSize() const = 0;

  virtual bool HasFocus() const = 0;

  /** Starts or stops text input, which turns on an on screen keyboard where the host has one. */
  virtual void SetTextInputActive(bool active) = 0;

  /**
   * Returns the window's native handles, which GraphicsSystem presents to, or kNotFound when the
   * window is not open.
   */
  [[nodiscard]] virtual Result<NativeWindow> GetNativeWindow() const = 0;

  /**
   * Waits for events and delivers each one, in order.
   *
   * @param timeout_ms The longest wait, 0 delivers what is waiting and returns.
   * @return False once the host asked the app to quit.
   */
  virtual bool WaitEvents(IUIEventListener& listener, u32 timeout_ms) = 0;

  /** Ends a WaitEvents in progress. Any thread. */
  virtual void WakeUp() = 0;

  /**
   * Shows a native message box and waits.
   *
   * @param type Information, warning or error.
   * @param buttons The button labels, one OK button when empty.
   * @return The index of the button chosen, or an error when it could not show.
   */
  [[nodiscard]] virtual Result<u32> ShowMessageBox(MessageType type, std::string_view title,
                                                   std::string_view body,
                                                   std::span<const std::string> buttons) = 0;

  /**
   * Shows the native open or save dialog and waits.
   *
   * @param config The mode, filters and start folder.
   * @return The paths chosen, empty when canceled.
   */
  [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const ui::FileDialogConfig& config) = 0;

  virtual std::vector<ui::Monitor> GetDisplays() = 0;

  /**
   * Lists the host's preferred locales, most preferred first.
   */
  virtual std::vector<ui::HostLocale> GetPreferredLocales() = 0;

  /**
   * @return The text, empty when none.
   */
  virtual std::string GetClipboardText() = 0;

  /**
   * Sets the clipboard text.
   */
  virtual void SetClipboardText(std::string_view text) = 0;
};

}  // namespace rex::ui
