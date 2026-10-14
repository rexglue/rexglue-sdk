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

#include <algorithm>
#include <iterator>

#include <rex/assert.h>
#include <rex/cvar.h>
#include "ui/sdl3/video_mode.h"
#include <rex/logging.h>
#include "ui/sdl3/imgui_drawer.h"
#include "ui/sdl3/window.h"

#include "ui/sdl3/cvar_table.h"

#include <imgui.h>

namespace rex {
namespace ui {

Window::Window(WindowedAppContext& app_context, const std::string_view title,
               u32 desired_logical_width, u32 desired_logical_height)
    : app_context_(app_context),
      desired_logical_width_(desired_logical_width),
      desired_logical_height_(desired_logical_height),
      title_(title) {}

Window::~Window() {
  REX_ASSERT(phase_ == Phase::kDeleting);
  EnterDestructor();

  if (presenter_) {
    gpu::IPresenter* old_presenter = presenter_;
    // Nulled first so Attach and SetPresenter do not call each other forever.
    presenter_ = nullptr;
    old_presenter->Attach(nullptr, nullptr);
    presenter_surface_.reset();
  }

  while (innermost_destruction_receiver_) {
    innermost_destruction_receiver_->window_ = nullptr;
    innermost_destruction_receiver_ = innermost_destruction_receiver_->outer_receiver_;
  }
}

void Window::AddListener(WindowListener* listener) {
  REX_ASSERT(listener != nullptr);
  if (std::find(listeners_.cbegin(), listeners_.cend(), listener) != listeners_.cend()) {
    return;
  }
  listeners_.push_back(listener);
}

void Window::RemoveListener(WindowListener* listener) {
  REX_ASSERT(listener != nullptr);
  auto it = std::find(listeners_.cbegin(), listeners_.cend(), listener);
  if (it == listeners_.cend()) {
    return;
  }
  ListenerIterationContext* iteration_context = innermost_listener_iteration_context_;
  if (iteration_context) {
    size_t existing_index = static_cast<size_t>(std::distance(listeners_.cbegin(), it));
    while (iteration_context) {
      if (iteration_context->next_index > existing_index) {
        --iteration_context->next_index;
      }
      iteration_context = iteration_context->outer_context;
    }
  }
  listeners_.erase(it);
}

void Window::AddInputListener(WindowInputListener* listener, size_t z_order) {
  REX_ASSERT(listener != nullptr);
  input_listeners_.Add(listener, z_order);
}

void Window::RemoveInputListener(WindowInputListener* listener) {
  REX_ASSERT(listener != nullptr);
  input_listeners_.Remove(listener);
}

bool Window::Open() {
  if (phase_ != Phase::kClosedOpenable) {
    return true;
  }

  // Reset in Open, the only open entry point, rather than on every close path.
  actual_physical_width_ = 0;
  actual_physical_height_ = 0;
  has_focus_ = false;
  phase_ = Phase::kOpening;
  bool platform_open_result = OpenImpl();
  if (!platform_open_result) {
    phase_ = Phase::kClosedOpenable;
    return false;
  }
  if (phase_ != Phase::kOpening) {
    return true;
  }
  phase_ = Phase::kOpen;

  // Listeners get the state the platform applied, which can differ from the desired state.
  {
    MonitorUpdateEvent e(this, true, true);
    OnMonitorUpdate(e);
  }
  {
    UISetupEvent opened(UIEvent::Type::kOpened, this, true);
    UISetupEvent dpi(UIEvent::Type::kDPIChanged, this, true);
    ResizeEvent resize(this, {GetActualPhysicalWidth(), GetActualPhysicalHeight()}, true);
    FocusEvent focus(this, true, true);
    WindowDestructionReceiver destruction_receiver(this);
    SendEventToListeners([&opened](auto listener) { listener->OnOpened(opened); },
                         destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrListenersUncallable()) {
      return true;
    }
    SendEventToListeners([&dpi](auto listener) { listener->OnDPIChanged(dpi); },
                         destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrListenersUncallable()) {
      return true;
    }
    SendEventToListeners([&resize](auto listener) { listener->OnResize(resize); },
                         destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrListenersUncallable()) {
      return true;
    }
    if (HasFocus()) {
      SendEventToListeners([&focus](auto listener) { listener->OnGotFocus(focus); },
                           destruction_receiver);
      if (destruction_receiver.IsWindowDestroyedOrListenersUncallable()) {
        return true;
      }
    }
  }

  // After OnDPIChanged and OnResize so painting code sees the right size.
  OnSurfaceChanged(true);

  return true;
}

void Window::SetFullscreen(bool new_fullscreen) {
  if (fullscreen_ == new_fullscreen) {
    return;
  }
  fullscreen_ = new_fullscreen;
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewFullscreen();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::RefreshFullscreen() {
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewFullscreen();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::SetMonitor(i32 new_monitor) {
  if (monitor_ == new_monitor) {
    return;
  }
  monitor_ = new_monitor;
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewMonitor();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::SetDesiredLogicalSize(u32 new_desired_logical_width, u32 new_desired_logical_height) {
  if (desired_logical_width_ == new_desired_logical_width &&
      desired_logical_height_ == new_desired_logical_height) {
    return;
  }
  desired_logical_width_ = new_desired_logical_width;
  desired_logical_height_ = new_desired_logical_height;
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewDesiredLogicalSize();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::ResolveConfiguredLogicalSize(u32& width_out, u32& height_out) {
  i32 configured_width = REXCVAR_GET(::rex::ui::cvars::window_width);
  i32 configured_height = REXCVAR_GET(::rex::ui::cvars::window_height);
  if (configured_width <= 0 || configured_height <= 0) {
    detail::ResolveConfiguredSize(configured_width, configured_height);
  }
  width_out = static_cast<u32>(std::clamp(configured_width, 1, 8192));
  height_out = static_cast<u32>(std::clamp(configured_height, 1, 8192));
}

void Window::SetTitle(const std::string_view new_title) {
  if (title_ == new_title) {
    return;
  }
  title_ = new_title;
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewTitle();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::CaptureMouse() {
  ++mouse_capture_request_count_;
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  // Applied even when assumed captured, since the OS may have released the capture.
  ApplyNewMouseCapture();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::ReleaseMouse() {
  REX_ASSERT(mouse_capture_request_count_ != 0);
  if (!mouse_capture_request_count_) {
    return;
  }
  if (--mouse_capture_request_count_) {
    return;
  }
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewMouseRelease();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::SetTextInputActive(bool active) {
  if (text_input_active_ == active) {
    return;
  }
  text_input_active_ = active;
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewTextInputActive();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::SetCursorVisibility(CursorVisibility new_cursor_visibility) {
  if (cursor_visibility_ == new_cursor_visibility) {
    return;
  }
  CursorVisibility old_cursor_visibility = cursor_visibility_;
  cursor_visibility_ = new_cursor_visibility;
  if (!CanApplyState()) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  ApplyNewCursorVisibility(old_cursor_visibility);
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::Focus() {
  if (!CanApplyState() || has_focus_) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  FocusImpl();
  if (destruction_receiver.IsWindowDestroyedOrStateInapplicable()) {
    return;
  }
}

void Window::OnPresenterDetached() {
  presenter_ = nullptr;
  presenter_surface_.reset();
}

void Window::SetPresenter(gpu::IPresenter* presenter) {
  if (presenter_ == presenter) {
    return;
  }
  if (presenter_) {
    presenter_->Attach(nullptr, nullptr);
    presenter_surface_.reset();
  }
  presenter_ = presenter;
  if (presenter_) {
    presenter_surface_ = CreateSurface(presenter_->GetSupportedSurfaceTypes());
    presenter_->Attach(this, presenter_surface_.get());
  }
}

void Window::OnSurfaceChanged(bool new_surface_potentially_exists) {
  if (!presenter_) {
    return;
  }

  if (presenter_surface_) {
    presenter_->Attach(this, nullptr);
    presenter_surface_.reset();
  }

  if (!new_surface_potentially_exists) {
    return;
  }

  gpu::Surface::TypeFlags supported_types = presenter_->GetSupportedSurfaceTypes();
  presenter_surface_ = CreateSurface(supported_types);
  if (presenter_surface_) {
    presenter_->Attach(this, presenter_surface_.get());
  } else if (phase_ == Phase::kOpen) {
    REXLOG_ERROR(
        ui,
        "No presentable surface for this window. The graphics provider supports surface types "
        "{:#x}. Set the video_driver cvar to force a different session type.",
        supported_types);
  }
}

void Window::OnBeforeClose(WindowDestructionReceiver& destruction_receiver) {
  // Must run before the platform close, closed windows send no events and the surface detaches.

  bool was_open = phase_ == Phase::kOpen;
  bool was_open_or_opening = was_open || phase_ == Phase::kOpening;
  REX_ASSERT(was_open_or_opening);
  if (!was_open_or_opening) {
    return;
  }

  if (was_open) {
    phase_ = Phase::kOpenBeforeClosing;

    // Sent before OnClosing so listeners see the focus loss on close.
    OnFocusUpdate(false, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }

    {
      UIEvent e(UIEvent::Type::kClosing, this);
      SendEventToListeners([&e](auto listener) { listener->OnClosing(e); }, destruction_receiver);
      if (destruction_receiver.IsWindowDestroyed()) {
        return;
      }
    }

    // After the listeners so they do not reconnect.
    OnSurfaceChanged(false);
  }

  phase_ = Phase::kClosing;
}

void Window::OnAfterClose() {
  REX_ASSERT(phase_ == Phase::kClosing);
  if (phase_ != Phase::kClosing) {
    return;
  }
  phase_ = (innermost_listener_iteration_context_ || input_listeners_.GetPropagating())
               ? Phase::kClosedLeavingListeners
               : Phase::kClosedOpenable;
}

void Window::OnDPIChanged(UISetupEvent& e, WindowDestructionReceiver& destruction_receiver) {
  SendEventToListeners([&e](auto listener) { listener->OnDPIChanged(e); }, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnMonitorUpdate(MonitorUpdateEvent& e) {
  if (presenter_surface_) {
    presenter_->OnSurfaceMonitorUpdate(e.GetOldMonitorPotentiallyDisconnected());
  }
}

bool Window::OnActualSizeUpdate(u32 new_physical_width, u32 new_physical_height,
                                WindowDestructionReceiver& destruction_receiver) {
  if (actual_physical_width_ == new_physical_width &&
      actual_physical_height_ == new_physical_height) {
    return false;
  }
  actual_physical_width_ = new_physical_width;
  actual_physical_height_ = new_physical_height;
  // The listeners may reference the presenter, so update it first.
  if (presenter_surface_) {
    presenter_->OnSurfaceResize();
  }
  ResizeEvent e(this, {GetActualPhysicalWidth(), GetActualPhysicalHeight()});
  SendEventToListeners([&e](auto listener) { listener->OnResize(e); }, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return true;
  }
  return true;
}

void Window::OnFocusUpdate(bool new_has_focus, WindowDestructionReceiver& destruction_receiver) {
  if (has_focus_ == new_has_focus) {
    return;
  }
  has_focus_ = new_has_focus;
  FocusEvent e(this, has_focus_);
  if (has_focus_) {
    SendEventToListeners([&e](auto listener) { listener->OnGotFocus(e); }, destruction_receiver);
  } else {
    SendEventToListeners([&e](auto listener) { listener->OnLostFocus(e); }, destruction_receiver);
  }
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

bool Window::SendCloseRequestToListeners(WindowDestructionReceiver& destruction_receiver) {
  if (!CanSendEventsToListeners()) {
    return true;
  }
  UIEvent e(UIEvent::Type::kClose, this);
  // Listeners may add or remove listeners or destroy the window, and a veto stops iteration.
  std::vector<WindowListener*> listeners(listeners_);
  for (WindowListener* listener : listeners) {
    bool proceed = listener->OnCloseRequested(e);
    if (destruction_receiver.IsWindowDestroyed()) {
      return false;
    }
    if (!proceed) {
      return false;
    }
  }
  return true;
}

void Window::OnMinimized(WindowDestructionReceiver& destruction_receiver) {
  if (!CanSendEventsToListeners()) {
    return;
  }
  UIEvent e(UIEvent::Type::kMinimized, this);
  SendEventToListeners([&e](auto listener) { listener->OnMinimized(e); }, destruction_receiver);
}

void Window::OnRestored(WindowDestructionReceiver& destruction_receiver) {
  if (!CanSendEventsToListeners()) {
    return;
  }
  UIEvent e(UIEvent::Type::kRestored, this);
  SendEventToListeners([&e](auto listener) { listener->OnRestored(e); }, destruction_receiver);
}

void Window::OnPaint(bool force_paint) {
  if (is_painting_) {
    return;
  }
  is_painting_ = true;
  if (presenter_surface_) {
    presenter_->Paint(force_paint);
  }
  is_painting_ = false;
}

void Window::OnFileDrop(FileDropEvent& e, WindowDestructionReceiver& destruction_receiver) {
  SendEventToListeners([&e](auto listener) { listener->OnFileDrop(e); }, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnKeyDown(KeyEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnKeyDown(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnKeyUp(KeyEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnKeyUp(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnKeyChar(KeyEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnKeyChar(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnMouseDown(MouseEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnMouseDown(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnMouseMove(MouseEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnMouseMove(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnMouseUp(MouseEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnMouseUp(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnMouseWheel(MouseEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnMouseWheel(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

void Window::OnTouchEvent(TouchEvent& e, WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnTouchEvent(e);
        return e.IsHandled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}

template <std::invocable<WindowListener*> F>
void Window::SendEventToListeners(F&& fn, WindowDestructionReceiver& destruction_receiver) {
  if (!CanSendEventsToListeners()) {
    return;
  }
  ListenerIterationContext iteration_context(innermost_listener_iteration_context_);
  innermost_listener_iteration_context_ = &iteration_context;
  while (iteration_context.next_index < listeners_.size()) {
    // next_index can change during the call when the listener modifies the list.
    fn(listeners_[iteration_context.next_index++]);
    if (destruction_receiver.IsWindowDestroyed()) {
      // The listener destroyed the window, so *this and the iteration context are inaccessible.
      return;
    }
    if (!CanSendEventsToListeners()) {
      break;
    }
  }
  REX_ASSERT(innermost_listener_iteration_context_ == &iteration_context);
  innermost_listener_iteration_context_ = innermost_listener_iteration_context_->outer_context;
  if (phase_ == Phase::kClosedLeavingListeners && !innermost_listener_iteration_context_ &&
      !input_listeners_.GetPropagating()) {
    phase_ = Phase::kClosedOpenable;
  }
}

template <std::predicate<WindowInputListener*> F>
void Window::PropagateEventThroughInputListeners(F&& fn,
                                                 WindowDestructionReceiver& destruction_receiver) {
  if (!CanSendEventsToListeners()) {
    return;
  }
  const bool finished = input_listeners_.Propagate([&](WindowInputListener* listener) {
    const bool event_handled = fn(listener);
    if (destruction_receiver.IsWindowDestroyed()) {
      return InputListenerStep::kAbandon;
    }
    if (event_handled || !CanSendEventsToListeners()) {
      return InputListenerStep::kStop;
    }
    return InputListenerStep::kNext;
  });
  if (!finished) {
    return;
  }
  if (phase_ == Phase::kClosedLeavingListeners && !innermost_listener_iteration_context_ &&
      !input_listeners_.GetPropagating()) {
    phase_ = Phase::kClosedOpenable;
  }
}

void Window::SetMinimumSize(WindowSize size) {
  min_size_ = size;
  ApplySizeLimits();
}

void Window::SetAspectRatio(AspectRatio ratio) {
  aspect_ratio_ = ratio;
  ApplySizeLimits();
}

}  // namespace ui
}  // namespace rex
