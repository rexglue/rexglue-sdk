/**
 * @file        kernel/object/object_table.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/kernel/object/kernel_object.h>
#include <rex/kernel/object/object_block.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/xam_object.h>
#include <rex/kernel/types/status.h>
#include <rex/logging/log_category.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::kernel {

struct AddOptions {
  std::string_view name; /**< The name to register, empty for none. */
  ObjectBlock block;
};

/**
 * Owns every kernel object and maps handle values to them. Handles are 0xF8000000 plus 4 times
 * the slot. The pseudo handle 0xFFFFFFFF translates to 0 and 0xFFFFFFFE to the calling thread.
 */
REX_DECLARE_INTERFACE(IObjectTable, 1)
  virtual ~IObjectTable() = default;

  /**
   * Takes ownership of a `KernelObject` and gives it its first handle.
   *
   * @param options The name to register and the guest block that backs the object.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Add(std::unique_ptr<KernelObject> object,
                                             const AddOptions& options) = 0;

  /**
   * Adds a `KernelObject` under the name of the object attributes, with a guest block that holds
   * the handle when block_size is not 0.
   *
   * @param attributes_ptr The guest address of an X_OBJECT_ATTRIBUTES, 0 for no name.
   * @param block_size The size of the guest object body, 0 for no block.
   * @return The handle, or X_STATUS_NO_MEMORY when the block cannot be allocated.
   */
  [[nodiscard]] virtual Result<X_HANDLE> AddWithBlock(std::unique_ptr<KernelObject> object,
                                                      u32 attributes_ptr, u32 block_size) = 0;

  /**
   * Allocates an object block that the table frees with the object.
   *
   * @param type The type of the object, which selects the exported type object the header
   *        points at.
   * @param heap The heap for the header and body.
   * @return The block with owned set, address 0 when the allocation failed.
   */
  [[nodiscard]] virtual ObjectBlock AllocateObjectBlock(u32 size, ObjectType type,
                                                        ObjectBlockHeap heap) = 0;

  /**
   * Creates an event with a guest KEVENT block and gives it its first handle.
   *
   * @param manual_reset True for a notification event, false for a synchronization event.
   * @param initial_state True when the event starts signaled.
   * @return The handle, or X_STATUS_NO_MEMORY when the block cannot be allocated.
   */
  [[nodiscard]] virtual Result<X_HANDLE> AddEvent(bool manual_reset, bool initial_state) = 0;

  /**
   * Creates an object around a host state XAM owns and gives it its first handle.
   *
   * @param body The host state. The table destroys it with the object.
   * @param options The name to register and the guest block that backs the object.
   */
  [[nodiscard]] virtual Result<X_HANDLE> AddXAMObject(std::unique_ptr<IXAMObject> body,
                                                      const AddOptions& options) = 0;

  /**
   * Creates a file object over an opened file or directory and gives it its first handle.
   *
   * @param synchronous True when the file was opened for synchronous IO.
   */
  [[nodiscard]] virtual Result<X_HANDLE> AddFile(std::unique_ptr<filesystem::IFile> file,
                                                 bool synchronous) = 0;

  /**
   * Opens an object again through a handle. The new handle has its own value and is closed once.
   *
   * @param handle The existing handle, pseudo handles are translated.
   * @return The new handle, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Duplicate(X_HANDLE handle) = 0;

  /**
   * Opens a pinned object. The new handle has its own value and is closed once.
   *
   * @param object A pin on the object.
   * @return The new handle, or X_STATUS_INVALID_HANDLE when the object has left the table.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Open(const ObjectPin& object) = 0;

  /**
   * Frees a handle. The object is destroyed once it has no handle and no pointer reference.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return Ok, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<void> Close(X_HANDLE handle) = 0;

  /**
   * Adds a pointer reference, which keeps the object after its last handle is closed.
   *
   * @param object A pin on the object.
   * @return Ok, or X_STATUS_INVALID_HANDLE when the object has left the table.
   */
  [[nodiscard]] virtual Result<void> Reference(const ObjectPin& object) = 0;

  /**
   * Removes a pointer reference and destroys the object when it has no handle left.
   *
   * @param object A pin on the object.
   * @return Ok, or X_STATUS_INVALID_PARAMETER when the object has no pointer reference.
   */
  [[nodiscard]] virtual Result<void> Dereference(const ObjectPin& object) = 0;

  /**
   * Pins the object whose guest block starts at an address, whether or not it has a handle.
   *
   * @param address The guest address of the object body.
   * @return The pin, empty when no object is registered at the address.
   */
  [[nodiscard]] virtual ObjectPin GetByAddress(u32 address) = 0;

  /**
   * Looks a handle up and pins its `KernelObject`.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid.
   */
  [[nodiscard]] virtual ObjectPin Get(X_HANDLE handle) = 0;

  /** Pins every `KernelObject`, once per `KernelObject`. */
  [[nodiscard]] virtual std::vector<ObjectPin> GetAll() = 0;

  /**
   * Registers a name for a handle.
   *
   * @param name The name, compared case insensitively.
   * @return Ok, or X_STATUS_OBJECT_NAME_COLLISION.
   */
  [[nodiscard]] virtual Result<void> AddName(std::string_view name, X_HANDLE handle) = 0;

  /**
   * Opens the object that carries a name. The new handle has its own value and is closed once.
   *
   * @return The new handle, or X_STATUS_OBJECT_NAME_NOT_FOUND.
   */
  [[nodiscard]] virtual Result<X_HANDLE> OpenByName(std::string_view name) = 0;

  /**
   * Finds the handle of the object behind a guest dispatch header, and creates the object on first
   * use for an event, mutant or semaphore header.
   *
   * @param address The guest address of the dispatch header.
   * @param as_type The dispatcher object type to create, -1 for the type in the header.
   * @return The handle, empty for a null address or a header type the table cannot create.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> ResolveHandle(u32 address, i32 as_type) = 0;

  /**
   * Finds a handle of the object registered at a guest address and creates nothing.
   *
   * @param address The guest address of the dispatch header.
   * @return A handle of the object, empty when nothing is registered at the address.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> FindHandle(u32 address) = 0;

  /**
   * Cancels and closes the object registered by guest address, such as a timer. Does nothing when
   * no object is registered there.
   *
   * @param address The guest address of the dispatch header.
   */
  virtual void ForgetAddress(u32 address) = 0;

  /**
   * Constructs an object in place and gives it its first handle.
   *
   * @param options The name to register and the guest block that backs the object.
   */
  template <typename T, typename... Args>
  [[nodiscard]] Result<X_HANDLE> Emplace(const AddOptions& options, Args&&... args) {
    if constexpr (XAMObjectClass<T>) {
      return AddXAMObject(std::make_unique<T>(std::forward<Args>(args)...), options);
    } else {
      return Add(MakeObject<T>(std::forward<Args>(args)...), options);
    }
  }

  /**
   * Constructs an object behind a new guest block named by object attributes.
   *
   * @param attributes_ptr The guest address of an X_OBJECT_ATTRIBUTES, 0 for no name.
   * @param block_size The size of the guest object body, 0 for no block.
   * @return The handle, or X_STATUS_NO_MEMORY.
   */
  template <typename T, typename... Args>
  [[nodiscard]] Result<X_HANDLE> CreateObject(u32 attributes_ptr, u32 block_size, Args&&... args) {
    return AddWithBlock(MakeObject<T>(std::forward<Args>(args)...), attributes_ptr, block_size);
  }

  /**
   * Looks a handle up and pins its `KernelObject` when it holds a T.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid or the type differs.
   */
  template <typename T>
  [[nodiscard]] Pinned<T> Get(X_HANDLE handle) {
    return Pinned<T>(Get(handle));
  }

  /** Pins every `KernelObject` that holds a T, once per `KernelObject`. */
  template <typename T>
  [[nodiscard]] std::vector<Pinned<T>> All() {
    std::vector<Pinned<T>> results;
    for (ObjectPin& slot : GetAll()) {
      Pinned<T> pin(std::move(slot));
      if (pin) {
        results.push_back(std::move(pin));
      }
    }
    return results;
  }

  /**
   * Resolves a guest dispatch header and pins its `KernelObject` when it holds a T.
   *
   * @param address The guest address of the dispatch header.
   * @param as_type The dispatcher object type to create, -1 for the type in the header.
   * @return The pin, empty when the header cannot be resolved or the type differs.
   */
  template <typename T>
  [[nodiscard]] Pinned<T> Resolve(u32 address, i32 as_type = -1) {
    const std::optional<X_HANDLE> handle = ResolveHandle(address, as_type);
    return handle ? Get<T>(*handle) : Pinned<T>();
  }
};

/**
 * Closes a handle the caller owns and logs a close that fails, for teardown and error unwinding.
 *
 * @param table The table that issued the handle.
 */
inline void CloseOwned(IObjectTable& table, X_HANDLE handle) noexcept {
  log::NoThrow(log::system, [&] {
    if (auto closed = table.Close(handle); !closed) {
      REXSYS_WARN("Handle {:08X} did not close, {}", handle, closed.error().message);
    }
  });
}

}  // namespace rex::kernel
