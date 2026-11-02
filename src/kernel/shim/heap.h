/**
 * @file        kernel/shim/heap.h
 * @brief       Heap behind the shim Rtl heap routines
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include <rex/platform.h>
#include <rex/kernel/guest_allocator.h>
#include <rex/types.h>

namespace rex::kernel {
class Memory;
}  // namespace rex::kernel

namespace rex::kernel::shim {

/** The heap that serves the shim Rtl heap routines from guest memory. */
class GuestHeap {
 public:
  /**
   * Creates the first heap segment.
   *
   * @param heap_size_bytes The size of the first segment.
   * @param memory The guest memory the segment is carved from.
   */
  bool Init(u32 heap_size_bytes, rex::kernel::Memory* memory);

  void Shutdown();

  /**
   * Allocates a block.
   *
   * @return The guest address of the block, or 0 when the heap is full.
   */
  u32 Alloc(u32 size, bool zero = false);

  void Free(u32 guest_addr);

  /** Returns the requested size of a block, or 0xFFFFFFFF when the address is not a block. */
  u32 GetSize(u32 guest_addr);

  /**
   * Resizes a block, moving it when it cannot grow in place.
   *
   * @return The guest address of the resized block, or 0 when the heap is full.
   */
  u32 Realloc(u32 guest_addr, u32 new_size, bool zero_new = false);

 private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<rex::kernel::GuestAllocator>> segments_;
  u8* membase_ = nullptr;
  u32 initial_segment_size_ = 0;
  rex::kernel::Memory* memory_ = nullptr;

  void* GuestToHost(u32 guest_addr) const;
  u32 HostToGuest(void* host_ptr) const;
  bool AllocateSegmentLocked(u32 segment_size_bytes);
  u32 AllocLocked(u32 size, bool zero = false);
  rex::kernel::GuestAllocator* FindSegmentByGuestLocked(u32 guest_addr);
  rex::kernel::GuestAllocator::AllocatorStats GetStatsLocked() const;
};

/**
 * Initializes the global shim heap.
 *
 * @param heap_size_mb The size of the first segment in MiB.
 * @param memory The guest memory the heap is carved from.
 */
bool InitHeap(u32 heap_size_mb, rex::kernel::Memory* memory);

void ShutdownHeap();

}  // namespace rex::kernel::shim
