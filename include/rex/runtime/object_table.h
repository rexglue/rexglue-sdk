/**
 * @file        runtime/object_table.h
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

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/runtime/object_data.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::runtime {

/** Placement options for a new `ObjectRecord`. */
struct AddOptions {
  std::string_view name; /**< The name to register, empty for none. */
  GuestBlock block;      /**< The guest block that backs the object. */
};

/**
 * Owns every kernel object and maps handle values to them. Handles are 0xF8000000 plus 4 times
 * the slot. The pseudo handle 0xFFFFFFFF translates to 0 and 0xFFFFFFFE to the calling thread.
 */
class IObjectTable {
 public:
  REX_INTERFACE(IObjectTable, "rex.runtime.object_table", 1);
  virtual ~IObjectTable() = default;

  /**
   * Takes ownership of an `ObjectRecord` and gives it its first handle with a count of 1.
   *
   * @param record The `ObjectRecord`.
   * @param options The name to register and the guest block that backs the object.
   * @return The handle.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Add(std::unique_ptr<ObjectRecord> record,
                                             const AddOptions& options) = 0;

  /**
   * Allocates a guest block when block_size is not 0, adds the `ObjectRecord` under the name of the
   * object attributes, and stashes the handle in the block.
   *
   * @param record The `ObjectRecord`.
   * @param attributes_ptr The guest address of an X_OBJECT_ATTRIBUTES, 0 for no name.
   * @param block_size The size of the guest object body, 0 for no block.
   * @return The handle, or X_STATUS_NO_MEMORY when the block cannot be allocated.
   */
  [[nodiscard]] virtual Result<X_HANDLE> AddWithBlock(std::unique_ptr<ObjectRecord> record,
                                                      u32 attributes_ptr, u32 block_size) = 0;

  /**
   * Gives an object a new handle value with a count of 1.
   *
   * @param handle The existing handle, pseudo handles are translated.
   * @return The new handle, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Duplicate(X_HANDLE handle) = 0;

  /**
   * Adds 1 to the count of a handle.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return Ok, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<void> Retain(X_HANDLE handle) = 0;

  /**
   * Removes 1 from the count of a handle and frees the handle at 0.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return Ok, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<void> Close(X_HANDLE handle) = 0;

  /**
   * Frees a handle whatever its count.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return Ok, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<void> Remove(X_HANDLE handle) = 0;

  /**
   * Adds 1 to the count of the first handle of the pinned object.
   *
   * @param slot A pin on the object.
   * @return Ok, or X_STATUS_INVALID_HANDLE when the object has no handle.
   */
  [[nodiscard]] virtual Result<void> RetainPrimary(const PinnedSlot& slot) = 0;

  /**
   * Removes 1 from the count of the first handle of the pinned object, as Close does.
   *
   * @param slot A pin on the object.
   * @return Ok, or X_STATUS_INVALID_HANDLE when the object has no handle.
   */
  [[nodiscard]] virtual Result<void> ReleasePrimary(const PinnedSlot& slot) = 0;

  /**
   * Reads the first handle of a pinned object under the table lock.
   *
   * @param slot A pin on the object.
   * @return The first handle, empty when the object has no handle.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> PrimaryHandle(const PinnedSlot& slot) = 0;

  /**
   * Looks a handle up and pins its `ObjectRecord`.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid.
   */
  [[nodiscard]] virtual PinnedSlot Get(X_HANDLE handle) = 0;

  /**
   * Pins every `ObjectRecord`, once per `ObjectRecord`.
   *
   * @return The pins.
   */
  [[nodiscard]] virtual std::vector<PinnedSlot> GetAll() = 0;

  /**
   * Registers a name for a handle.
   *
   * @param name The name, compared case insensitively.
   * @param handle The handle.
   * @return Ok, or X_STATUS_OBJECT_NAME_COLLISION.
   */
  [[nodiscard]] virtual Result<void> AddName(std::string_view name, X_HANDLE handle) = 0;

  /**
   * Finds a handle by name and adds 1 to the count of the first handle of its object.
   *
   * @param name The name.
   * @return The handle, or X_STATUS_OBJECT_NAME_NOT_FOUND.
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
  [[nodiscard]] virtual std::optional<X_HANDLE> ResolveGuestObject(u32 address, i32 as_type) = 0;

  /**
   * Constructs an object in place and gives it its first handle with a count of 1.
   *
   * @tparam T The object struct to construct.
   * @tparam Args The constructor argument types of T.
   * @param options The name to register and the guest block that backs the object.
   * @param args The constructor arguments of T.
   * @return The handle.
   */
  template <typename T, typename... Args>
  [[nodiscard]] Result<X_HANDLE> Emplace(const AddOptions& options, Args&&... args) {
    return Add(std::make_unique<ObjectRecord>(std::in_place_type<T>, std::forward<Args>(args)...),
               options);
  }

  /**
   * Constructs an object behind a new guest block named by object attributes.
   *
   * @tparam T The object struct to construct.
   * @tparam Args The constructor argument types of T.
   * @param attributes_ptr The guest address of an X_OBJECT_ATTRIBUTES, 0 for no name.
   * @param block_size The size of the guest object body, 0 for no block.
   * @param args The constructor arguments of T.
   * @return The handle, or X_STATUS_NO_MEMORY.
   */
  template <typename T, typename... Args>
  [[nodiscard]] Result<X_HANDLE> CreateWithBlock(u32 attributes_ptr, u32 block_size,
                                                 Args&&... args) {
    return AddWithBlock(
        std::make_unique<ObjectRecord>(std::in_place_type<T>, std::forward<Args>(args)...),
        attributes_ptr, block_size);
  }

  /**
   * Looks a handle up and pins its `ObjectRecord` when it holds a T.
   *
   * @tparam T The expected object struct.
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid or the kind differs.
   */
  template <typename T>
  [[nodiscard]] Pinned<T> Get(X_HANDLE handle) {
    return Pinned<T>(Get(handle));
  }

  /**
   * Pins every `ObjectRecord` that holds a T, once per `ObjectRecord`.
   *
   * @tparam T The object struct.
   * @return The pins.
   */
  template <typename T>
  [[nodiscard]] std::vector<Pinned<T>> GetAll() {
    std::vector<Pinned<T>> results;
    for (PinnedSlot& slot : GetAll()) {
      Pinned<T> pin(std::move(slot));
      if (pin) {
        results.push_back(std::move(pin));
      }
    }
    return results;
  }

  /**
   * Resolves a guest dispatch header and pins its `ObjectRecord` when it holds a T.
   *
   * @tparam T The expected object struct.
   * @param address The guest address of the dispatch header.
   * @param as_type The dispatcher object type to create, -1 for the type in the header.
   * @return The pin, empty when the header cannot be resolved or the kind differs.
   */
  template <typename T>
  [[nodiscard]] Pinned<T> ResolveGuest(u32 address, i32 as_type = -1) {
    const std::optional<X_HANDLE> handle = ResolveGuestObject(address, as_type);
    return handle ? Get<T>(*handle) : Pinned<T>();
  }
};

}  // namespace rex::runtime
