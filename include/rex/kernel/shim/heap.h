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
#include <rex/export.h>
#include <rex/runtime/guest_allocator.h>
#include <rex/types.h>

namespace rex::runtime {
class Memory;
}  // namespace rex::runtime

namespace rex::kernel::shim {

/** The heap that serves the shim Rtl heap routines from guest memory. */
class REX_RUNTIME_API GuestHeap {
 public:
  /**
   * Creates the first heap segment.
   *
   * @param heap_size_bytes The size of the first segment.
   * @param memory The guest memory the segment is carved from.
   * @return True when the segment was created.
   */
  bool Init(u32 heap_size_bytes, rex::runtime::Memory* memory);

  /** Releases every segment. */
  void Shutdown();

  /**
   * Allocates a block.
   *
   * @param size The requested size in bytes.
   * @param zero True to clear the block.
   * @return The guest address of the block, or 0 when the heap is full.
   */
  u32 Alloc(u32 size, bool zero = false);

  /**
   * Frees a block.
   *
   * @param guest_addr The guest address returned by Alloc.
   */
  void Free(u32 guest_addr);

  /**
   * Returns the requested size of a block.
   *
   * @param guest_addr The guest address returned by Alloc.
   * @return The size the block was allocated with, or 0 when the address is not a block.
   */
  u32 GetSize(u32 guest_addr);

  /**
   * Resizes a block, moving it when it cannot grow in place.
   *
   * @param guest_addr The guest address returned by Alloc.
   * @param new_size The new size in bytes.
   * @param zero_new True to clear the added bytes.
   * @return The guest address of the resized block, or 0 when the heap is full.
   */
  u32 Realloc(u32 guest_addr, u32 new_size, bool zero_new = false);

 private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<rex::runtime::GuestAllocator>> segments_;
  u8* membase_ = nullptr;
  u32 initial_segment_size_ = 0;
  rex::runtime::Memory* memory_ = nullptr;

  void* GuestToHost(u32 guest_addr) const;
  u32 HostToGuest(void* host_ptr) const;
  bool AllocateSegmentLocked(u32 segment_size_bytes);
  u32 AllocLocked(u32 size, bool zero = false);
  rex::runtime::GuestAllocator* FindSegmentByGuestLocked(u32 guest_addr);
  rex::runtime::GuestAllocator::Diagnostics GetDiagnosticsLocked() const;
};

/**
 * Initializes the global shim heap.
 *
 * @param heap_size_mb The size of the first segment in MiB.
 * @param memory The guest memory the heap is carved from.
 * @return True when the heap was created.
 */
REX_RUNTIME_API bool InitHeap(u32 heap_size_mb, rex::runtime::Memory* memory);

/** Releases the global shim heap. */
REX_RUNTIME_API void ShutdownHeap();

}  // namespace rex::kernel::shim
