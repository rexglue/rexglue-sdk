/**
 * @file        runtime/guest_allocator.h
 * @brief       An o1heap allocator over a range of guest memory
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

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>

struct O1HeapInstance;

namespace rex::runtime {

class Memory;

/** Profiler pool that the blocks of a guest allocator are reported to. */
enum class GuestPool : u8 {
  kHeap,      /**< The shim Rtl heap. */
  kWorkspace, /**< The title workspace. */
  kTitle,     /**< The heap of the title. */
  kCount,     /**< Number of pools, not a pool. */
};

/** Reserves and commits a range of guest memory and serves blocks from it. */
class REX_RUNTIME_API GuestAllocator : public NonCopyable {
  /** Limits construction to Create. */
  struct Key {};

 public:
  /** Usage counters of the allocator. */
  struct Diagnostics {
    u64 capacity;          /**< Total capacity in bytes. */
    u64 allocated;         /**< Bytes allocated. */
    u64 peak_allocated;    /**< Highest allocated byte count seen. */
    u64 peak_request_size; /**< Largest single request seen. */
    u64 oom_count;         /**< Allocations that failed for lack of space. */
  };

  /**
   * Wraps a committed range. Only Create can make the key.
   *
   * @param key The private construction key.
   * @param memory The guest memory.
   * @param base The guest address of the range.
   * @param size The byte count.
   * @param heap The o1heap instance over the range.
   */
  GuestAllocator(Key, Memory& memory, u32 base, u32 size, O1HeapInstance* heap);

  /**
   * Reserves and commits a range and starts the allocator over it.
   *
   * @param memory Guest memory, which must outlive the allocator.
   * @param size Range bytes.
   * @param alignment Range alignment in bytes.
   * @param physical True to take the range from the physical heap, false for the virtual heap.
   * @param page_size The page size that selects the heap, in bytes.
   * @param flags Allocation type flags added to reserve and commit.
   * @return An error when the range cannot be committed or the allocator cannot start.
   */
  [[nodiscard]] static Result<std::unique_ptr<GuestAllocator>> Create(Memory& memory, u32 size,
                                                                      u32 alignment, bool physical,
                                                                      u32 page_size, u32 flags);

  /** Releases the range. */
  ~GuestAllocator();

  /**
   * Returns the guest address of the range.
   *
   * @return The address.
   */
  [[nodiscard]] u32 GetBase() const;

  /**
   * Returns the range size.
   *
   * @return The size in bytes.
   */
  [[nodiscard]] u32 GetSize() const;

  /**
   * Reports whether an address is in the range.
   *
   * @param address The guest address.
   * @return True when the address is in the range.
   */
  [[nodiscard]] bool Contains(u32 address) const;

  /**
   * Allocates a block.
   *
   * @param size Bytes.
   * @return The host pointer of the block, or null when the range is exhausted.
   */
  [[nodiscard]] void* Allocate(u32 size);

  /**
   * Resizes a block, moving it when it cannot grow in place.
   *
   * @param block A pointer from Allocate.
   * @param size The new byte count.
   * @return The host pointer of the resized block, or null when the range is exhausted and the
   * block is left as it was.
   */
  [[nodiscard]] void* Reallocate(void* block, u32 size);

  /**
   * Frees a block.
   *
   * @param block A pointer from Allocate, null for none.
   */
  void Free(void* block);

  /**
   * Samples the usage counters.
   *
   * @return The counters.
   */
  [[nodiscard]] Diagnostics GetDiagnostics() const;

  /**
   * Checks the allocator invariants.
   *
   * @return True when they hold.
   */
  [[nodiscard]] bool InvariantsHold() const;

 private:
  Memory& memory_;
  u32 base_;
  u32 size_;
  O1HeapInstance* heap_;
  mutable std::mutex mutex_;
};

}  // namespace rex::runtime
