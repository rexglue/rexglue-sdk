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

#include "xam/xam_state.h"
#include "xam/enumerator_object.h"

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/object_block.h>
#include <rex/kernel/object/object_table.h>

namespace rex::xam {

EnumeratorObject::EnumeratorObject(size_t items_per_enumerate_in, size_t item_size_in,
                                   EnumeratorWriter write_items_in)
    : items_per_enumerate(items_per_enumerate_in),
      item_size(item_size_in),
      write_items(std::move(write_items_in)) {}

StaticItemList::StaticItemList(size_t items_per_enumerate, size_t item_size)
    : items_per_enumerate_(items_per_enumerate), item_size_(item_size) {}

u8* StaticItemList::Append() {
  size_t offset = buffer_.size();
  buffer_.resize(offset + item_size_);
  item_count_++;
  return const_cast<u8*>(&buffer_.data()[offset]);
}

u32 StaticItemList::Write(u8* buffer_data, u32* written_count) {
  size_t count = std::min(item_count_ - current_item_, items_per_enumerate_);
  if (!count) {
    return kernel::X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size_;
  size_t offset = current_item_ * item_size_;
  std::memcpy(buffer_data, buffer_.data() + offset, size);

  current_item_ += count;

  if (written_count) {
    *written_count = static_cast<u32>(count);
  }

  return kernel::X_ERROR_SUCCESS;
}

kernel::X_STATUS CreateEnumerator(kernel::X_HANDLE* out_handle, const EnumeratorInit& init,
                                  EnumeratorObject&& enumerator, u32 extra_size,
                                  void** extra_buffer) {
  kernel::ObjectBlock block = CurrentTitle().objects.AllocateObjectBlock(
      sizeof(X_KENUMERATOR) + extra_size, kernel::ObjectBlockHeap::kWorkspace);
  if (!block.address) {
    return kernel::X_STATUS_NO_MEMORY;
  }
  auto* native_object = CurrentTitle().membase.TranslateVirtual<u8*>(block.address);
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
  Result<kernel::X_HANDLE> added = CurrentTitle().objects.Emplace<EnumeratorObject>(
      kernel::AddOptions{{}, block}, std::move(enumerator));
  if (added) {
    *out_handle = *added;
  }
  return kernel::ToXStatus(added);
}

}  // namespace rex::xam
