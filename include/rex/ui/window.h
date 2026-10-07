/**
 * @file        ui/window.h
 * @brief       Platform independent window with desired and actual state and listeners
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's ui/window.h (Ben Vanik, 2020).
 */

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/macros.h>
#include <rex/platform.h>
#include <rex/types.h>
#include <rex/ui.h>
#include <rex/gpu/presenter.h>
#include <rex/gpu/surface.h>
#include <rex/ui/ui_event.h>
#include <rex/hid/virtual_key.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

struct SDL_Window;

namespace rex::ui {

/**
 * A window. Holds the desired state callers set and the actual state the platform reports, and
 * sends events to its listeners. The phases advance in order and loop, and kDeleting may be
 * entered from any phase.
 */
class REX_RUNTIME_API Window : public gpu::IPresentationHost {
 public:
  /** Life cycle phase of the window. */
  enum class Phase {
    kClosedOpenable,         /**< Not open. State changes update only the desired state. */
    kOpening,                /**< OpenImpl runs. External state changes are dropped. */
    kOpen,                   /**< Fully interactive. The only phase that creates a surface. */
    kOpenBeforeClosing,      /**< OnBeforeClose runs. Listeners are still called. */
    kClosing,                /**< The implementation destroys the native window. */
    kClosedLeavingListeners, /**< Closed from a listener that has not returned yet. */
    kDeleting,               /**< The destructor runs. Never left. */
  };

  /** Cursor visibility over the window. */
  enum class CursorVisibility {
    kVisible,    /**< Always shown. */
    kAutoHidden, /**< Shown while the mouse moves, hidden after the auto hide delay. */
    kHidden,     /**< Always hidden. */
  };

  /**
   * Creates the platform window object. The window opens with Open.
   *
   * @param app_context The app context that owns the UI thread.
   * @param title The window title.
   * @return The window.
   */
  static std::unique_ptr<Window> Create(WindowedAppContext& app_context,
                                        const std::string_view title);

  virtual ~Window();

  /**
   * Returns the app context.
   *
   * @return The app context that owns the UI thread.
   */
  WindowedAppContext& app_context() const { return app_context_; }

  /**
   * Returns the life cycle phase.
   *
   * @return The current phase.
   */
  Phase phase() const { return phase_; }

  /**
   * Tests whether the actual state may be read.
   *
   * @return True from kOpening through kClosing.
   */
  bool HasActualState() const { return phase_ >= Phase::kOpening && phase_ <= Phase::kClosing; }

  /**
   * Adds a listener for window events.
   *
   * @param listener The listener.
   */
  void AddListener(WindowListener* listener);

  /**
   * Removes a window event listener.
   *
   * @param listener The listener.
   */
  void RemoveListener(WindowListener* listener);

  /**
   * Adds a listener for input events. Higher z orders receive events first.
   *
   * @param listener The listener.
   * @param z_order Order among input listeners.
   */
  void AddInputListener(WindowInputListener* listener, size_t z_order);

  /**
   * Removes an input event listener.
   *
   * @param listener The listener.
   */
  void RemoveInputListener(WindowInputListener* listener);

  /**
   * Opens the platform window. Listeners may close or destroy the window during the call, check
   * phase() afterward when the window must be open.
   *
   * @return False only when the platform failed to open the window.
   */
  bool Open();

  /**
   * Requests closing. The platform may close the window later, check phase() afterward.
   */
  void RequestClose() {
    if (phase_ != Phase::kOpen) {
      return;
    }
    RequestCloseImpl();
  }

  /**
   * Returns the DPI that maps logical sizes 1 to 1 onto physical ones.
   *
   * @return 96 unless the platform differs.
   */
  virtual u32 GetMediumDpi() const { return 96; }

  /**
   * Returns the window DPI.
   *
   * @return The latest DPI, or the medium DPI when the platform reports none.
   */
  u32 GetDpi() const {
    u32 dpi = GetLatestDpiImpl();
    return dpi ? dpi : GetMediumDpi();
  }

  /**
   * Converts a size between 2 DPIs, rounding up. Round trips may differ.
   *
   * @param size The size.
   * @param new_dpi The DPI to convert to.
   * @param old_dpi The DPI of size.
   * @return The converted size.
   */
  static constexpr u32 ConvertSizeDpi(u32 size, u32 new_dpi, u32 old_dpi) {
    return u32((u64(size) * new_dpi + (old_dpi - 1)) / old_dpi);
  }

  /**
   * Converts a physical size to a logical one.
   *
   * @param size The physical size.
   * @return The logical size.
   */
  u32 SizeToLogical(u32 size) const { return ConvertSizeDpi(size, GetMediumDpi(), GetDpi()); }

  /**
   * Converts a logical size to a physical one.
   *
   * @param size The logical size.
   * @return The physical size.
   */
  u32 SizeToPhysical(u32 size) const { return ConvertSizeDpi(size, GetDpi(), GetMediumDpi()); }

  /**
   * Returns the desired logical width of the window when it is neither maximized nor
   * fullscreen.
   *
   * @return The desired logical width.
   */
  u32 GetDesiredLogicalWidth() const { return desired_logical_width_; }

  /**
   * Returns the desired logical height of the window when it is neither maximized nor
   * fullscreen.
   *
   * @return The desired logical height.
   */
  u32 GetDesiredLogicalHeight() const { return desired_logical_height_; }

  /**
   * Sets the desired logical size of the window when it is neither maximized nor fullscreen.
   *
   * @param new_desired_logical_width The width.
   * @param new_desired_logical_height The height.
   */
  void SetDesiredLogicalSize(u32 new_desired_logical_width, u32 new_desired_logical_height);

  /**
   * Resolves the logical size the ui cvars configure.
   *
   * @param width_out Receives the width.
   * @param height_out Receives the height.
   */
  static void ResolveConfiguredLogicalSize(u32& width_out, u32& height_out);

  /**
   * Returns the physical width of the client area. May be 0 for an open window.
   *
   * @return The width, or 0 without actual state.
   */
  u32 GetActualPhysicalWidth() const { return HasActualState() ? actual_physical_width_ : 0; }

  /**
   * Returns the physical height of the client area. May be 0 for an open window.
   *
   * @return The height, or 0 without actual state.
   */
  u32 GetActualPhysicalHeight() const { return HasActualState() ? actual_physical_height_ : 0; }

  /**
   * Reads the pixel size of the display the window is on.
   *
   * @param width Receives the width.
   * @param height Receives the height.
   * @return False when the platform cannot tell.
   */
  virtual bool GetDisplayPixelSize(u32& width, u32& height) const {
    (void)width;
    (void)height;
    return false;
  }

  /**
   * Tests whether the window is fullscreen.
   *
   * @return The desired fullscreen state.
   */
  bool IsFullscreen() const { return fullscreen_; }

  /**
   * Enters or leaves fullscreen.
   *
   * @param new_fullscreen True for fullscreen.
   */
  void SetFullscreen(bool new_fullscreen);

  /** Applies the fullscreen mode again, after the video mode cvars change. */
  void RefreshFullscreen();

  /**
   * Returns the desired monitor.
   *
   * @return The monitor index.
   */
  i32 GetMonitor() const { return monitor_; }

  /**
   * Moves the window to a monitor.
   *
   * @param new_monitor The monitor index.
   */
  void SetMonitor(i32 new_monitor);

  /**
   * Returns the title.
   *
   * @return The desired title.
   */
  const std::string& GetTitle() const { return title_; }

  /**
   * Sets the title.
   *
   * @param new_title The title.
   */
  void SetTitle(const std::string_view new_title);

  /** Brings the window to the front and gives it focus. */
  virtual void Raise() {}

  /**
   * Returns the platform window handle.
   *
   * @return HWND on Windows, NSWindow on macOS, the X11 window or wl_surface on Linux, or
   *         nullptr before the window opens.
   */
  virtual void* native() const { return nullptr; }

  /**
   * Returns the SDL window.
   *
   * @return The SDL window, or nullptr before the window opens.
   */
  virtual SDL_Window* sdl() const { return nullptr; }

  /**
   * Sets the smallest client area the window accepts. Applies now and whenever the window
   * opens.
   *
   * @param size Minimum size. A 0 field means no limit on that axis.
   */
  void SetMinimumSize(WindowSize size);

  /**
   * Sets the client area shape the window holds while the user resizes it. Applies now and
   * whenever the window opens.
   *
   * @param ratio The shape, AspectRatio::kStretch for none.
   */
  void SetAspectRatio(AspectRatio ratio);

  /** Adds a mouse capture request. */
  void CaptureMouse();

  /** Releases a mouse capture request. */
  void ReleaseMouse();

  /**
   * Locks the pointer and switches motion events to relative deltas, for mouse look.
   *
   * @param enable True locks the pointer.
   * @return True when the mode took.
   */
  virtual bool SetRelativeMouseMode(bool enable) {
    (void)enable;
    return false;
  }

  /**
   * Moves the pointer to the center, for platforms without relative mode.
   *
   * @param x_out Receives the center x when the pointer got there.
   * @param y_out Receives the center y when the pointer got there.
   * @return True when the pointer verifiably reached the center.
   */
  virtual bool WarpMouseToCenter(i32& x_out, i32& y_out) {
    (void)x_out;
    (void)y_out;
    return false;
  }

  /**
   * Tests whether the window is an active text input field for the OS input method.
   *
   * @return The desired text input state.
   */
  bool IsTextInputActive() const { return text_input_active_; }

  /**
   * Turns text input for the OS input method on or off.
   *
   * @param active True turns it on.
   */
  void SetTextInputActive(bool active);

  /**
   * Returns the cursor visibility.
   *
   * @return The desired visibility.
   */
  CursorVisibility GetCursorVisibility() const { return cursor_visibility_; }

  /**
   * Sets the cursor visibility. kAutoHidden from another value hides the cursor at once.
   *
   * @param new_cursor_visibility The visibility.
   */
  void SetCursorVisibility(CursorVisibility new_cursor_visibility);

  /**
   * Returns the auto hide delay.
   *
   * @return Idle milliseconds before kAutoHidden hides the cursor.
   */
  u32 GetCursorAutoHideDelayMs() const { return cursor_auto_hide_delay_ms_; }

  /**
   * Sets the auto hide delay. Applies from the next mouse motion.
   *
   * @param delay_ms Idle milliseconds before kAutoHidden hides the cursor.
   */
  void SetCursorAutoHideDelayMs(u32 delay_ms) { cursor_auto_hide_delay_ms_ = delay_ms; }

  /**
   * Tests whether the window has focus.
   *
   * @return True when the window has focus.
   */
  bool HasFocus() const { return HasActualState() ? has_focus_ : false; }

  /** Requests focus. The platform may apply it later or drop it. */
  void Focus();

  /**
   * Sets the presenter that paints the window.
   *
   * @param presenter The presenter, or null.
   */
  void SetPresenter(gpu::IPresenter* presenter);

  /** Requests a repaint. Callable from any thread while the surface exists. */
  void RequestPaint() override {
    if (presenter_surface_) {
      RequestPaintImpl();
    }
  }

  /** The presenter left this window. Drops the surface without calling back. */
  void OnPresenterDetached() override;

  /** Requests a repaint of the presenter UI from the UI thread. */
  void RequestPresenterUIPaintFromUIThread() {
    if (presenter_) {
      presenter_->RequestOverlayPaint();
    }
  }

 protected:
  /**
   * Detects destruction of the window from within an event handler. Create 1 on the stack
   * around code that calls listeners, and stop touching the window once IsWindowDestroyed is
   * true. Only LIFO order is supported.
   */
  class WindowDestructionReceiver {
   public:
    /**
     * Starts watching a window.
     *
     * @param window The window, or null.
     */
    explicit WindowDestructionReceiver(Window* window) : window_(window) {
      if (window_) {
        outer_receiver_ = window_->innermost_destruction_receiver_;
        window_->innermost_destruction_receiver_ = this;
      }
    }

    ~WindowDestructionReceiver() {
      if (window_) {
        assert_true(window_->innermost_destruction_receiver_ == this);
        window_->innermost_destruction_receiver_ = outer_receiver_;
      }
    }

    /**
     * Tests whether the window was destroyed.
     *
     * @return True once the window destructor ran.
     */
    bool IsWindowDestroyed() const { return window_ == nullptr; }

    /**
     * Tests whether the window was destroyed or closed.
     *
     * @return True when the native window can no longer be used.
     */
    bool IsWindowDestroyedOrClosed() const { return IsWindowDestroyed() || window_->IsClosed(); }

    /**
     * Tests whether state may no longer be applied.
     *
     * @return True when the window was destroyed or cannot apply state.
     */
    bool IsWindowDestroyedOrStateInapplicable() const {
      return IsWindowDestroyed() || !window_->CanApplyState();
    }

    /**
     * Tests whether listeners may no longer be called.
     *
     * @return True when the window was destroyed or cannot send events.
     */
    bool IsWindowDestroyedOrListenersUncallable() const {
      return IsWindowDestroyed() || !window_->CanSendEventsToListeners();
    }

   private:
    friend Window;
    Window* window_;
    WindowDestructionReceiver* outer_receiver_ = nullptr;
  };

  /** Default auto hide delay, the one Windows Media Player uses. */
  static constexpr u32 kDefaultCursorAutoHideMilliseconds = 3333;

  /**
   * Creates the common window state.
   *
   * @param app_context The app context that owns the UI thread.
   * @param title The window title.
   * @param desired_logical_width The desired logical width.
   * @param desired_logical_height The desired logical height.
   */
  Window(WindowedAppContext& app_context, const std::string_view title, u32 desired_logical_width,
         u32 desired_logical_height);

  /** Enters kDeleting and detaches the surface. Implementation destructors call it first. */
  void EnterDestructor() {
    phase_ = Phase::kDeleting;
    OnSurfaceChanged(false);
  }

  /**
   * Returns the current DPI, or the closest guess for a window that is not open.
   *
   * @return The DPI, 0 for the medium DPI.
   */
  virtual u32 GetLatestDpiImpl() const { return GetMediumDpi(); }

  /**
   * Creates the native window from the desired state and reports the initial actual state
   * through OnActualSizeUpdate and OnFocusUpdate. Must not destroy the window, and must leave
   * a state that allows another attempt on failure.
   *
   * @return False when the native window could not be created.
   */
  virtual bool OpenImpl() = 0;

  /** Closes the native window. */
  virtual void RequestCloseImpl() = 0;

  /** Applies a changed desired fullscreen state. */
  virtual void ApplyNewFullscreen() {}

  /** Applies a changed desired monitor. */
  virtual void ApplyNewMonitor() {}

  /** Applies a changed desired logical size. */
  virtual void ApplyNewDesiredLogicalSize() {}

  /** Applies a changed title. */
  virtual void ApplyNewTitle() {}

  /** Captures the mouse, even when it is already assumed captured. */
  virtual void ApplyNewMouseCapture() {}

  /** Releases the mouse. */
  virtual void ApplyNewMouseRelease() {}

  /** Applies a changed text input state. */
  virtual void ApplyNewTextInputActive() {}

  /**
   * Applies a changed cursor visibility.
   *
   * @param old_cursor_visibility The visibility before the change.
   */
  virtual void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) {
    (void)old_cursor_visibility;
  }

  /** Requests focus from the platform. */
  virtual void FocusImpl() {}

  /**
   * Returns the presenter.
   *
   * @return The presenter, or null.
   */
  gpu::IPresenter* presenter() const { return presenter_; }

  /**
   * Recreates or destroys the presenter surface. Calls no virtual function when
   * new_surface_potentially_exists is false, so the destructor may call it.
   *
   * @param new_surface_potentially_exists False only destroys the surface.
   */
  void OnSurfaceChanged(bool new_surface_potentially_exists);

  /**
   * Creates a surface for the open window.
   *
   * @param allowed_types The surface types the presenter accepts.
   * @return The surface, or null.
   */
  virtual std::unique_ptr<gpu::Surface> CreateSurfaceImpl(
      gpu::Surface::TypeFlags allowed_types) = 0;

  /** Requests a paint of the existing surface. */
  virtual void RequestPaintImpl() = 0;

  /**
   * Runs the closing listeners and disconnects the surface.
   *
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnBeforeClose(WindowDestructionReceiver& destruction_receiver);

  /** Completes closing after the implementation destroyed the native window. */
  void OnAfterClose();

  /**
   * Asks every listener whether a user close may proceed.
   *
   * @param destruction_receiver Receiver that detects destruction during the call.
   * @return False when a listener vetoed or the window was destroyed.
   */
  bool SendCloseRequestToListeners(WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports that the window was minimized.
   *
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnMinimized(WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports that the window was restored.
   *
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnRestored(WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a DPI change.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnDpiChanged(UISetupEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a monitor change.
   *
   * @param e The event.
   */
  void OnMonitorUpdate(MonitorUpdateEvent& e);

  /**
   * Records a desired logical size the platform chose.
   *
   * @param new_desired_logical_width The width.
   * @param new_desired_logical_height The height.
   */
  void OnDesiredLogicalSizeUpdate(u32 new_desired_logical_width, u32 new_desired_logical_height) {
    desired_logical_width_ = new_desired_logical_width;
    desired_logical_height_ = new_desired_logical_height;
  }

  /**
   * Records a client area size and notifies what depends on it.
   *
   * @param new_physical_width The width.
   * @param new_physical_height The height.
   * @param destruction_receiver Receiver that detects destruction during the call.
   * @return False when the size did not change.
   */
  bool OnActualSizeUpdate(u32 new_physical_width, u32 new_physical_height,
                          WindowDestructionReceiver& destruction_receiver);

  /**
   * Records a focus change and notifies the listeners.
   *
   * @param new_has_focus True when the window gained focus.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnFocusUpdate(bool new_has_focus, WindowDestructionReceiver& destruction_receiver);

  /**
   * Paints the window.
   *
   * @param force_paint True when the platform cannot retain the previous image.
   */
  void OnPaint(bool force_paint = false);

  /**
   * Reports a dropped file.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnFileDrop(FileDropEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a key press.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnKeyDown(KeyEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a key release.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnKeyUp(KeyEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a typed character.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnKeyChar(KeyEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a mouse button press.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnMouseDown(MouseEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports mouse motion.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnMouseMove(MouseEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a mouse button release.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnMouseUp(MouseEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a wheel change.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnMouseWheel(MouseEvent& e, WindowDestructionReceiver& destruction_receiver);

  /**
   * Reports a touch point change.
   *
   * @param e The event.
   * @param destruction_receiver Receiver that detects destruction during the call.
   */
  void OnTouchEvent(TouchEvent& e, WindowDestructionReceiver& destruction_receiver);

  /** Applies the stored minimum size and aspect ratio to the open window. */
  virtual void ApplySizeLimits() {}

  /** Minimum client size set through SetMinimumSize. */
  WindowSize min_size_;
  /** Client shape set through SetAspectRatio. */
  AspectRatio aspect_ratio_ = AspectRatio::kStretch;

 private:
  struct ListenerIterationContext {
    explicit ListenerIterationContext(ListenerIterationContext* outer_context,
                                      size_t first_index = 0)
        : outer_context(outer_context), next_index(first_index) {}
    ListenerIterationContext* outer_context;
    size_t next_index;
  };

  struct InputListenerIterationContext {
    explicit InputListenerIterationContext(
        InputListenerIterationContext* outer_context,
        std::multimap<size_t, WindowInputListener*>::const_reverse_iterator first_iterator,
        size_t first_z_order = SIZE_MAX)
        : outer_context(outer_context),
          next_iterator(first_iterator),
          current_z_order(first_z_order) {}
    InputListenerIterationContext* outer_context;
    std::multimap<size_t, WindowInputListener*>::const_reverse_iterator next_iterator;
    size_t current_z_order;
  };

  bool IsClosed() const { return phase_ < Phase::kOpening || phase_ > Phase::kOpenBeforeClosing; }

  bool CanApplyState() const {
    return phase_ >= Phase::kOpen && phase_ <= Phase::kOpenBeforeClosing;
  }

  bool CanSendEventsToListeners() const {
    return phase_ >= Phase::kOpen && phase_ <= Phase::kOpenBeforeClosing;
  }

  void SendEventToListeners(std::function<void(WindowListener*)> fn,
                            WindowDestructionReceiver& destruction_receiver);
  void PropagateEventThroughInputListeners(std::function<bool(WindowInputListener*)> fn,
                                           WindowDestructionReceiver& destruction_receiver);

  std::unique_ptr<gpu::Surface> CreateSurface(gpu::Surface::TypeFlags allowed_types) {
    if (phase_ != Phase::kOpen) {
      return nullptr;
    }
    return CreateSurfaceImpl(allowed_types);
  }

  WindowedAppContext& app_context_;

  Phase phase_ = Phase::kClosedOpenable;
  WindowDestructionReceiver* innermost_destruction_receiver_ = nullptr;

  std::vector<WindowListener*> listeners_;
  std::multimap<size_t, WindowInputListener*> input_listeners_;
  ListenerIterationContext* innermost_listener_iteration_context_ = nullptr;
  InputListenerIterationContext* innermost_input_listener_iteration_context_ = nullptr;

  u32 desired_logical_width_ = 0;
  u32 desired_logical_height_ = 0;
  u32 actual_physical_width_ = 0;
  u32 actual_physical_height_ = 0;

  bool fullscreen_ = false;
  i32 monitor_ = 0;

  std::string title_;

  u32 mouse_capture_request_count_ = 0;

  bool text_input_active_ = false;

  CursorVisibility cursor_visibility_ = CursorVisibility::kVisible;

  u32 cursor_auto_hide_delay_ms_ = kDefaultCursorAutoHideMilliseconds;

  bool has_focus_ = false;

  gpu::IPresenter* presenter_ = nullptr;
  std::unique_ptr<gpu::Surface> presenter_surface_;
  bool is_painting_ = false;
};

}  // namespace rex::ui
