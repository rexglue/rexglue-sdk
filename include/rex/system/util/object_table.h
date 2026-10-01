#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/string/key.h>
#include <rex/system/object_data.h>
#include <rex/system/xtypes.h>

namespace rex::system {

/** Placement options for a new table record. */
struct AddOptions {
  std::string_view name;
  GuestBlock block;
};

}  // namespace rex::system

namespace rex::system::util {

/** Owns every kernel object and maps handle values to them. */
class ObjectTable {
 public:
  /**
   * @param memory The guest memory that owned guest blocks are freed from, null in tests.
   */
  explicit ObjectTable(memory::Memory* memory);
  ~ObjectTable();

  /** Closes every handle and destroys every object. */
  void Reset();

  /**
   * Constructs an object in place and gives it its first handle with a count of 1.
   *
   * @tparam T The object struct to construct.
   * @tparam Args The constructor argument types of T.
   * @param out_handle Receives the handle, may be null.
   * @param options The name to register and the guest block that backs the object.
   * @param args The constructor arguments of T.
   * @return X_STATUS_SUCCESS, or X_STATUS_NO_MEMORY when the table cannot grow.
   */
  template <typename T, typename... Args>
  X_STATUS Emplace(X_HANDLE* out_handle, const AddOptions& options, Args&&... args) {
    return Insert(
        std::make_unique<ObjectRecord>(std::in_place_type<T>, std::forward<Args>(args)...), options,
        out_handle);
  }

  /**
   * Gives an object a new handle value with a count of 1.
   *
   * @param handle The existing handle, pseudo handles are translated.
   * @param out_handle Receives the new handle, may be null.
   * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE.
   */
  X_STATUS Duplicate(X_HANDLE handle, X_HANDLE* out_handle);

  /**
   * Adds 1 to the count of a handle.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE.
   */
  X_STATUS Retain(X_HANDLE handle);

  /**
   * Removes 1 from the count of a handle and frees the handle at 0.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE.
   */
  X_STATUS Close(X_HANDLE handle);

  /**
   * Frees a handle whatever its count.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE.
   */
  X_STATUS Remove(X_HANDLE handle);

  /**
   * Adds 1 to the count of the first handle of the pinned object.
   *
   * @param slot A pin on the object.
   * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE when the object has no handle.
   */
  X_STATUS RetainPrimary(const PinnedSlot& slot);

  /**
   * Removes 1 from the count of the first handle of the pinned object, as Close does.
   *
   * @param slot A pin on the object.
   * @return X_STATUS_SUCCESS, or X_STATUS_INVALID_HANDLE when the object has no handle.
   */
  X_STATUS ReleasePrimary(const PinnedSlot& slot);

  /**
   * Reads the first handle of a pinned object under the table lock.
   *
   * @param slot A pin on the object.
   * @return The first handle, 0 when the object has no handle.
   */
  X_HANDLE PrimaryHandle(const PinnedSlot& slot);

  /**
   * Looks a handle up and pins its record.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid.
   */
  PinnedSlot Get(X_HANDLE handle);

  /**
   * Looks a handle up and pins its record when it holds a T.
   *
   * @tparam T The expected object struct.
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid or the kind differs.
   */
  template <typename T>
  Pinned<T> Get(X_HANDLE handle) {
    return Pinned<T>(Get(handle));
  }

  /**
   * Pins every record that holds a T, once per record.
   *
   * @tparam T The object struct.
   * @return The pins.
   */
  template <typename T>
  std::vector<Pinned<T>> GetAll() {
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
   * Pins every record, once per record.
   *
   * @return The pins.
   */
  std::vector<PinnedSlot> GetAll();

  /**
   * Registers a name for a handle.
   *
   * @param name The name, compared case insensitively.
   * @param handle The handle.
   * @return X_STATUS_SUCCESS, or X_STATUS_OBJECT_NAME_COLLISION.
   */
  X_STATUS AddName(std::string_view name, X_HANDLE handle);

  /**
   * Removes a name.
   *
   * @param name The name.
   */
  void RemoveName(std::string_view name);

  /**
   * Finds a handle by name and adds 1 to the count of the first handle of its object.
   *
   * @param name The name.
   * @param out_handle Receives the handle, X_INVALID_HANDLE_VALUE when absent.
   * @return X_STATUS_SUCCESS, or X_STATUS_OBJECT_NAME_NOT_FOUND.
   */
  X_STATUS OpenByName(std::string_view name, X_HANDLE* out_handle);

 private:
  struct Slot {
    ObjectRecord* record = nullptr;
    u32 ref_count = 0;
  };

  X_STATUS Insert(std::unique_ptr<ObjectRecord> record, const AddOptions& options,
                  X_HANDLE* out_handle);
  X_HANDLE Translate(X_HANDLE handle) const;
  Slot* FindSlot(X_HANDLE handle);
  X_STATUS AllocateSlot(u32* out_slot);
  ObjectRecord* Detach(Slot& slot, X_HANDLE handle);
  static void Retire(ObjectRecord* record);

  static constexpr u32 GetHandleSlot(X_HANDLE handle) { return (handle - kHandleBase) >> 2; }

  memory::Memory* memory_;
  rex::platform::RankedSharedMutex mutex_{rex::platform::LockRank::Table};
  rex::platform::RankedMutex name_mutex_{rex::platform::LockRank::Name};
  std::vector<Slot> slots_;
  u32 last_free_slot_ = 0;
  std::unordered_map<ObjectRecord*, std::unique_ptr<ObjectRecord>> records_;
  std::unordered_map<string_key_case, X_HANDLE> names_;
};

}  // namespace rex::system::util
