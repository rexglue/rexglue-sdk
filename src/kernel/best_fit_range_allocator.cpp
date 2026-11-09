/**
 * @file        kernel/best_fit_range_allocator.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "kernel/best_fit_range_allocator.h"

#include <algorithm>
#include <iterator>

namespace rex::kernel {

BestFitRangeAllocator::BestFitRangeAllocator(u32 size) : size_(size & ~(kGranule - 1)) {
  if (size_) {
    AddFree(0, size_);
  }
}

void BestFitRangeAllocator::AddFree(u32 offset, u32 size) {
  free_by_address_.emplace(offset, size);
  free_by_size_.emplace(size, offset);
}

void BestFitRangeAllocator::RemoveFree(const std::map<u32, u32>::iterator& it) {
  free_by_size_.erase({it->second, it->first});
  free_by_address_.erase(it);
}

std::optional<u32> BestFitRangeAllocator::Allocate(u32 size) {
  size = std::max(size, 1u);
  if (size > size_) {
    return std::nullopt;
  }
  size = (size + kGranule - 1) & ~(kGranule - 1);
  const auto best = free_by_size_.lower_bound({size, 0});
  if (best == free_by_size_.end()) {
    return std::nullopt;
  }
  const u32 offset = best->second;
  const u32 range_size = best->first;
  RemoveFree(free_by_address_.find(offset));
  if (range_size > size) {
    AddFree(offset + size, range_size - size);
  }
  blocks_.emplace(offset, size);
  allocated_ += size;
  return offset;
}

std::optional<u32> BestFitRangeAllocator::Free(u32 offset) {
  const auto block = blocks_.find(offset);
  if (block == blocks_.end()) {
    return std::nullopt;
  }
  const u32 size = block->second;
  blocks_.erase(block);
  allocated_ -= size;

  u32 start = offset;
  u32 length = size;
  const auto next = free_by_address_.find(offset + size);
  if (next != free_by_address_.end()) {
    length += next->second;
    RemoveFree(next);
  }
  auto after = free_by_address_.lower_bound(offset);
  if (after != free_by_address_.begin()) {
    const auto previous = std::prev(after);
    if (previous->first + previous->second == offset) {
      start = previous->first;
      length += previous->second;
      RemoveFree(previous);
    }
  }
  AddFree(start, length);
  return size;
}

}  // namespace rex::kernel
