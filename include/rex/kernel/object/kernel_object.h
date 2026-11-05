/**
 * @file        kernel/object/kernel_object.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/io_completion_object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/mutant_object.h>
#include <rex/kernel/object/notify_listener_object.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/object_block.h>
#include <rex/kernel/object/semaphore_object.h>
#include <rex/kernel/object/symbolic_link_object.h>
#include <rex/kernel/object/timer_object.h>
#include <rex/kernel/object/xam_object.h>
#include <rex/kernel/types/status.h>
#include <rex/types.h>

namespace rex::kernel {

/** The data of a `KernelObject`, one alternative per ObjectType in enumerator order. */
using ObjectData = std::variant<EventObject, SemaphoreObject, MutantObject, TimerObject, FileObject,
                                NotifyListenerObject, IOCompletionObject, SymbolicLinkObject,
                                ModuleObject, ThreadRef, XAMObject>;

/** A kernel object, owned by the table and shared by its handles. */
struct KernelObject {
  /**
   * Constructs the object around an alternative of ObjectData.
   *
   * @tparam T The alternative of ObjectData to construct.
   * @param tag Selects T.
   */
  template <typename T, typename... Args>
  explicit KernelObject(std::in_place_type_t<T> tag, Args&&... args)
      : data(tag, std::forward<Args>(args)...) {}
  /** Frees the guest block the object owns. */
  REX_KERNEL_API ~KernelObject();

  [[nodiscard]] ObjectType GetType() const { return static_cast<ObjectType>(data.index()); }

  ObjectData data;
  std::string name;              /**< Empty for an unnamed object. */
  std::vector<X_HANDLE> handles; /**< One entry per open, in the order opened. */
  u32 pointer_refs = 0;          /**< Pointer references, which keep the object without a handle. */
  X_HANDLE anchor =
      0; /**< The handle that stands in while only pointer references hold the object. */
  u32 guest_ptr = 0;             /**< Address of the object body, 0 without a guest block. */
  bool owns_guest_block = false; /**< True when the destructor frees the block at guest_ptr. */
  kernel::Memory* memory = nullptr;
  WorkspaceHeap* workspace = nullptr; /**< Null for the system heap. */
  PinCount pins;                      /**< Keeps the object alive while a caller uses it. */
  /** Frees the object. The owning table sets it. */
  void (*destroy)(KernelObject* object) noexcept = nullptr;
};

/**
 * Makes a KernelObject around an alternative of ObjectData, or around a class XAM owns.
 *
 * @tparam T An alternative of ObjectData, or an XAMObjectClass.
 */
template <typename T, typename... Args>
[[nodiscard]] std::unique_ptr<KernelObject> MakeObject(Args&&... args) {
  if constexpr (XAMObjectClass<T>) {
    return std::make_unique<KernelObject>(std::in_place_type<XAMObject>,
                                          std::make_unique<T>(std::forward<Args>(args)...));
  } else {
    return std::make_unique<KernelObject>(std::in_place_type<T>, std::forward<Args>(args)...);
  }
}

}  // namespace rex::kernel
