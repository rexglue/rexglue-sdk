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

#include <rex/system/xenumerator.h>

#include <rex/system/kernel_state.h>
#include <rex/system/object_data.h>
#include <rex/system/util/object_table.h>

namespace rex::system {

EnumeratorObject::EnumeratorObject(size_t items_per_enumerate_in, size_t item_size_in,
                                   EnumeratorWriter write_items_in)
    : items_per_enumerate(items_per_enumerate_in),
      item_size(item_size_in),
      write_items(std::move(write_items_in)) {}

StaticItems::StaticItems(size_t items_per_enumerate, size_t item_size)
    : items_per_enumerate_(items_per_enumerate), item_size_(item_size) {}

u8* StaticItems::Append() {
  size_t offset = buffer_.size();
  buffer_.resize(offset + item_size_);
  item_count_++;
  return const_cast<u8*>(&buffer_.data()[offset]);
}

u32 StaticItems::Write(u8* buffer_data, u32* written_count) {
  size_t count = std::min(item_count_ - current_item_, items_per_enumerate_);
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size_;
  size_t offset = current_item_ * item_size_;
  std::memcpy(buffer_data, buffer_.data() + offset, size);

  current_item_ += count;

  if (written_count) {
    *written_count = static_cast<u32>(count);
  }

  return X_ERROR_SUCCESS;
}

X_STATUS CreateEnumerator(KernelState* kernel_state, X_HANDLE* out_handle,
                          const EnumeratorInit& init, EnumeratorObject&& enumerator, u32 extra_size,
                          void** extra_buffer) {
  memory::Memory* memory = kernel_state->memory();
  GuestBlock block = AllocateGuestBlock(memory, sizeof(X_KENUMERATOR) + extra_size);
  if (!block.address) {
    return X_STATUS_NO_MEMORY;
  }
  auto* native_object = memory->TranslateVirtual<u8*>(block.address);
  auto* guest_object = reinterpret_cast<X_KENUMERATOR*>(native_object);
  guest_object->app_id = init.app_id;
  guest_object->open_message = init.open_message;
  guest_object->close_message = init.close_message;
  guest_object->user_index = init.user_index;
  guest_object->items_per_enumerate = static_cast<u32>(enumerator.items_per_enumerate);
  guest_object->flags = init.flags;
  if (extra_buffer) {
    *extra_buffer = &native_object[sizeof(X_KENUMERATOR)];
  }
  return kernel_state->object_table()->Emplace<EnumeratorObject>(out_handle, AddOptions{{}, block},
                                                                 std::move(enumerator));
}

}  // namespace rex::system
