#include "ui/sdl3/hid_host.h"

#include <rex/logging.h>
#include <rex/move_only_function.h>

namespace rex::ui {

HIDHost::HIDHost(IUserInterface& ui, Window& window, ImGuiDrawer& drawer, OverlayRegistry& overlays,
                 hid::IHIDBackend& backend)
    : ui_(ui), window_(window), drawer_(drawer), overlays_(overlays), backend_(backend) {}

void HIDHost::Attach() {
  backend_.SetHost(this);
  backend_.OnEvent({.kind = hid::InputEvent::Kind::kResize,
                    .width = window_.GetActualPhysicalWidth(),
                    .height = window_.GetActualPhysicalHeight()});
  backend_.OnEvent({.kind = window_.HasFocus() ? hid::InputEvent::Kind::kFocusGained
                                               : hid::InputEvent::Kind::kFocusLost});
}

void HIDHost::Detach() {
  backend_.SetHost(nullptr);
}

bool HIDHost::Post(rex::MoveOnlyFunction<void()> work) {
  return ui_.Post(std::move(work));
}

bool HIDHost::ApplyPointerMode(hid::PointerMode mode) {
  if (applied_ == hid::PointerMode::kLocked) {
    window_.SetRelativeMouseMode(false);
    window_.ReleaseMouse();
  }
  if (applied_ == hid::PointerMode::kFree) {
    arrow_ = window_.GetCursorVisibility();
  }
  window_.SetCursorVisibility(mode == hid::PointerMode::kFree ? arrow_ : CursorVisibility::kHidden);
  bool relative = false;
  if (mode == hid::PointerMode::kLocked) {
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

bool HIDHost::GetCaptureActive() const {
  if (!window_.HasFocus()) {
    return false;
  }
  if (!overlays_.IsAnyVisible()) {
    return true;
  }
  return !drawer_.GetIO().WantCaptureMouse;
}

bool HIDHost::IsOverlayCapturingInput(bool keyboard) const {
  if (!overlays_.IsAnyVisible()) {
    return false;
  }
  const ImGuiIO& io = drawer_.GetIO();
  return keyboard ? io.WantCaptureKeyboard : io.WantCaptureMouse;
}

void HIDHost::OnKeyDown(KeyEvent& e) {
  if (!IsOverlayCapturingInput(true)) {
    Key(hid::InputEvent::Kind::kKeyDown, e);
  }
}

void HIDHost::OnKeyUp(KeyEvent& e) {
  Key(hid::InputEvent::Kind::kKeyUp, e);
}

void HIDHost::OnKeyChar(KeyEvent& e) {
  if (IsOverlayCapturingInput(true)) {
    return;
  }
  backend_.OnEvent(
      {.kind = hid::InputEvent::Kind::kKeyChar, .character = static_cast<u32>(e.GetVirtualKey())});
}

void HIDHost::OnMouseDown(MouseEvent& e) {
  if (!IsOverlayCapturingInput(false)) {
    Button(hid::InputEvent::Kind::kButtonDown, e);
  }
}

void HIDHost::OnMouseUp(MouseEvent& e) {
  Button(hid::InputEvent::Kind::kButtonUp, e);
}

void HIDHost::OnMouseMove(MouseEvent& e) {
  if (IsOverlayCapturingInput(false)) {
    return;
  }
  backend_.OnEvent({.kind = hid::InputEvent::Kind::kMotion,
                    .x = static_cast<f32>(e.GetX()),
                    .y = static_cast<f32>(e.GetY()),
                    .dx = e.GetDx(),
                    .dy = e.GetDy()});
}

void HIDHost::OnMouseWheel(MouseEvent& e) {
  const i32 detents = e.GetScrollY() / static_cast<i32>(MouseEvent::kScrollPerDetent);
  if (detents != 0 && !IsOverlayCapturingInput(false)) {
    backend_.OnEvent({.kind = hid::InputEvent::Kind::kWheel, .wheel = detents});
  }
}

void HIDHost::OnGotFocus(UISetupEvent& e) {
  (void)e;
  backend_.OnEvent({.kind = hid::InputEvent::Kind::kFocusGained});
}

void HIDHost::OnLostFocus(UISetupEvent& e) {
  (void)e;
  backend_.OnEvent({.kind = hid::InputEvent::Kind::kFocusLost});
}

void HIDHost::OnResize(UISetupEvent& e) {
  (void)e;
  backend_.OnEvent({.kind = hid::InputEvent::Kind::kResize,
                    .width = window_.GetActualPhysicalWidth(),
                    .height = window_.GetActualPhysicalHeight()});
}

void HIDHost::Key(hid::InputEvent::Kind kind, const KeyEvent& e) {
  hid::KeyModifiers modifiers = hid::KeyModifiers::kNone;
  if (e.IsShiftPressed()) {
    modifiers |= hid::KeyModifiers::kShift;
  }
  if (e.IsCtrlPressed()) {
    modifiers |= hid::KeyModifiers::kCtrl;
  }
  if (e.IsAltPressed()) {
    modifiers |= hid::KeyModifiers::kAlt;
  }
  backend_.OnEvent({.kind = kind,
                    .key = e.GetVirtualKey(),
                    .modifiers = modifiers,
                    .repeat = kind == hid::InputEvent::Kind::kKeyDown && e.GetPrevState()});
}

void HIDHost::Button(hid::InputEvent::Kind kind, const MouseEvent& e) {
  hid::MouseButton button = hid::MouseButton::kLeft;
  switch (e.GetButton()) {
    case MouseEvent::Button::kLeft:
      button = hid::MouseButton::kLeft;
      break;
    case MouseEvent::Button::kRight:
      button = hid::MouseButton::kRight;
      break;
    case MouseEvent::Button::kMiddle:
      button = hid::MouseButton::kMiddle;
      break;
    case MouseEvent::Button::kX1:
      button = hid::MouseButton::kX1;
      break;
    case MouseEvent::Button::kX2:
      button = hid::MouseButton::kX2;
      break;
    case MouseEvent::Button::kNone:
      return;
  }
  backend_.OnEvent({.kind = kind,
                    .button = button,
                    .x = static_cast<f32>(e.GetX()),
                    .y = static_cast<f32>(e.GetY())});
}

}  // namespace rex::ui
