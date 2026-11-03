#include <algorithm>
#include <chrono>

#include <rex/logging/log.h>
#include <rex/apu/audio_system.h>
#include <rex/hid/input_system.h>
#include <rex/title.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>
#include "ui/ui_system.h"

#include "ui/ui_state.h"

namespace rex::ui {

void UISystem::State::Dispatcher::OnUIEvent(const ui::UIEvent& event) {
  state.OnEvent(const_cast<ui::UIEvent&>(event));
}

void UISystem::State::Dispatcher::OnInputEvent(hid::InputEvent& event) {
  state.OnInputEvent(event);
}

bool UISystem::State::IsCaptureActive() const {
  if (!focused.load()) {
    return false;
  }
  if (!overlays || !overlays->IsAnyVisible()) {
    return true;
  }
  return !imgui->GetInputCapture().WantsMouse();
}

bool UISystem::State::IsOverlayCapturingInput(bool keyboard) const {
  if (!imgui || dialog_count.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  const ui::InputCapture& capture = imgui->GetInputCapture();
  return keyboard ? capture.WantsKeyboard() : capture.WantsMouse();
}

void UISystem::State::ApplyPointerMode(hid::PointerMode mode) {
  if (!driver || mode == applied_pointer_mode) {
    return;
  }
  driver->SetPointerMode(mode);
  applied_pointer_mode = mode;
  driver->SetCursorShape(mode == hid::PointerMode::kFree && !cursor_hidden_by_timeout,
                         rex::ui::CursorShape::kArrow);
  REXLOG_INFO(ui, "pointer mode applied");
}

void UISystem::State::RefreshCapture() {
  if (!input || !window_open.load()) {
    return;
  }
  input->SetInputCaptured(IsCaptureActive());
  ApplyPointerMode(input->GetPointerModeToApply());
}

void UISystem::State::UpdateCursorTimeout() {
  const f64 seconds = owner.GetSettings().cursor_timeout.Get();
  if (!window_open.load() || seconds <= 0.0 || cursor_hidden_by_timeout ||
      applied_pointer_mode != hid::PointerMode::kFree) {
    return;
  }
  const auto idle = std::chrono::steady_clock::now() - last_pointer_motion;
  if (idle >= std::chrono::duration<f64>(seconds)) {
    cursor_hidden_by_timeout = true;
    driver->SetCursorShape(false, rex::ui::CursorShape::kArrow);
  }
}

bool UISystem::State::IsHeldByOverlay(const hid::InputEvent& event) const {
  using Type = hid::InputEvent::Type;
  switch (event.type) {
    case Type::kKeyDown:
    case Type::kKeyChar:
      return IsOverlayCapturingInput(true);
    case Type::kButtonDown:
    case Type::kMotion:
    case Type::kWheel:
      return IsOverlayCapturingInput(false);
    default:
      return false;
  }
}

void UISystem::State::OnInputEvent(hid::InputEvent& event) {
  using Type = hid::InputEvent::Type;
  if (!IsHeldByOverlay(event)) {
    std::lock_guard delivery(delivery_mutex);
    std::vector<rex::ui::IUIEventListener*> snapshot;
    {
      std::lock_guard lock(listener_mutex);
      snapshot = listeners;
    }
    for (rex::ui::IUIEventListener* listener : snapshot) {
      listener->OnInputEvent(event);
    }
  }
  if (event.type == Type::kMotion || event.type == Type::kButtonDown) {
    last_pointer_motion = std::chrono::steady_clock::now();
    if (cursor_hidden_by_timeout) {
      cursor_hidden_by_timeout = false;
      if (applied_pointer_mode == hid::PointerMode::kFree) {
        driver->SetCursorShape(true, rex::ui::CursorShape::kArrow);
      }
    }
  }
  if (imgui) {
    imgui->HandleEvent(event);
  }
  if (event.type == Type::kKeyDown && !event.handled) {
    event.handled = binds.Process(event);
  }
  if (!input) {
    return;
  }
  switch (event.type) {
    case Type::kKeyDown:
      if (!event.handled && !IsOverlayCapturingInput(true)) {
        input->OnInputEvent(event);
      }
      RefreshCapture();
      break;
    case Type::kKeyUp:
      input->OnInputEvent(event);
      RefreshCapture();
      break;
    case Type::kKeyChar:
      if (!event.handled && !IsOverlayCapturingInput(true)) {
        input->OnInputEvent(event);
      }
      break;
    case Type::kButtonDown:
      if (event.button != hid::MouseButton::kNone && !IsOverlayCapturingInput(false)) {
        input->OnInputEvent(event);
      }
      RefreshCapture();
      break;
    case Type::kButtonUp:
      if (event.button != hid::MouseButton::kNone) {
        input->OnInputEvent(event);
      }
      RefreshCapture();
      break;
    case Type::kMotion:
      RefreshCapture();
      if (!IsOverlayCapturingInput(false)) {
        input->OnInputEvent(event);
      }
      break;
    case Type::kWheel:
      if (!IsOverlayCapturingInput(false)) {
        input->OnInputEvent(event);
      }
      break;
    default:
      break;
  }
}

void UISystem::State::OnEvent(ui::UIEvent& event) {
  {
    std::lock_guard delivery(delivery_mutex);
    std::vector<rex::ui::IUIEventListener*> snapshot;
    {
      std::lock_guard lock(listener_mutex);
      snapshot = listeners;
    }
    for (rex::ui::IUIEventListener* listener : snapshot) {
      listener->OnUIEvent(event);
    }
  }
  using Type = ui::UIEvent::Type;
  switch (event.GetType()) {
    case Type::kResize: {
      const ui::WindowSize size = driver->GetWindowSize();
      window_width = size.width_px;
      window_height = size.height_px;
      content_scale = driver->GetContentScale();
      if (resize_handler) {
        resize_handler(window_width, window_height);
      }
      if (input) {
        hid::InputEvent resized{
            .type = hid::InputEvent::Type::kResize, .width = window_width, .height = window_height};
        input->OnInputEvent(resized);
      }
      break;
    }
    case Type::kDPIChanged:
    case Type::kMonitorUpdate:
      content_scale = driver->GetContentScale();
      break;
    case Type::kFocus: {
      const bool gained = driver->HasFocus();
      focused = gained;
      if (audio) {
        audio->SetFocused(gained);
      }
      if (input) {
        hid::InputEvent focus{.type = gained ? hid::InputEvent::Type::kFocusGained
                                             : hid::InputEvent::Type::kFocusLost};
        input->OnInputEvent(focus);
      }
      RefreshCapture();
      break;
    }
    case Type::kClose:
      RequestClose();
      break;
    case Type::kTouch:
      if (imgui) {
        imgui->HandleEvent(event);
      }
      break;
    default:
      break;
  }
}

bool UISystem::RunFrame(u32 timeout_ms) {
  State& s = *state_;
  s.ExecutePosts();
  u32 wait = timeout_ms;
  const f64 cursor_seconds = settings_.cursor_timeout.Get();
  if (s.window_open.load() && cursor_seconds > 0.0 && !s.cursor_hidden_by_timeout) {
    wait = std::min<u32>(wait, 250);
  }
  bool alive = true;
  if (s.window_open.load()) {
    {
      std::lock_guard lock(s.post_mutex);
      if (!s.posts.empty()) {
        wait = 0;
      }
    }
    alive = s.driver->WaitEvents(s.dispatcher, wait);
  } else {
    std::unique_lock lock(s.post_mutex);
    s.post_wake.wait_for(lock, std::chrono::milliseconds(wait),
                         [&] { return !s.posts.empty() || s.quit.load(); });
  }
  s.ExecutePosts();
  s.UpdateCursorTimeout();
  if (!alive) {
    s.RequestClose();
  }
  return !s.quit.load();
}

void UISystem::SetResizeHandler(MoveOnlyFunction<void(u32, u32)> handler) {
  state_->resize_handler = std::move(handler);
}

void UISystem::SetCloseHandler(MoveOnlyFunction<void()> handler) {
  state_->close_handler = std::move(handler);
}

void UISystem::State::RequestClose() {
  if (close_handler) {
    close_handler();
  }
}

void UISystem::EndFrameLoop() {
  state_->quit = true;
  state_->WakeLoop();
}

Result<void> UISystem::AttachTitle(const TitleInfo& info, IAchievementRegistry& achievements) {
  State& s = *state_;
  const bool ran = RunSynchronous([&s, &info, &achievements] {
    s.title_info = info;
    s.achievements = &achievements;
    s.CreateToast();
  });
  if (!ran) {
    return Err(ErrorCategory::kRuntime, "the UI system stopped");
  }
  return {};
}

void UISystem::DetachTitle() {
  State& s = *state_;
  (void)RunSynchronous([&s] {
    s.ReleaseToast();
    s.achievements = nullptr;
  });
}

void UISystem::State::CreateToast() {
  if (!achievements || !imgui || !overlay_context) {
    return;
  }
  std::lock_guard lock(toast_mutex);
  if (toast) {
    return;
  }
  toast = std::make_unique<ui::AchievementToastDialog>(*overlay_context);
  imgui->AddDialog(*toast);
  toast_listener = achievements->OnAchievementUnlocked([this](const Achievement& achievement) {
    std::lock_guard callback_lock(toast_mutex);
    if (toast) {
      toast->Push(achievement);
    }
  });
}

void UISystem::State::ReleaseToast() {
  toast_listener = {};
  std::lock_guard lock(toast_mutex);
  if (toast && imgui) {
    imgui->RemoveDialog(*toast);
  }
  toast.reset();
}

}  // namespace rex::ui
