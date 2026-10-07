#pragma once

#include <functional>
#include <optional>

#include <rex/hid.h>
#include <rex/ui/user_interface.h>

#include "ui/sdl3/imgui_drawer.h"
#include "ui/sdl3/overlay_registry.h"
#include "ui/sdl3/window.h"
#include "ui/sdl3/window_listener.h"

namespace rex::ui {

class HIDHost final : public hid::IInputHost, public WindowInputListener, public WindowListener {
 public:
  HIDHost(IUserInterface& ui, Window& window, ImGuiDrawer& drawer, OverlayRegistry& overlays,
          hid::IBackend& backend);

  void Attach();
  void Detach();

  bool Post(std::move_only_function<void()> work) override;
  bool ApplyPointerMode(hid::PointerMode mode) override;
  std::optional<hid::PointerState> WarpPointerToCenter() override;
  bool capture_active() const override;

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
  bool OverlayWants(bool keyboard) const;
  void Key(hid::InputEvent::Kind kind, const KeyEvent& e);
  void Button(hid::InputEvent::Kind kind, const MouseEvent& e);

  IUserInterface& ui_;
  Window& window_;
  ImGuiDrawer& drawer_;
  OverlayRegistry& overlays_;
  hid::IBackend& backend_;
  hid::PointerMode applied_ = hid::PointerMode::Free;
  CursorVisibility arrow_ = CursorVisibility::Visible;
};

}  // namespace rex::ui
