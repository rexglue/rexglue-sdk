/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>
#include <rex/move_only_function.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** Guest block of an enumerator, followed by the extra bytes of its creator. */
struct X_KENUMERATOR {
  be_u32 app_id;              /**< Id of the XAM app the messages go to. */
  be_u32 open_message;        /**< Message sent to the app to open the enumerator. */
  be_u32 close_message;       /**< Message sent to the app to close the enumerator. */
  be_u32 user_index;          /**< Index of the user the enumerator belongs to. */
  be_u32 items_per_enumerate; /**< Items an enumerate call returns. */
  be_u32 flags;               /**< Set from EnumeratorInit::flags, 0 at every call. */
};
REX_STATIC_ASSERT_SIZE(X_KENUMERATOR, 0x18);

/** Extra block after `X_KENUMERATOR` for the content aggregate enumerator. */
struct X_KENUMERATOR_CONTENT_AGGREGATE {
  be_u32 magic;  /**< `kStashSignature`. */
  be_u32 handle; /**< Handle of the enumerator. */
};

/**
 * Writes the next batch of items.
 *
 * @param buffer_ptr The guest address of the output buffer.
 * @param buffer_data The host address of the output buffer.
 * @param written_count Receives the number of items written.
 * @return The guest error code.
 */
using EnumeratorWriter =
    rex::MoveOnlyFunction<u32(u32 buffer_ptr, u8* buffer_data, u32* written_count)>;

/** The host state of an enumerator object. */
struct EnumeratorObject {
  /**
   * @param items_per_enumerate_in The items returned by 1 enumerate call.
   * @param item_size_in The size of 1 item.
   * @param write_items_in The callback that writes the next batch.
   */
  REX_RUNTIME_API EnumeratorObject(size_t items_per_enumerate_in, size_t item_size_in,
                                   EnumeratorWriter write_items_in);

  /** Maximum items an enumerate call writes, copied to X_KENUMERATOR. */
  size_t items_per_enumerate;
  size_t item_size; /**< Size in bytes of 1 item in the output buffer. */
  /** Called by the enumerate message to write the next batch into the guest buffer. */
  EnumeratorWriter write_items;
};

/** A fixed list of items that an enumerator hands out in batches. */
class REX_RUNTIME_API StaticItems {
 public:
  /**
   * @param items_per_enumerate The items returned by 1 Write call.
   * @param item_size The size of 1 item.
   */
  StaticItems(size_t items_per_enumerate, size_t item_size);

  /**
   * Appends a zeroed item.
   *
   * @return The host address of the item, invalidated by the next Append.
   */
  u8* Append();

  /**
   * Appends a zeroed item as a given type.
   *
   * @tparam T The item type.
   * @return The item, invalidated by the next Append.
   */
  template <typename T>
  T* Append() {
    return reinterpret_cast<T*>(Append());
  }

  /**
   * Returns the number of items appended.
   *
   * @return The count.
   */
  [[nodiscard]] size_t GetItemCount() const { return item_count_; }

  /**
   * Writes the next batch of items.
   *
   * @param buffer_data The host address of the output buffer.
   * @param written_count Receives the number of items written, may be null.
   * @return X_ERROR_NO_MORE_FILES when no items remain, X_ERROR_SUCCESS otherwise.
   */
  u32 Write(u8* buffer_data, u32* written_count);

 private:
  size_t items_per_enumerate_;
  size_t item_size_;
  size_t item_count_ = 0;
  size_t current_item_ = 0;
  std::vector<u8> buffer_;
};

/** The fields a guest enumerator reports to its consumers. */
struct EnumeratorInit {
  u32 user_index; /**< Stored in X_KENUMERATOR::user_index, the user the enumerator belongs to. */
  u32 app_id;     /**< Stored in X_KENUMERATOR::app_id, the XAM app that receives the messages. */
  /** Stored in X_KENUMERATOR::open_message, the message that opens the enumerator. */
  u32 open_message;
  /** Stored in X_KENUMERATOR::close_message, the message that closes the enumerator. */
  u32 close_message;
  u32 flags; /**< Stored in X_KENUMERATOR::flags. */
};

/**
 * Creates an enumerator object with its guest X_KENUMERATOR block.
 *
 * @param runtime The runtime the call runs in.
 * @param out_handle Receives the handle.
 * @param init The X_KENUMERATOR fields.
 * @param enumerator The enumerator state, moved into the table.
 * @param extra_size The bytes to reserve after the X_KENUMERATOR.
 * @param extra_buffer Receives the host address of the reserved bytes, may be null.
 * @return X_STATUS_SUCCESS, or X_STATUS_NO_MEMORY.
 */
REX_RUNTIME_API X_STATUS CreateEnumerator(Runtime& runtime, X_HANDLE* out_handle,
                                          const EnumeratorInit& init, EnumeratorObject&& enumerator,
                                          u32 extra_size = 0, void** extra_buffer = nullptr);

}  // namespace rex::runtime
