#pragma once
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


#include <rex/ui/ui_event.h>

namespace rex {
namespace ui {

// Virtual interfaces for types that want to listen for Window events.
// Use Window::Add[Input]Listener and Window::Remove[Input]Listener to manage
// active listeners.

class WindowListener {
 public:
  virtual ~WindowListener() = default;

  // OnOpened will be followed by various initial setup listeners.
  virtual void OnOpened(UISetupEvent&) {}
  virtual void OnClosing(UIEvent&) {}

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
}  // namespace xe

