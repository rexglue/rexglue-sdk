#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

namespace rex::system::util {

// List is designed for storing pointers to objects in the guest heap.
// All values in the list should be assumed to be in big endian.

// Pass LIST_ENTRY pointers.
// struct MYOBJ {
//   uint32_t stuff;
//   LIST_ENTRY list_entry; <-- pass this
//   ...
// }

class NativeList {
 public:
  NativeList();
  explicit NativeList(memory::Memory* memory);

  void Insert(uint32_t list_entry_ptr);
  bool IsQueued(uint32_t list_entry_ptr);
  void Remove(uint32_t list_entry_ptr);
  uint32_t Shift();
  bool HasPending();

  uint32_t head() const { return head_; }
  void set_head(uint32_t head) { head_ = head; }

  void set_memory(memory::Memory* mem) { memory_ = mem; }

 private:
  const uint32_t kInvalidPointer = 0xE0FE0FFF;

 private:
  memory::Memory* memory_ = nullptr;
  uint32_t head_;
};

}  // namespace rex::system::util
