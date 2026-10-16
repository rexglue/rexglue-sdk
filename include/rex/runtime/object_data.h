/**
 * @file        runtime/object_data.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/runtime/pin_count.h>
#include <rex/runtime/enumerator_object.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/file_object.h>
#include <rex/runtime/io_completion_object.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/mutant_object.h>
#include <rex/runtime/notify_listener_object.h>
#include <rex/runtime/object.h>
#include <rex/runtime/semaphore_object.h>
#include <rex/runtime/session_object.h>
#include <rex/runtime/socket_object.h>
#include <rex/runtime/symbolic_link_object.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/timer_object.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::runtime {

class WorkspaceHeap;

/** A guest block that backs an object, with whether the table owns and frees it. */
struct ObjectBlock {
  u32 address = 0;    /**< Guest address of the object body, after the X_OBJECT_HEADER. */
  bool owned = false; /**< True when the table frees the block when the object dies. */
  /** The workspace that holds the block, null for the system heap. */
  WorkspaceHeap* workspace = nullptr;
};

/**
 * Allocates a guest block for an object, with its X_OBJECT_HEADER and X_OBJECT_TYPE.
 *
 * @param memory The guest memory.
 * @param size The size of the object body.
 * @param workspace The workspace for the header and body, null for the system heap.
 * @return The block with owned set, address 0 when the allocation failed.
 */
REX_RUNTIME_API ObjectBlock AllocObjectBlock(runtime::Memory* memory, u32 size,
                                             WorkspaceHeap* workspace = nullptr);

/**
 * Frees a block made by AllocObjectBlock, with its X_OBJECT_TYPE.
 *
 * @param memory The guest memory.
 * @param address The address of the object body, 0 for none.
 * @param workspace The workspace AllocObjectBlock used, null for the system heap.
 */
REX_RUNTIME_API void FreeObjectBlock(runtime::Memory* memory, u32 address,
                                     WorkspaceHeap* workspace = nullptr);

/** The data an `KernelObject` owns, 1 alternative per ObjectKind in enumerator order. */
using ObjectData =
    std::variant<EventObject, SemaphoreObject, MutantObject, TimerObject, FileObject, SocketObject,
                 NotifyListenerObject, IOCompletionObject, EnumeratorObject, SymbolicLinkObject,
                 ModuleObject, ThreadRef, SessionObject>;

/** One kernel object, owned by the table and shared by every handle that names it. */
struct KernelObject {
  /**
   * @tparam T The alternative of ObjectData to construct.
   * @tparam Args The constructor argument types of T.
   * @param tag Selects T.
   * @param args The constructor arguments of T.
   */
  template <typename T, typename... Args>
  explicit KernelObject(std::in_place_type_t<T> tag, Args&&... args)
      : data(tag, std::forward<Args>(args)...) {}
  /** Frees the guest block the object owns. */
  REX_RUNTIME_API ~KernelObject();

  /**
   * Returns the data kind.
   *
   * @return The kind.
   */
  [[nodiscard]] ObjectKind GetKind() const { return static_cast<ObjectKind>(data.index()); }

  ObjectData data;  /**< Host state of the object, the alternative kind() reports. */
  std::string name; /**< Name the object was created with, empty for an unnamed object. */
  std::vector<X_HANDLE> handles; /**< Handles in the table that name this object. */
  u32 guest_ptr = 0; /**< Guest address of the object body, 0 when the object has no guest block. */
  bool owns_guest_block = false;     /**< True when the destructor frees the block at guest_ptr. */
  runtime::Memory* memory = nullptr; /**< Guest memory used to free the block at guest_ptr. */
  /** Workspace that holds the block at guest_ptr, null for the system heap. */
  WorkspaceHeap* workspace = nullptr;
  PinCount pins; /**< Transient pins that keep the object alive while a caller uses it. */
};

/** A transient pin on an `KernelObject`. Never stored. Move only. */
class PinnedSlot : public rex::MoveOnly {
 public:
  PinnedSlot() = default;
  /**
   * Takes a pin already taken on an `KernelObject`.
   *
   * @param object The pinned `KernelObject`.
   * @param handle The handle the pin was resolved from.
   */
  PinnedSlot(KernelObject* object, X_HANDLE handle) noexcept : object_(object), handle_(handle) {}

  /**
   * Takes over the pin of another slot.
   *
   * @param other The slot left empty.
   */
  PinnedSlot(PinnedSlot&& other) noexcept : object_(other.object_), handle_(other.handle_) {
    other.object_ = nullptr;
  }
  /**
   * Takes over the pin of another slot and releases the pin this slot held.
   *
   * @param other The slot left empty.
   * @return This slot.
   */
  PinnedSlot& operator=(PinnedSlot&& other) noexcept {
    if (this != &other) {
      Reset();
      object_ = other.object_;
      handle_ = other.handle_;
      other.object_ = nullptr;
    }
    return *this;
  }
  ~PinnedSlot() { Reset(); }

  /**
   * Reports whether the pin holds an `KernelObject`.
   *
   * @return True when it does.
   */
  explicit operator bool() const noexcept { return object_ != nullptr; }

  /**
   * Returns the pinned `KernelObject` kind.
   *
   * @return The kind.
   */
  [[nodiscard]] ObjectKind GetKind() const { return object_->GetKind(); }

  /**
   * Returns the data of the pinned `KernelObject`.
   *
   * @return The data.
   */
  [[nodiscard]] ObjectData& GetData() const { return object_->data; }

  /**
   * Returns the `KernelObject`.
   *
   * @return The `KernelObject`.
   */
  [[nodiscard]] KernelObject& GetKernelObject() const { return *object_; }

  /**
   * Returns the handle value the pin was looked up by.
   *
   * @return The handle, with pseudo handles already translated.
   */
  [[nodiscard]] X_HANDLE GetHandle() const noexcept { return handle_; }

  /**
   * Returns the guest block address of the object.
   *
   * @return The address, 0 when the object has no guest block.
   */
  [[nodiscard]] u32 GetGuestObject() const { return object_->guest_ptr; }

  /** Drops the pin. */
  void Reset() noexcept {
    if (object_ != nullptr) {
      if (object_->pins.Unpin()) {
        delete object_;
      }
      object_ = nullptr;
    }
  }

 private:
  KernelObject* object_ = nullptr;
  X_HANDLE handle_ = 0;
};

/**
 * A pin that exposes the object struct of a known kind.
 *
 * @tparam T The object struct the `KernelObject` is expected to hold.
 */
template <typename T>
class Pinned {
 public:
  Pinned() = default;
  /**
   * Views a pinned slot as kind T. The view is empty when the slot holds another kind.
   *
   * @param slot The pinned slot, moved in.
   */
  explicit Pinned(PinnedSlot slot) : slot_(std::move(slot)) {
    if (slot_) {
      value_ = std::get_if<T>(&slot_.GetData());
      if (value_ == nullptr) {
        slot_.Reset();
      }
    }
  }

  /**
   * Reports whether the pin holds an object of the expected kind.
   *
   * @return True when it does.
   */
  explicit operator bool() const noexcept { return value_ != nullptr; }
  T* operator->() const noexcept { return value_; }
  T& operator*() const noexcept { return *value_; }

  /**
   * Returns the handle value the pin was looked up by.
   *
   * @return The handle.
   */
  [[nodiscard]] X_HANDLE GetHandle() const noexcept { return slot_.GetHandle(); }

  /**
   * Returns the guest block address of the object.
   *
   * @return The address, 0 when the object has no guest block.
   */
  [[nodiscard]] u32 GetGuestObject() const { return slot_.GetGuestObject(); }

  /**
   * Returns the underlying pin.
   *
   * @return The pin.
   */
  [[nodiscard]] const PinnedSlot& GetSlot() const noexcept { return slot_; }

 private:
  PinnedSlot slot_;
  T* value_ = nullptr;
};

}  // namespace rex::runtime
