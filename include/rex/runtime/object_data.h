/**
 * @file        runtime/object_data.h
 * @brief       Object table entry data, records and transient pins
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

#include <rex/runtime/pin_count.h>
#include <rex/runtime/xenumerator.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xfile.h>
#include <rex/runtime/xiocompletion.h>
#include <rex/runtime/xmodule.h>
#include <rex/runtime/xmutant.h>
#include <rex/runtime/xnotifylistener.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xsemaphore.h>
#include <rex/runtime/xsocket.h>
#include <rex/runtime/xsymboliclink.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtimer.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** A guest block that backs an object, with whether the table owns and frees it. */
struct GuestBlock {
  u32 address = 0;
  bool owned = false;
};

/**
 * Allocates a guest block for an object, with its X_OBJECT_HEADER and X_OBJECT_TYPE.
 *
 * @param memory The guest memory.
 * @param size The size of the object body.
 * @return The block with owned set, address 0 when the allocation failed.
 */
GuestBlock AllocateGuestBlock(runtime::Memory* memory, u32 size);

/**
 * Frees a block made by AllocateGuestBlock, with its X_OBJECT_TYPE.
 *
 * @param memory The guest memory.
 * @param address The address of the object body, 0 for none.
 */
void FreeGuestBlock(runtime::Memory* memory, u32 address);

/** The data a table record owns, 1 alternative per ObjectKind in enumerator order. */
using ObjectData = std::variant<EventObject, SemaphoreObject, MutantObject, TimerObject, FileObject,
                                SocketObject, NotifyListenerObject, IOCompletionObject,
                                EnumeratorObject, SymbolicLinkObject, ModuleObject, ThreadRef>;

/** One kernel object, owned by the table and shared by every handle that names it. */
struct ObjectRecord {
  /**
   * @tparam T The alternative of ObjectData to construct.
   * @tparam Args The constructor argument types of T.
   * @param tag Selects T.
   * @param args The constructor arguments of T.
   */
  template <typename T, typename... Args>
  explicit ObjectRecord(std::in_place_type_t<T> tag, Args&&... args)
      : data(tag, std::forward<Args>(args)...) {}
  ~ObjectRecord();

  /**
   * Returns the kind of the data.
   *
   * @return The kind.
   */
  [[nodiscard]] ObjectKind kind() const { return static_cast<ObjectKind>(data.index()); }

  ObjectData data;
  std::string name;
  std::vector<X_HANDLE> handles;
  u32 guest_ptr = 0;
  bool owns_guest_block = false;
  runtime::Memory* memory = nullptr;
  PinCount pins;
};

/** A transient pin on a record. Never stored. Move only. */
class PinnedSlot {
 public:
  PinnedSlot() = default;
  /**
   * Adopts a pin already taken on a record.
   *
   * @param record The pinned record.
   * @param handle The handle the pin was resolved from.
   */
  PinnedSlot(ObjectRecord* record, X_HANDLE handle) noexcept : record_(record), handle_(handle) {}

  /**
   * Takes over the pin of another slot.
   *
   * @param other The slot left empty.
   */
  PinnedSlot(PinnedSlot&& other) noexcept : record_(other.record_), handle_(other.handle_) {
    other.record_ = nullptr;
  }
  PinnedSlot& operator=(PinnedSlot&& other) noexcept {
    if (this != &other) {
      Reset();
      record_ = other.record_;
      handle_ = other.handle_;
      other.record_ = nullptr;
    }
    return *this;
  }
  PinnedSlot(const PinnedSlot&) = delete;
  PinnedSlot& operator=(const PinnedSlot&) = delete;
  ~PinnedSlot() { Reset(); }

  /**
   * Tests whether the pin holds a record.
   *
   * @return True when it does.
   */
  explicit operator bool() const noexcept { return record_ != nullptr; }

  /**
   * Returns the kind of the pinned record.
   *
   * @return The kind.
   */
  [[nodiscard]] ObjectKind kind() const { return record_->kind(); }

  /**
   * Returns the data of the pinned record.
   *
   * @return The data.
   */
  [[nodiscard]] ObjectData& data() const { return record_->data; }

  /**
   * Returns the record.
   *
   * @return The record.
   */
  [[nodiscard]] ObjectRecord& record() const { return *record_; }

  /**
   * Returns the handle value the pin was looked up by.
   *
   * @return The handle, with pseudo handles already translated.
   */
  [[nodiscard]] X_HANDLE handle() const noexcept { return handle_; }

  /**
   * Returns the guest block address of the object.
   *
   * @return The address, 0 when the object has no guest block.
   */
  [[nodiscard]] u32 guest_object() const { return record_->guest_ptr; }

  /** Drops the pin. */
  void Reset() noexcept {
    if (record_ != nullptr) {
      if (record_->pins.Unpin()) {
        delete record_;
      }
      record_ = nullptr;
    }
  }

 private:
  ObjectRecord* record_ = nullptr;
  X_HANDLE handle_ = 0;
};

/**
 * A pin that exposes the object struct of a known kind.
 *
 * @tparam T The object struct the record is expected to hold.
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
      value_ = std::get_if<T>(&slot_.data());
      if (value_ == nullptr) {
        slot_.Reset();
      }
    }
  }

  /**
   * Tests whether the pin holds an object of the expected kind.
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
  [[nodiscard]] X_HANDLE handle() const noexcept { return slot_.handle(); }

  /**
   * Returns the guest block address of the object.
   *
   * @return The address, 0 when the object has no guest block.
   */
  [[nodiscard]] u32 guest_object() const { return slot_.guest_object(); }

  /**
   * Returns the underlying pin.
   *
   * @return The pin.
   */
  [[nodiscard]] const PinnedSlot& slot() const noexcept { return slot_; }

 private:
  PinnedSlot slot_;
  T* value_ = nullptr;
};

}  // namespace rex::runtime
