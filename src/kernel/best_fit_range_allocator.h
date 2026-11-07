/**
 * @file        kernel/best_fit_range_allocator.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <map>
#include <optional>
#include <set>
#include <utility>

#include <rex/types.h>
#include <rex/utility.h>

namespace rex::kernel {

/**
 * Hands out blocks of an offset range, taking the smallest free range that fits. It keeps all of
 * its bookkeeping in host memory, so it never touches the memory its ranges cover. Callers lock it.
 */
class BestFitRangeAllocator : public NonCopyable {
 public:
  /** Size and alignment of every block, in bytes. */
  static constexpr u32 kGranule = 16;

  /**
   * @param size The size of the range in bytes, rounded down to kGranule.
   */
  explicit BestFitRangeAllocator(u32 size);

  /**
   * Takes a block from the smallest free range that holds it.
   *
   * @param size The size in bytes, rounded up to kGranule and at least kGranule.
   * @return The offset of the block, a multiple of kGranule, or nothing when no free range fits.
   */
  [[nodiscard]] std::optional<u32> Allocate(u32 size);

  /**
   * Returns a block and joins it with the free ranges next to it.
   *
   * @param offset An offset from Allocate.
   * @return The size of the block in bytes, or nothing when the offset is not a live block.
   */
  std::optional<u32> Free(u32 offset);

  /** Returns the size of the range in bytes. */
  [[nodiscard]] u32 GetSize() const { return size_; }

  /** Returns the bytes held by live blocks, counted in kGranule steps. */
  [[nodiscard]] u32 GetAllocated() const { return allocated_; }

  /** Returns the number of live blocks. */
  [[nodiscard]] size_t GetBlockCount() const { return blocks_.size(); }

  /** Returns the number of free ranges. */
  [[nodiscard]] size_t GetFreeRangeCount() const { return free_by_address_.size(); }

 private:
  void AddFree(u32 offset, u32 size);
  void RemoveFree(std::map<u32, u32>::iterator it);

  u32 size_;
  u32 allocated_ = 0;
  std::map<u32, u32> free_by_address_;         /**< Offset to size of each free range. */
  std::set<std::pair<u32, u32>> free_by_size_; /**< Size and offset of each free range. */
  std::map<u32, u32> blocks_;                  /**< Offset to size of each live block. */
};

}  // namespace rex::kernel
