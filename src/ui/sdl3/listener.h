#pragma once

#include <rex/ui/user_interface.h>

#include "ui/sdl3/bind_table.h"
#include "ui/sdl3/window_listener.h"

namespace rex::ui {

class EventListener final : public WindowListener, public WindowInputListener {
 public:
  explicit EventListener(EventCallback& callback) : callback_(callback) {}

  bool OnCloseRequested(UIEvent& e) override {
    Notify(e);
    return false;
  }
  void OnClosing(UIEvent& e) override { Notify(e); }
  void OnResize(UISetupEvent& e) override { Notify(e); }
  void OnGotFocus(UISetupEvent& e) override { Notify(e); }
  void OnLostFocus(UISetupEvent& e) override { Notify(e); }
  void OnDpiChanged(UISetupEvent& e) override { Notify(e); }
  void OnMinimized(UIEvent& e) override { Notify(e); }
  void OnRestored(UIEvent& e) override { Notify(e); }
  void OnFileDrop(FileDropEvent& e) override { Notify(e); }
  void OnKeyDown(KeyEvent& e) override { Forward(e); }
  void OnKeyUp(KeyEvent& e) override { Forward(e); }
  void OnKeyChar(KeyEvent& e) override { Forward(e); }
  void OnMouseDown(MouseEvent& e) override { Forward(e); }
  void OnMouseUp(MouseEvent& e) override { Forward(e); }
  void OnMouseMove(MouseEvent& e) override { Forward(e); }
  void OnMouseWheel(MouseEvent& e) override { Forward(e); }

 private:
  bool Notify(const UIEvent& e) { return callback_ && callback_(e); }

  template <typename T>
  void Forward(T& e) {
    if (Notify(e)) {
      e.set_handled(true);
    }
  }

  EventCallback& callback_;
};

class BindListener final : public WindowInputListener {
 public:
  explicit BindListener(BindTable& binds) : binds_(binds) {}

  void OnKeyDown(KeyEvent& e) override {
    if (binds_.Process(e)) {
      e.set_handled(true);
    }
  }

 private:
  BindTable& binds_;
};

}  // namespace rex::ui
