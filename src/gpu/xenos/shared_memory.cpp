/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/dbg.h>
#include "gpu/xenos/shared_memory.h"
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::gpu {

SharedMemory::SharedMemory(runtime::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(static_cast<u32>(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(u32 granularity_log2) {
  REX_ASSERT(granularity_log2 <= kBufferSizeLog2);
  REX_ASSERT(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      static_cast<size_t>(1) << (std::max(kBufferSizeLog2 - granularity_log2, static_cast<u32>(6)) -
                                 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  REX_ASSERT(global_watches_.empty());
  // No watches remain, so guest threads cannot reach the pools without the lock.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    REX_PROFILE_PLOT("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    REX_PROFILE_PLOT("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto lock = AcquireLock();

  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), static_cast<u64>(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), static_cast<u64>(0));
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto lock = AcquireLock();

  system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_;
}

void SharedMemory::ClearCache() {
  // GPU written data is kept, so the watches report invalidated by GPU.
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches remain, so guest threads cannot reach the pools without the lock.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  auto watch = std::make_unique<GlobalWatch>();
  watch->callback = callback;
  watch->callback_context = callback_context;
  GlobalWatch* raw = watch.get();

  auto lock = AcquireLock();
  global_watches_.push_back(std::move(watch));

  return reinterpret_cast<GlobalWatchHandle>(raw);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);
  std::unique_ptr<GlobalWatch> removed;
  {
    auto lock = AcquireLock();
    auto it = std::find_if(
        global_watches_.begin(), global_watches_.end(),
        [watch](const std::unique_ptr<GlobalWatch>& entry) { return entry.get() == watch; });
    REX_ASSERT(!(it == global_watches_.end()));
    if (it != global_watches_.end()) {
      removed = std::move(*it);
      global_watches_.erase(it);
    }
  }
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(
    const std::unique_lock<rex::platform::RankedMutex>&, u32 start, u32 length,
    WatchCallback callback, void* callback_context, void* callback_data, u64 callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  u32 watch_page_first = start >> page_size_log2_;
  u32 watch_page_last = (start + length - 1) >> page_size_log2_;
  u32 bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  u32 bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(
          std::make_unique_for_overwrite<WatchRange[]>(kWatchRangePoolSize));
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  WatchNode* node_previous = nullptr;
  for (u32 i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(
            std::make_unique_for_overwrite<WatchNode[]>(kWatchNodePoolSize));
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto lock = AcquireLock();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(u32 page_first, u32 page_last, bool invalidated_by_gpu) {
  auto lock = AcquireLock();
  FireWatchesLocked(page_first, page_last, invalidated_by_gpu);
}

void SharedMemory::FireWatchesLocked(u32 page_first, u32 page_last, bool invalidated_by_gpu) {
  u32 address_first = page_first << page_size_log2_;
  u32 address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  u32 bucket_first = address_first >> kWatchBucketSizeLog2;
  u32 bucket_last = address_last >> kWatchBucketSizeLog2;

  for (const auto& global_watch : global_watches_) {
    global_watch->callback(global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  for (u32 i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Fetch the next node first, since the callback breaks the links.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(range->callback_context, range->callback_data, range->callback_argument,
                        invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(u32 start, u32 length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  u32 end = start + length - 1;
  u32 page_first = start >> page_size_log2_;
  u32 page_last = end >> page_size_log2_;

  FireWatches(page_first, page_last, true);

  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange([[maybe_unused]] u32 offset_allocations,
                                                    [[maybe_unused]] u32 length_allocations) {
  REX_ASSERT_ALWAYS(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(u32 start, u32 length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  u32 last = start + length - 1;
  u32 valid_page_first = start >> page_size_log2_;
  u32 valid_page_last = last >> page_size_log2_;
  u32 valid_block_first = valid_page_first >> 6;
  u32 valid_block_last = valid_page_last >> 6;

  {
    auto lock = AcquireLock();

    for (u32 i = valid_block_first; i <= valid_block_last; ++i) {
      u64 valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((static_cast<u64>(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (static_cast<u64>(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      system_page_flags_valid_[i] |= valid_bits;
      u64& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
    }
  }

  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  u32 bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<u32, u32>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  std::vector<std::pair<u32, u32>> merged_ranges;
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    u32 start = ranges[i].first;
    u32 length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  REX_PROFILE_SCOPE("gpu");

  std::sort(
      merged_ranges.begin(), merged_ranges.end(),
      [](const std::pair<u32, u32>& a, const std::pair<u32, u32>& b) { return a.first < b.first; });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<u32, u32>& range_previous = merged_ranges[merged_write];
    const std::pair<u32, u32>& range_current = merged_ranges[i];
    u64 previous_end =
        static_cast<u64>(range_previous.first) + static_cast<u64>(range_previous.second);
    u64 current_start = static_cast<u64>(range_current.first);
    if (current_start <= previous_end) {
      u64 current_end = current_start + static_cast<u64>(range_current.second);
      if (current_end > previous_end) {
        range_previous.second =
            static_cast<u32>(current_end - static_cast<u64>(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<u32, u32>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](u32 page_start, u32 page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<u32, u32>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto lock = AcquireLock();
    for (const std::pair<u32, u32>& range : merged_ranges) {
      u32 page_first = range.first >> page_size_log2_;
      u32 page_last = (range.first + range.second - 1) >> page_size_log2_;
      u32 block_first = page_first >> 6;
      u32 block_last = page_last >> 6;
      u32 range_start = UINT32_MAX;
      for (u32 i = block_first; i <= block_last; ++i) {
        u64 block_valid = system_page_flags_valid_[i];
        if (i == block_first) {
          u64 block_before = (static_cast<u64>(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          u64 block_inside = (static_cast<u64>(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          u32 block_page;
          if (range_start == UINT32_MAX) {
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            u64 block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((static_cast<u64>(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            block_valid |= (static_cast<u64>(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  REX_PROFILE_PLOT("gpu/shared_memory/request_ranges_count", static_cast<u32>(count));
  REX_PROFILE_PLOT("gpu/shared_memory/request_ranges_merged_count",
                   static_cast<u32>(merged_ranges.size()));
  REX_PROFILE_PLOT("gpu/shared_memory/request_ranges_upload_count",
                   static_cast<u32>(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    return true;
  }

  return UploadRanges(upload_ranges_);
}

bool SharedMemory::RequestRange(u32 start, u32 length) {
  std::pair<u32, u32> range(start, length);
  return RequestRanges(&range, 1);
}

std::pair<u32, u32> SharedMemory::MemoryInvalidationCallbackThunk(void* context_ptr,
                                                                  u32 physical_address_start,
                                                                  u32 length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<u32, u32> SharedMemory::MemoryInvalidationCallback(u32 physical_address_start, u32 length,
                                                             bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(static_cast<u32>(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  u32 physical_address_last = physical_address_start + (length - 1);

  u32 page_first = physical_address_start >> page_size_log2_;
  u32 page_last = physical_address_last >> page_size_log2_;
  u32 block_first = page_first >> 6;
  u32 block_last = page_last >> 6;

  if (!exact_range) {
    if (page_first & 63) {
      u64 gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (static_cast<u64>(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~static_cast<u32>(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      u64 gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((static_cast<u64>(1) << ((page_last & 63) + 1)) - 1);
      page_last = (page_last & ~static_cast<u32>(63)) +
                  (std::max(rex::tzcnt(gpu_written_end), static_cast<u8>(1)) - 1);
    }
  }

  for (u32 i = block_first; i <= block_last; ++i) {
    u64 invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((static_cast<u64>(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (static_cast<u64>(1) << ((page_last & 63) + 1)) - 1;
    }
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
  }

  FireWatchesLocked(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(u32 start, u32 length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  u32 page_first = start >> page_size_log2_;
  u32 page_last = (start + length - 1) >> page_size_log2_;
  u32 allocation_first = page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  u32 allocation_last = page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                               allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(static_cast<u32>(allocation_range.first),
                                          static_cast<u32>(allocation_range.second))) {
      return false;
    }
    rex::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                  allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    REX_PROFILE_PLOT("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                     host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += static_cast<u32>(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    REX_PROFILE_PLOT("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                     (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = static_cast<u32>(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::gpu
