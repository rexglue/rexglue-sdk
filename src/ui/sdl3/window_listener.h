/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/ui/ui_event.h>

namespace rex {
namespace ui {

/** Listener for Window events. Manage with Window::Add[Input]Listener and Remove[Input]Listener. */

class WindowListener {
 public:
  virtual ~WindowListener() = default;

  /**
   * Called when the window opens, followed by the initial setup events.
   *
   * @param event Setup state of the opened window.
   */
  virtual void OnOpened(UISetupEvent&) {}
  virtual void OnClosing(UIEvent&) {}

  /**
   * Called when the user asks to close the window (close button, Alt+F4). Returning false vetoes
   * the close. The window stays open and the vetoing party closes it later with
   * Window::RequestClose, once renderers are drained and guest threads stopped.
   *
   * @param event Close request event.
   * @return True to accept the close, the default.
   */
  virtual bool OnCloseRequested(UIEvent&) { return true; }

  virtual void OnMinimized(UIEvent&) {}
  virtual void OnRestored(UIEvent&) {}

  virtual void OnDpiChanged(UISetupEvent&) {}
  virtual void OnResize(UISetupEvent&) {}

  virtual void OnGotFocus(UISetupEvent&) {}
  virtual void OnLostFocus(UISetupEvent&) {}

  virtual void OnFileDrop(FileDropEvent&) {}
};

class WindowInputListener {
 public:
  virtual ~WindowInputListener() = default;

  virtual void OnKeyDown(KeyEvent&) {}
  virtual void OnKeyUp(KeyEvent&) {}
  virtual void OnKeyChar(KeyEvent&) {}

  virtual void OnMouseDown(MouseEvent&) {}
  virtual void OnMouseMove(MouseEvent&) {}
  virtual void OnMouseUp(MouseEvent&) {}
  virtual void OnMouseWheel(MouseEvent&) {}

  virtual void OnTouchEvent(TouchEvent&) {}
};

}  // namespace ui
}  // namespace rex
