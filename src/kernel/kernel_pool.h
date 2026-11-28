/**
 * @file        kernel/kernel_pool.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/assert.h>
#include <rex/types.h>
#include <rex/memory.h>

namespace rex::kernel {

class GuestAddressSpace;

/** Guest layout of the 8 byte header in front of every small pool block. */
struct PoolBlockHeader {
  u8 previous_units; /**< Size in units of the block before this one in the page, 0 for the first.
                      */
  u8 units;          /**< Size in units of this block, header included. */
  u8 state;          /**< One of the kPoolState values. */
  u8 pad_3;          /**< Not written by allocate or free. */
  be_u32 tag;        /**< Pool tag of the allocation. */
};
REX_STATIC_ASSERT_SIZE(PoolBlockHeader, 8);

/**
 * The kernel pool, with a title and a system descriptor like the console allocator. A block of up
 * to 0xFD8 bytes comes from a shared 4 KiB page cut into 32 byte units. The title descriptor takes
 * its pages from the system heap counted against the title budget, and the system descriptor
 * takes them uncounted. A larger block takes whole pages. One lock guards both descriptors.
 */
class KernelPool {
 public:
  explicit KernelPool(GuestAddressSpace& memory);

  /**
   * Reports whether the page table was allocated. A pool that is not valid must not be used.
   *
   * @return True when the page table exists.
   */
  [[nodiscard]] bool IsValid() const { return small_pages_ != nullptr; }

  /**
   * Allocates a pool block.
   *
   * @param size The size in bytes.
   * @param tag The pool tag, written to the block header.
   * @param pool_type 0 for the pool of the calling thread's process, 1 for title, 2 for system.
   * @return The guest address, 0 for any other pool type or when no memory is left.
   */
  [[nodiscard]] u32 Allocate(u32 size, u32 tag, u32 pool_type);

  /**
   * Frees a pool block. The pool that owns the page takes it, whatever thread frees it.
   *
   * @param address The address Allocate returned.
   * @return False when the address is not a pool block.
   */
  bool Free(u32 address);

  /**
   * Tells whether an address is the start of an allocated pool block.
   *
   * @param address An address that Allocate could have returned.
   */
  [[nodiscard]] bool Owns(u32 address);

 private:
  static constexpr u32 kUnitSize = 32;
  static constexpr u32 kPageUnits = 0x80;
  static constexpr u32 kMaxLookasideUnits = 10;
  static constexpr u32 kLookasideDepth = 2;

  /** The freed blocks of one size that the next allocation of that size takes first. */
  struct Lookaside {
    std::array<u32, kLookasideDepth> blocks{};
    u32 count = 0;
  };

  struct Descriptor {
    bool title = false;
    /** Head and tail block of the free list of each size in units, 0 when empty. */
    std::array<u32, kPageUnits + 1> head{};
    std::array<u32, kPageUnits + 1> tail{};
    std::array<Lookaside, kMaxLookasideUnits + 1> lookaside;
  };

  struct LargeBlock {
    u8 descriptor;
    u32 size;
  };

  [[nodiscard]] PoolBlockHeader* Header(u32 block) const;
  [[nodiscard]] u32 NextBlock(u32 block) const;
  /** Zeroes the 8 bytes after the header, where a free block keeps its list links. */
  void ZeroLinks(u32 block) const;
  void SetNextPrevious(u32 block, u32 page, u32 units);
  void Insert(Descriptor& descriptor, u32 block, bool at_tail);
  void Remove(Descriptor& descriptor, u32 block);
  [[nodiscard]] u32 AllocateSmall(Descriptor& descriptor, u32 index, u32 units, u32 tag);
  [[nodiscard]] u32 AllocateLarge(Descriptor& descriptor, u32 index, u32 size);
  void FreeSmall(Descriptor& descriptor, u32 block, u32 page);

  GuestAddressSpace& memory_;
  std::mutex mutex_;
  std::array<Descriptor, 2> descriptors_;
  /** Descriptor index plus 1 of each 4 KiB page of the address space, 0 for a page of no pool. */
  std::unique_ptr<u8[], decltype(&std::free)> small_pages_;
  std::unordered_map<u32, LargeBlock> large_blocks_;
};

}  // namespace rex::kernel
