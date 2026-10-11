#include "ui/sdl3/hid_host.h"

#include <rex/logging.h>
#include <rex/move_only_function.h>

namespace rex::ui {

HIDHost::HIDHost(IUserInterface& ui, Window& window, ImGuiDrawer& drawer, OverlayRegistry& overlays,
                 hid::IHIDBackend& backend)
    : ui_(ui), window_(window), drawer_(drawer), overlays_(overlays), backend_(backend) {}

void HIDHost::Attach() {
  backend_.SetHost(this);
  backend_.OnEvent({.kind = hid::InputEvent::Kind::Resize,
                    .width = window_.GetActualPhysicalWidth(),
                    .height = window_.GetActualPhysicalHeight()});
  backend_.OnEvent({.kind = window_.HasFocus() ? hid::InputEvent::Kind::FocusGained
                                               : hid::InputEvent::Kind::FocusLost});
}

void HIDHost::Detach() {
  backend_.SetHost(nullptr);
}

bool HIDHost::Post(rex::MoveOnlyFunction<void()> work) {
  return ui_.Post(std::move(work));
}

bool HIDHost::ApplyPointerMode(hid::PointerMode mode) {
  if (applied_ == hid::PointerMode::Locked) {
    window_.SetRelativeMouseMode(false);
    window_.ReleaseMouse();
  }
  if (applied_ == hid::PointerMode::Free) {
    arrow_ = window_.GetCursorVisibility();
  }
  window_.SetCursorVisibility(mode == hid::PointerMode::Free ? arrow_ : CursorVisibility::Hidden);
  bool relative = false;
  if (mode == hid::PointerMode::Locked) {
    window_.CaptureMouse();
    relative = window_.SetRelativeMouseMode(true);
    if (!relative) {
      REXLOG_WARN(hid, "pointer lock is unavailable, mouse look recenters the pointer instead");
    }
  }
  applied_ = mode;
  return relative;
}

std::optional<hid::PointerState> HIDHost::WarpPointerToCenter() {
  i32 x = 0;
  i32 y = 0;
  if (!window_.WarpMouseToCenter(x, y)) {
    return std::nullopt;
  }
  return hid::PointerState{.x = static_cast<f32>(x),
                           .y = static_cast<f32>(y),
                           .window_width = window_.GetActualPhysicalWidth(),
                           .window_height = window_.GetActualPhysicalHeight()};
}

bool HIDHost::capture_active() const {
  if (!window_.HasFocus()) {
    return false;
  }
  if (!overlays_.AnyVisible()) {
    return true;
  }
  return !drawer_.GetIO().WantCaptureMouse;
}

bool HIDHost::OverlayWants(bool keyboard) const {
  if (!overlays_.AnyVisible()) {
    return false;
  }
  const ImGuiIO& io = drawer_.GetIO();
  return keyboard ? io.WantCaptureKeyboard : io.WantCaptureMouse;
}

void HIDHost::OnKeyDown(KeyEvent& e) {
  if (!OverlayWants(true)) {
    Key(hid::InputEvent::Kind::KeyDown, e);
  }
}

void HIDHost::OnKeyUp(KeyEvent& e) {
  Key(hid::InputEvent::Kind::KeyUp, e);
}

void HIDHost::OnKeyChar(KeyEvent& e) {
  if (OverlayWants(true)) {
    return;
  }
  backend_.OnEvent(
      {.kind = hid::InputEvent::Kind::KeyChar, .character = static_cast<u32>(e.virtual_key())});
}

void HIDHost::OnMouseDown(MouseEvent& e) {
  if (!OverlayWants(false)) {
    Button(hid::InputEvent::Kind::ButtonDown, e);
  }
}

void HIDHost::OnMouseUp(MouseEvent& e) {
  Button(hid::InputEvent::Kind::ButtonUp, e);
}

void HIDHost::OnMouseMove(MouseEvent& e) {
  if (OverlayWants(false)) {
    return;
  }
  backend_.OnEvent({.kind = hid::InputEvent::Kind::Motion,
                    .x = static_cast<f32>(e.x()),
                    .y = static_cast<f32>(e.y()),
                    .dx = e.dx(),
                    .dy = e.dy()});
}

void HIDHost::OnMouseWheel(MouseEvent& e) {
  const i32 detents = e.scroll_y() / static_cast<i32>(MouseEvent::kScrollPerDetent);
  if (detents != 0 && !OverlayWants(false)) {
    backend_.OnEvent({.kind = hid::InputEvent::Kind::Wheel, .wheel = detents});
  }
}

void HIDHost::OnGotFocus(UISetupEvent& e) {
  (void)e;
  backend_.OnEvent({.kind = hid::InputEvent::Kind::FocusGained});
}

void HIDHost::OnLostFocus(UISetupEvent& e) {
  (void)e;
  backend_.OnEvent({.kind = hid::InputEvent::Kind::FocusLost});
}

void HIDHost::OnResize(UISetupEvent& e) {
  (void)e;
  backend_.OnEvent({.kind = hid::InputEvent::Kind::Resize,
                    .width = window_.GetActualPhysicalWidth(),
                    .height = window_.GetActualPhysicalHeight()});
}

void HIDHost::Key(hid::InputEvent::Kind kind, const KeyEvent& e) {
  hid::KeyModifiers modifiers = hid::KeyModifiers::None;
  if (e.is_shift_pressed()) {
    modifiers |= hid::KeyModifiers::Shift;
  }
  if (e.is_ctrl_pressed()) {
    modifiers |= hid::KeyModifiers::Ctrl;
  }
  if (e.is_alt_pressed()) {
    modifiers |= hid::KeyModifiers::Alt;
  }
  backend_.OnEvent({.kind = kind,
                    .key = e.virtual_key(),
                    .modifiers = modifiers,
                    .repeat = kind == hid::InputEvent::Kind::KeyDown && e.prev_state()});
}

void HIDHost::Button(hid::InputEvent::Kind kind, const MouseEvent& e) {
  hid::MouseButton button = hid::MouseButton::Left;
  switch (e.button()) {
    case MouseEvent::Button::kLeft:
      button = hid::MouseButton::Left;
      break;
    case MouseEvent::Button::kRight:
      button = hid::MouseButton::Right;
      break;
    case MouseEvent::Button::kMiddle:
      button = hid::MouseButton::Middle;
      break;
    case MouseEvent::Button::kX1:
      button = hid::MouseButton::X1;
      break;
    case MouseEvent::Button::kX2:
      button = hid::MouseButton::X2;
      break;
    case MouseEvent::Button::kNone:
      return;
  }
  backend_.OnEvent(
      {.kind = kind, .button = button, .x = static_cast<f32>(e.x()), .y = static_cast<f32>(e.y())});
}

}  // namespace rex::ui
