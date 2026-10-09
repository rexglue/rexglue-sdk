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

#include <rex/types.h>
#include <rex/ui.h>
#include <rex/hid/virtual_key.h>

namespace rex::ui {

class IWindow;

/** Base of every window and input event. */
class UIEvent {
 public:
  /** Kinds of event. */
  enum class Type : u8 {
    Opened,        /**< The window opened. UISetupEvent. */
    DpiChanged,    /**< The window's DPI changed. UISetupEvent. */
    MonitorUpdate, /**< The window's monitor changed. MonitorUpdateEvent. */
    Minimized,     /**< The window was minimized. No data. */
    Restored,      /**< The window was restored. No data. */
    Closing,       /**< The window is closing after the close was accepted. No data. */
    FileDrop,      /**< A file was dropped on the window. FileDropEvent. */
    Resize,        /**< The client area changed size. ResizeEvent. */
    Close,         /**< The user asked to close the window. No data. */
    Focus,         /**< The window gained or lost focus. FocusEvent. */
    KeyDown,       /**< A key was pressed. KeyEvent. */
    KeyUp,         /**< A key was released. KeyEvent. */
    Char,          /**< Text was typed. KeyEvent. */
    MouseDown,     /**< A mouse button was pressed. MouseEvent. */
    MouseUp,       /**< A mouse button was released. MouseEvent. */
    MouseMove,     /**< The mouse moved. MouseEvent. */
    MouseWheel,    /**< The wheel turned. MouseEvent. */
    Touch,         /**< A touch point changed. TouchEvent. */
  };

  /**
   * Creates an event.
   *
   * @param type Event kind.
   * @param target Window the event belongs to.
   */
  UIEvent(Type type, IWindow* target) : type_(type), target_(target) {}

  virtual ~UIEvent() = default;

  /**
   * Returns the event kind.
   *
   * @return The type the event was created with.
   */
  Type type() const { return type_; }

  /**
   * Returns the window the event belongs to.
   *
   * @return The target window.
   */
  IWindow* target() const { return target_; }

  /**
   * Casts this event to the class that carries its data.
   *
   * @tparam T A class derived from UIEvent.
   * @return This event as T when type() is 1 of the types T carries, otherwise nullptr.
   */
  template <typename T>
  const T* As() const {
    return T::Carries(type_) ? static_cast<const T*>(this) : nullptr;
  }

 private:
  Type type_;
  IWindow* target_;
};

/** Carries the setup state for the window opening, DPI, resize, focus and monitor events. */
class UISetupEvent : public UIEvent {
 public:
  /**
   * Creates a setup event.
   *
   * @param type Opened, DpiChanged, Resize, Focus or MonitorUpdate.
   * @param target Window the event belongs to.
   * @param is_initial_setup True when the window is opening.
   */
  UISetupEvent(Type type, IWindow* target, bool is_initial_setup = false)
      : UIEvent(type, target), is_initial_setup_(is_initial_setup) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for Opened, DpiChanged, Resize, Focus and MonitorUpdate.
   */
  static constexpr bool Carries(Type type) {
    return type == Type::Opened || type == Type::DpiChanged || type == Type::Resize ||
           type == Type::Focus || type == Type::MonitorUpdate;
  }

  /**
   * Reports whether the window is opening.
   *
   * @return True when the event is part of the window opening.
   */
  bool is_initial_setup() const { return is_initial_setup_; }

 private:
  bool is_initial_setup_;
};

/** Carries the new client area size for UIEvent::Type::Resize. */
class ResizeEvent : public UISetupEvent {
 public:
  /**
   * Creates a resize event.
   *
   * @param target Window the event belongs to.
   * @param size The client area size after the change.
   * @param is_initial_setup True when the window is opening.
   */
  ResizeEvent(IWindow* target, WindowSize size, bool is_initial_setup = false)
      : UISetupEvent(Type::Resize, target, is_initial_setup), size_(size) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for Resize.
   */
  static constexpr bool Carries(Type type) { return type == Type::Resize; }

  /**
   * Returns the new client area size.
   *
   * @return The size after the change.
   */
  WindowSize size() const { return size_; }

 private:
  WindowSize size_;
};

/** Carries the focus state for UIEvent::Type::Focus. */
class FocusEvent : public UISetupEvent {
 public:
  /**
   * Creates a focus event.
   *
   * @param target Window the event belongs to.
   * @param focused True when the window gained focus.
   * @param is_initial_setup True when the window is opening.
   */
  FocusEvent(IWindow* target, bool focused, bool is_initial_setup = false)
      : UISetupEvent(Type::Focus, target, is_initial_setup), focused_(focused) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for Focus.
   */
  static constexpr bool Carries(Type type) { return type == Type::Focus; }

  /**
   * Reports whether the window gained focus.
   *
   * @return True when the window gained focus, false when it lost it.
   */
  bool focused() const { return focused_; }

 private:
  bool focused_;
};

/** Carries the monitor change for UIEvent::Type::MonitorUpdate. */
class MonitorUpdateEvent : public UISetupEvent {
 public:
  /**
   * Creates a monitor update event.
   *
   * @param target Window the event belongs to.
   * @param old_monitor_potentially_disconnected True when the previous monitor may be gone.
   * @param is_initial_setup True when the window is opening.
   */
  MonitorUpdateEvent(IWindow* target, bool old_monitor_potentially_disconnected,
                     bool is_initial_setup = false)
      : UISetupEvent(Type::MonitorUpdate, target, is_initial_setup),
        old_monitor_potentially_disconnected_(old_monitor_potentially_disconnected) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for MonitorUpdate.
   */
  static constexpr bool Carries(Type type) { return type == Type::MonitorUpdate; }

  /**
   * Reports whether the previous monitor may have been disconnected.
   *
   * @return True when the previous monitor may be gone.
   */
  bool old_monitor_potentially_disconnected() const {
    return old_monitor_potentially_disconnected_;
  }

 private:
  bool old_monitor_potentially_disconnected_;
};

/** Carries the dropped file for UIEvent::Type::FileDrop. */
class FileDropEvent : public UIEvent {
 public:
  /**
   * Creates a file drop event.
   *
   * @param target Window the event belongs to.
   * @param filename Path of the dropped file.
   */
  FileDropEvent(IWindow* target, std::filesystem::path filename)
      : UIEvent(Type::FileDrop, target), filename_(std::move(filename)) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for FileDrop.
   */
  static constexpr bool Carries(Type type) { return type == Type::FileDrop; }

  /**
   * Returns the dropped file.
   *
   * @return Path of the dropped file.
   */
  const std::filesystem::path& filename() const { return filename_; }

 private:
  const std::filesystem::path filename_;
};

/** Carries a key press, release or typed character. */
class KeyEvent : public UIEvent {
 public:
  /**
   * Creates a key event.
   *
   * @param type KeyDown, KeyUp or Char.
   * @param target Window the event belongs to.
   * @param virtual_key The key, or the typed code point for Char.
   * @param repeat_count Number of repeats the event stands for.
   * @param prev_state True when the key was down before this event.
   * @param modifier_shift_pressed True while Shift is held.
   * @param modifier_ctrl_pressed True while Ctrl is held.
   * @param modifier_alt_pressed True while Alt is held.
   */
  KeyEvent(Type type, IWindow* target, hid::VirtualKey virtual_key, i32 repeat_count,
           bool prev_state, bool modifier_shift_pressed, bool modifier_ctrl_pressed,
           bool modifier_alt_pressed)
      : UIEvent(type, target),
        virtual_key_(virtual_key),
        repeat_count_(repeat_count),
        prev_state_(prev_state),
        modifier_shift_pressed_(modifier_shift_pressed),
        modifier_ctrl_pressed_(modifier_ctrl_pressed),
        modifier_alt_pressed_(modifier_alt_pressed) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for KeyDown, KeyUp and Char.
   */
  static constexpr bool Carries(Type type) {
    return type == Type::KeyDown || type == Type::KeyUp || type == Type::Char;
  }

  /**
   * Reports whether a listener consumed the event.
   *
   * @return True once a listener marked the event handled.
   */
  bool is_handled() const { return handled_; }

  /**
   * Marks the event consumed.
   *
   * @param value True stops later listeners from acting on it.
   */
  void set_handled(bool value) { handled_ = value; }

  /**
   * Returns the key.
   *
   * @return The key, or the typed code point for Char.
   */
  hid::VirtualKey virtual_key() const { return virtual_key_; }

  /**
   * Returns the repeat count.
   *
   * @return Number of repeats the event stands for.
   */
  i32 repeat_count() const { return repeat_count_; }

  /**
   * Reports whether the key was down before this event.
   *
   * @return True when the key was already down.
   */
  bool prev_state() const { return prev_state_; }

  /**
   * Reports whether Shift was held.
   *
   * @return True while Shift is held.
   */
  bool is_shift_pressed() const { return modifier_shift_pressed_; }

  /**
   * Reports whether Ctrl was held.
   *
   * @return True while Ctrl is held.
   */
  bool is_ctrl_pressed() const { return modifier_ctrl_pressed_; }

  /**
   * Reports whether Alt was held.
   *
   * @return True while Alt is held.
   */
  bool is_alt_pressed() const { return modifier_alt_pressed_; }

 private:
  bool handled_ = false;
  hid::VirtualKey virtual_key_ = hid::VirtualKey::kNone;
  i32 repeat_count_ = 0;
  bool prev_state_ = false;
  bool modifier_shift_pressed_ = false;
  bool modifier_ctrl_pressed_ = false;
  bool modifier_alt_pressed_ = false;
};

/** Carries a mouse button, move or wheel change. */
class MouseEvent : public UIEvent {
 public:
  /** Mouse buttons. */
  enum class Button {
    kNone = 0, /**< No button. */
    kLeft,     /**< The left button. */
    kRight,    /**< The right button. */
    kMiddle,   /**< The middle button. */
    kX1,       /**< The first extra button. */
    kX2,       /**< The second extra button. */
  };

  /** Scroll units per wheel detent, matching the Windows WHEEL_DELTA. */
  static constexpr u32 kScrollPerDetent = 120;

  /**
   * Creates a mouse event.
   *
   * @param type MouseDown, MouseUp, MouseMove or MouseWheel.
   * @param target Window the event belongs to.
   * @param button The button, kNone for a move or wheel.
   * @param x Cursor x in physical pixels.
   * @param y Cursor y in physical pixels.
   * @param scroll_x Horizontal wheel change.
   * @param scroll_y Vertical wheel change, positive away from the user.
   * @param dx Motion since the previous event in physical pixels.
   * @param dy Motion since the previous event in physical pixels.
   */
  MouseEvent(Type type, IWindow* target, Button button, i32 x, i32 y, i32 scroll_x = 0,
             i32 scroll_y = 0, f32 dx = 0.0f, f32 dy = 0.0f)
      : UIEvent(type, target),
        button_(button),
        x_(x),
        y_(y),
        scroll_x_(scroll_x),
        scroll_y_(scroll_y),
        dx_(dx),
        dy_(dy) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for MouseDown, MouseUp, MouseMove and MouseWheel.
   */
  static constexpr bool Carries(Type type) {
    return type == Type::MouseDown || type == Type::MouseUp || type == Type::MouseMove ||
           type == Type::MouseWheel;
  }

  /**
   * Reports whether a listener consumed the event.
   *
   * @return True once a listener marked the event handled.
   */
  bool is_handled() const { return handled_; }

  /**
   * Marks the event consumed.
   *
   * @param value True stops later listeners from acting on it.
   */
  void set_handled(bool value) { handled_ = value; }

  /**
   * Returns the button.
   *
   * @return The button, kNone for a move or wheel.
   */
  Button button() const { return button_; }

  /**
   * Returns the cursor x.
   *
   * @return Cursor x in physical pixels.
   */
  i32 x() const { return x_; }

  /**
   * Returns the cursor y.
   *
   * @return Cursor y in physical pixels.
   */
  i32 y() const { return y_; }

  /**
   * Returns the horizontal wheel change.
   *
   * @return Scroll units, kScrollPerDetent per detent.
   */
  i32 scroll_x() const { return scroll_x_; }

  /**
   * Returns the vertical wheel change.
   *
   * @return Scroll units, positive away from the user.
   */
  i32 scroll_y() const { return scroll_y_; }

  /**
   * Returns the motion since the previous event. Moves while the pointer is locked for mouse look,
   * where x and y do not.
   *
   * @return Fractional physical pixels.
   */
  f32 dx() const { return dx_; }

  /**
   * Returns the motion since the previous event. Moves while the pointer is locked for mouse look,
   * where x and y do not.
   *
   * @return Fractional physical pixels.
   */
  f32 dy() const { return dy_; }

 private:
  bool handled_ = false;
  Button button_;
  i32 x_ = 0;
  i32 y_ = 0;
  i32 scroll_x_ = 0;
  i32 scroll_y_ = 0;
  f32 dx_ = 0.0f;
  f32 dy_ = 0.0f;
};

/** Carries a touch point change. */
class TouchEvent : public UIEvent {
 public:
  /** Touch point changes. */
  enum class Action {
    kDown,   /**< The point touched down. */
    kUp,     /**< The point lifted. */
    kCancel, /**< The point lifted without its usual release action. */
    kMove,   /**< The point moved. */
  };

  /** Pointer id meaning no current pointer. */
  static constexpr u32 kPointerIDNone = UINT32_MAX;

  /**
   * Creates a touch event.
   *
   * @param type Touch.
   * @param target Window the event belongs to.
   * @param pointer_id Id of the touch point.
   * @param action What the point did.
   * @param x Point x, possibly outside the surface.
   * @param y Point y, possibly outside the surface.
   */
  TouchEvent(Type type, IWindow* target, u32 pointer_id, Action action, f32 x, f32 y)
      : UIEvent(type, target), pointer_id_(pointer_id), action_(action), x_(x), y_(y) {}

  /**
   * Reports whether this class carries a type.
   *
   * @param type The type to test.
   * @return True for Touch.
   */
  static constexpr bool Carries(Type type) { return type == Type::Touch; }

  /**
   * Reports whether a listener consumed the event.
   *
   * @return True once a listener marked the event handled.
   */
  bool is_handled() const { return handled_; }

  /**
   * Marks the event consumed.
   *
   * @param value True stops later listeners from acting on it.
   */
  void set_handled(bool value) { handled_ = value; }

  /**
   * Returns the touch point id.
   *
   * @return Id of the touch point.
   */
  u32 pointer_id() const { return pointer_id_; }

  /**
   * Returns what the point did.
   *
   * @return The action.
   */
  Action action() const { return action_; }

  /**
   * Returns the point x.
   *
   * @return Point x, possibly outside the surface.
   */
  f32 x() const { return x_; }

  /**
   * Returns the point y.
   *
   * @return Point y, possibly outside the surface.
   */
  f32 y() const { return y_; }

 private:
  bool handled_ = false;
  u32 pointer_id_;
  Action action_;
  f32 x_;
  f32 y_;
};

}  // namespace rex::ui
