/**
 * @file        ui/sdl3/hid_host.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>

#include <rex/hid.h>
#include <rex/ui/user_interface.h>
#include <rex/move_only_function.h>

#include "ui/sdl3/imgui_drawer.h"
#include "ui/sdl3/overlay_registry.h"
#include "ui/sdl3/window.h"
#include "ui/sdl3/window_listener.h"

namespace rex::ui {

class HIDHost final : public hid::IInputHost, public WindowInputListener, public WindowListener {
 public:
  HIDHost(IUserInterface& ui, Window& window, ImGuiDrawer& drawer, OverlayRegistry& overlays,
          hid::IHIDBackend& backend);

  void Attach();
  void Detach();

  bool Post(rex::MoveOnlyFunction<void()> work) override;
  bool ApplyPointerMode(hid::PointerMode mode) override;
  std::optional<hid::PointerState> WarpPointerToCenter() override;
  bool IsCaptureActive() const override;

  void OnKeyDown(KeyEvent& e) override;
  void OnKeyUp(KeyEvent& e) override;
  void OnKeyChar(KeyEvent& e) override;
  void OnMouseDown(MouseEvent& e) override;
  void OnMouseUp(MouseEvent& e) override;
  void OnMouseMove(MouseEvent& e) override;
  void OnMouseWheel(MouseEvent& e) override;
  void OnGotFocus(UISetupEvent& e) override;
  void OnLostFocus(UISetupEvent& e) override;
  void OnResize(UISetupEvent& e) override;

 private:
  bool IsOverlayCapturingInput(bool keyboard) const;
  void ForwardKey(hid::InputEvent::Kind kind, const KeyEvent& e);
  void ForwardButton(hid::InputEvent::Kind kind, const MouseEvent& e);

  IUserInterface& ui_;
  Window& window_;
  ImGuiDrawer& drawer_;
  OverlayRegistry& overlays_;
  hid::IHIDBackend& backend_;
  hid::PointerMode applied_pointer_mode_ = hid::PointerMode::kFree;
  CursorVisibility saved_cursor_visibility_ = CursorVisibility::kVisible;
};

}  // namespace rex::ui
