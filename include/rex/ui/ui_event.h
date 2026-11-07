/**
 * @file        ui/ui_event.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's ui/ui_event.h (Ben Vanik, 2020).
 */

#pragma once

#include <climits>
#include <filesystem>

#include <rex/input_keys.h>
#include <rex/types.h>

namespace rex::ui {

/** Base of every window event. */
class UIEvent {
 public:
  enum class Type : u8 {
    kOpened,        /**< The window opened. UISetupEvent. */
    kDPIChanged,    /**< The window's DPI changed. UISetupEvent. */
    kMonitorUpdate, /**< The window's monitor changed. MonitorUpdateEvent. */
    kMinimized,     /**< The window was minimized. No data. */
    kRestored,      /**< The window was restored. No data. */
    kClosing,       /**< The window is closing after the close was accepted. No data. */
    kFileDrop,      /**< A file was moved onto the window. FileDropEvent. */
    kResize,        /**< The client area changed size. UISetupEvent. */
    kClose,         /**< The user asked to close the window. No data. */
    kFocus,         /**< The window gained or lost focus. UISetupEvent. */
    kTouch,         /**< A touch point changed. TouchEvent. */
  };

  explicit UIEvent(Type type) : type_(type) {}

  virtual ~UIEvent() = default;

  Type GetType() const { return type_; }

  /**
   * Casts this event to the class that carries its data.
   *
   * @return This event as T when GetType() is one of the types T carries, otherwise null.
   */
  template <typename T>
  const T* As() const {
    return T::Carries(type_) ? static_cast<const T*>(this) : nullptr;
  }

 private:
  Type type_;
};

/** Carries the setup state for the window opening, DPI, resize, focus and monitor events. */
class UISetupEvent : public UIEvent {
 public:
  /**
   * Creates a setup event.
   *
   * @param type Opened, DPIChanged, Resize, Focus or MonitorUpdate.
   * @param is_initial_setup True when the window is opening.
   */
  UISetupEvent(Type type, bool is_initial_setup = false)
      : UIEvent(type), is_initial_setup_(is_initial_setup) {}

  /**
   * Reports whether this class carries a type.
   *
   * @return True for Opened, DPIChanged, Resize, Focus and MonitorUpdate.
   */
  static constexpr bool Carries(Type type) {
    return type == Type::kOpened || type == Type::kDPIChanged || type == Type::kResize ||
           type == Type::kFocus || type == Type::kMonitorUpdate;
  }

  bool IsInitialSetup() const { return is_initial_setup_; }

 private:
  bool is_initial_setup_;
};

class MonitorUpdateEvent : public UISetupEvent {
 public:
  /**
   * Creates a monitor update event.
   *
   * @param old_monitor_potentially_disconnected True when the previous monitor may be gone.
   * @param is_initial_setup True when the window is opening.
   */
  MonitorUpdateEvent(bool old_monitor_potentially_disconnected, bool is_initial_setup = false)
      : UISetupEvent(Type::kMonitorUpdate, is_initial_setup),
        old_monitor_potentially_disconnected_(old_monitor_potentially_disconnected) {}

  /**
   * Reports whether this class carries a type.
   *
   * @return True for MonitorUpdate.
   */
  static constexpr bool Carries(Type type) { return type == Type::kMonitorUpdate; }

  bool IsOldMonitorPotentiallyDisconnected() const { return old_monitor_potentially_disconnected_; }

 private:
  bool old_monitor_potentially_disconnected_;
};

class FileDropEvent : public UIEvent {
 public:
  /** Creates an event for a file moved onto the window. */
  FileDropEvent(std::filesystem::path filename)
      : UIEvent(Type::kFileDrop), filename_(std::move(filename)) {}

  /**
   * Reports whether this class carries a type.
   *
   * @return True for FileDrop.
   */
  static constexpr bool Carries(Type type) { return type == Type::kFileDrop; }

  /** Returns the file moved onto the window. */
  const std::filesystem::path& GetFilename() const { return filename_; }

 private:
  const std::filesystem::path filename_;
};

class TouchEvent : public UIEvent {
 public:
  enum class Action : u8 {
    kDown,
    kUp,
    kCancel, /**< The point lifted without its usual release action. */
    kMove,
  };

  /** Pointer ID meaning no current pointer. */
  static constexpr u32 kPointerIDNone = UINT32_MAX;

  /**
   * Creates a touch event.
   *
   * @param x Point x, possibly outside the surface.
   * @param y Point y, possibly outside the surface.
   */
  TouchEvent(Type type, u32 pointer_id, Action action, f32 x, f32 y)
      : UIEvent(type), pointer_id_(pointer_id), action_(action), x_(x), y_(y) {}

  /** Reports whether this class carries a type. */
  static constexpr bool Carries(Type type) { return type == Type::kTouch; }

  /** Reports whether a listener consumed the event. */
  bool IsHandled() const { return handled_; }

  /**
   * Marks the event consumed.
   *
   * @param value True stops later listeners from acting on it.
   */
  void SetHandled(bool value) { handled_ = value; }

  u32 GetPointerID() const { return pointer_id_; }

  Action GetAction() const { return action_; }

  f32 GetX() const { return x_; }

  f32 GetY() const { return y_; }

 private:
  bool handled_ = false;
  u32 pointer_id_;
  Action action_;
  f32 x_;
  f32 y_;
};

/** Receives window and input events on the UI thread. */
class IUIEventListener {
 public:
  virtual ~IUIEventListener() = default;

  /**
   * Called for every window event, in order.
   *
   * @param event Valid only during the call.
   */
  virtual void OnUIEvent(const ui::UIEvent& event) = 0;

  /**
   * Called for every key, character, mouse button, motion and wheel event, in order. A receiver
   * that consumes the event sets its handled flag.
   *
   * @param event Valid only during the call.
   */
  virtual void OnInputEvent(InputEvent& event) = 0;
};

}  // namespace rex::ui
