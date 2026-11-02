/**
 * @file        kernel/object/object_pin.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <utility>
#include <variant>

#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/kernel_object.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/notify_listener_object.h>
#include <rex/kernel/object/xam_object.h>
#include <rex/kernel/types/status.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::kernel {

/** A transient pin on a `KernelObject`. Never stored. Move only. */
class ObjectPin : public rex::MoveOnly {
 public:
  ObjectPin() = default;
  /**
   * Takes a pin already taken on a `KernelObject`.
   *
   * @param handle The handle the pin was resolved from.
   */
  ObjectPin(KernelObject* object, X_HANDLE handle) noexcept : object_(object), handle_(handle) {}

  ObjectPin(ObjectPin&& other) noexcept : object_(other.object_), handle_(other.handle_) {
    other.object_ = nullptr;
  }
  ObjectPin& operator=(ObjectPin&& other) noexcept {
    if (this != &other) {
      Reset();
      object_ = other.object_;
      handle_ = other.handle_;
      other.object_ = nullptr;
    }
    return *this;
  }
  ~ObjectPin() { Reset(); }

  explicit operator bool() const noexcept { return object_ != nullptr; }

  [[nodiscard]] ObjectType GetType() const { return object_->GetType(); }

  [[nodiscard]] ObjectData& GetData() const { return object_->data; }

  [[nodiscard]] KernelObject& GetKernelObject() const { return *object_; }

  /** Returns the handle value the pin was looked up by, with pseudo handles translated. */
  [[nodiscard]] X_HANDLE GetHandle() const noexcept { return handle_; }

  /** Returns the guest address of the object body, 0 when the object has no guest block. */
  [[nodiscard]] u32 GetGuestAddress() const { return object_->guest_ptr; }

  void Reset() noexcept {
    if (object_ != nullptr) {
      if (object_->pins.Unpin()) {
        object_->destroy(object_);
      }
      object_ = nullptr;
    }
  }

 private:
  KernelObject* object_ = nullptr;
  X_HANDLE handle_ = 0;
};

/**
 * Maps T to the object struct a pin looks for. An interface maps to the struct that implements it.
 *
 * @tparam T An object struct, or an interface an object struct implements.
 */
template <typename T>
struct ObjectViewOf {
  using Type = T; /**< The alternative of ObjectData to look for. */
};

template <>
struct ObjectViewOf<IGuestEvent> {
  using Type = EventObject;
};

template <>
struct ObjectViewOf<IOpenedFile> {
  using Type = FileObject;
};

template <>
struct ObjectViewOf<INotifyListenerQueue> {
  using Type = NotifyListenerObject;
};

template <>
struct ObjectViewOf<ILoadedModule> {
  using Type = ModuleObject;
};

/**
 * A pin that exposes the object struct of a known type.
 *
 * @tparam T The object struct the `KernelObject` is expected to hold, or an interface it
 *         implements.
 */
template <typename T>
class Pinned {
 public:
  Pinned() = default;
  /**
   * Views an object pin as type T. The view is empty when the pin holds another type.
   *
   * @param pin The object pin, moved in.
   */
  explicit Pinned(ObjectPin pin) : slot_(std::move(pin)) {
    if (slot_) {
      if constexpr (XAMObjectClass<T>) {
        auto* xam = std::get_if<XAMObject>(&slot_.GetData());
        if (xam != nullptr && xam->body && xam->body->GetTag() == T::kTag) {
          value_ = static_cast<T*>(xam->body.get());
        }
      } else {
        value_ = std::get_if<typename ObjectViewOf<T>::Type>(&slot_.GetData());
      }
      if (value_ == nullptr) {
        slot_.Reset();
      }
    }
  }

  explicit operator bool() const noexcept { return value_ != nullptr; }

  T* operator->() const noexcept { return value_; }

  T& operator*() const noexcept { return *value_; }

  [[nodiscard]] X_HANDLE GetHandle() const noexcept { return slot_.GetHandle(); }

  [[nodiscard]] u32 GetGuestAddress() const { return slot_.GetGuestAddress(); }

  [[nodiscard]] const ObjectPin& GetSlot() const noexcept { return slot_; }

 private:
  ObjectPin slot_;
  T* value_ = nullptr;
};

}  // namespace rex::kernel
