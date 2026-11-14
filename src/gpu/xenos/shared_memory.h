/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/memory.h>
#include <rex/platform/lock_order.h>
#include <rex/guest.h>
#include <rex/types.h>
#include <rex/utility.h>
#include "gpu/xenos/xenos_export.h"

namespace rex::gpu {

/**
 * Manages memory for unconverted textures, resolve targets, vertex and index buffers that shaders
 * access by guest physical address, tracked per system page.
 */
class REX_GPU_XENOS_API SharedMemory : public rex::NonCopyable, public IPhysicalWriteListener {
 public:
  static constexpr u32 kBufferSizeLog2 = 29;
  static constexpr u32 kBufferSize = 1 << kBufferSizeLog2;

  virtual ~SharedMemory();
  /** Implementations call it from their ClearCache. */
  virtual void ClearCache();
  void SetSystemPageFlagsValidWithGPUDataWritten();
  void InvalidateAllPages();

  using GlobalWatchCallback = void (*)(void* context, u32 address_first, u32 address_last,
                                       bool invalidated_by_gpu);
  using GlobalWatchHandle = void*;
  /**
   * Registers a callback invoked when the GPU memory copy is invalidated by the CPU, or by the GPU
   * when triggered explicitly such as by a resolve. It fires for memory the GPU emulation did not
   * use, such as when the title changes the protection of a range containing the watched range.
   * The callback runs with the memory watch lock held.
   *
   * @param callback_context The context passed to the callback.
   */
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback, void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  using WatchCallback = void (*)(void* context, void* data, u64 argument, bool invalidated_by_gpu);
  using WatchHandle = void*;
  /**
   * Registers a callback invoked when the range is invalidated in the GPU memory copy, firing in
   * the same cases as RegisterGlobalWatch. The callback runs with the memory watch lock held and
   * must not watch or unwatch ranges. The watch is cancelled after the callback and the handle
   * becomes invalid.
   *
   * @param lock The lock from AcquireLock, held by the caller.
   * @param callback The callback to invoke.
   * @param callback_context The subsystem pointer, for example the texture cache.
   * @param callback_data The object, for example a texture.
   * @param callback_argument Subsystem or object specific data.
   */
  WatchHandle WatchMemoryRange(const ScopeGuard& lock, u32 start, u32 length,
                               WatchCallback callback, void* callback_context, void* callback_data,
                               u64 callback_argument);

  /**
   * Takes the memory watch lock that guards page validity, watches and the texture state that
   * watch callbacks update.
   */
  ScopeGuard AcquireLock() const { return write_watch_.LockWriteWatches(); }
  void UnwatchMemoryRange(WatchHandle handle);

  /**
   * Uploads new data for ranges that changed and makes the host GPU memory backing them resident.
   *
   * @param ranges The ranges as start and length pairs.
   */
  bool RequestRanges(const std::pair<u32, u32>* ranges, size_t count);
  bool RequestRange(u32 start, u32 length);

  /**
   * Marks the range as modified by the CPU and invalidates GPU written pages inside it. Unless
   * exact_range is true, the range widens to the nearest GPU written pages, at most 256 KB. Called
   * with the memory watch lock held.
   *
   * @param exact_range True to invalidate only the pages in the range.
   * @return The invalidated range as start address and length.
   */
  PhysicalRange OnPhysicalWrite(PhysicalRange range, bool exact_range) override;

  /**
   * Marks the range as containing GPU generated data such as resolves. It triggers modification
   * callbacks, makes the range valid so pages are not copied from main memory until the CPU
   * modifies them, and protects it. RequestRange must run before the GPU writes, so the CPU data is
   * loaded into the parts of touched pages that the writes leave intact.
   */
  void RangeWrittenByGPU(u32 start, u32 length);

 protected:
  SharedMemory(IPhysicalMemory& memory, IPhysicalWriteWatch& watch);
  /** Implementations call it from their initialization. */
  void InitializeCommon();
  void InitializeSparseHostGPUMemory(u32 granularity_log2);
  /** Runs last in implementation shutdown and in the destructor. */
  void ShutdownCommon();

  /** Sparse allocations are 4 MB. */
  static constexpr u32 kHostGPUMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGPUMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  IPhysicalMemory& GetMemory() const { return physical_memory_; }

  u32 GetPageSizeLog2() const { return page_size_log2_; }

  u32 GetHostGPUMemorySparseGranularityLog2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  /**
   * Allocates a range of sparse host GPU memory. Allocations in the host buffer are aligned the
   * same way as in guest physical memory. A 64 KB allocation can represent 0-64 KB, 64-128 KB and
   * 128-192 KB of guest memory, but not 16-80 KB.
   *
   * @param offset_allocations The offset of the range in allocations.
   * @param length_allocations The length of the range in allocations.
   */
  virtual bool AllocateSparseHostGPUMemoryRange(u32 offset_allocations, u32 length_allocations);

  /**
   * Marks the memory range as updated and protects it.
   *
   * @param written_by_gpu True when the range holds GPU written data.
   */
  void MarkRangeValid(u32 start, u32 length, bool written_by_gpu);

  /**
   * Uploads a range of host pages, only when the sparse host GPU memory allocation succeeded. The
   * ranges are page granular and sorted by address.
   *
   * @param upload_page_ranges The page ranges as start and count pairs.
   */
  virtual bool UploadRanges(const std::vector<std::pair<u32, u32>>& upload_page_ranges) = 0;

 private:
  IPhysicalMemory& physical_memory_;
  IPhysicalWriteWatch& write_watch_;

  /** Log2 of the system page size, the invalidation granularity. */
  u32 page_size_log2_;

  bool AllocateHostGPUMemory(u32 start, u32 length);
  u32 host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<u64> host_gpu_memory_sparse_allocated_;
  u32 host_gpu_memory_sparse_allocations_ = 0;
  u32 host_gpu_memory_sparse_used_bytes_ = 0;

  ScopeGuard write_listener_;
  bool write_listener_registered_ = false;

  /** Ranges to upload, generated by RequestRanges. */
  std::vector<std::pair<u32, u32>> upload_ranges_;

  // Members below are guarded by the memory watch lock.

  /** Pages whose buffer contents are in sync with guest memory. */
  std::vector<u64> system_page_flags_valid_;
  std::vector<u64> system_page_flags_valid_and_gpu_written_;
  u32 num_system_page_flags_ = 0;

  struct GlobalWatch {
    GlobalWatchCallback callback;
    void* callback_context;
  };
  std::vector<std::unique_ptr<GlobalWatch>> global_watches_;
  struct WatchNode;
  struct WatchRange {
    union {
      struct {
        WatchCallback callback;
        void* callback_context;
        void* callback_data;
        u64 callback_argument;
        WatchNode* node_first;
        u32 page_first;
        u32 page_last;
      };
      WatchRange* next_free;
    };
  };
  /**
   * Node for faster checking of watches when pages are written. All 512 MB are split into equally
   * sized buckets, then ranges are checked linearly.
   */
  struct WatchNode {
    union {
      struct {
        WatchRange* range;
        /** Link to another node of this watched range in the next bucket. */
        WatchNode* range_node_next;
        /** Links to nodes of other watched ranges in the bucket. */
        WatchNode* bucket_node_previous;
        WatchNode* bucket_node_next;
      };
      WatchNode* next_free;
    };
  };
  static constexpr u32 kWatchBucketSizeLog2 = 22;
  static constexpr u32 kWatchBucketCount = 1 << (kBufferSizeLog2 - kWatchBucketSizeLog2);
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  /** Pool sizes for WatchRanges and WatchNodes. */
  static constexpr u32 kWatchRangePoolSize = 8192;
  static constexpr u32 kWatchNodePoolSize = 8192;
  std::vector<std::unique_ptr<WatchRange[]>> watch_range_pools_;
  std::vector<std::unique_ptr<WatchNode[]>> watch_node_pools_;
  u32 watch_range_current_pool_allocated_ = 0;
  u32 watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;
  /** Triggers the global and per range watches, removing the triggered range watches. */
  void FireWatches(u32 page_first, u32 page_last, bool invalidated_by_gpu);
  void FireWatchesLocked(u32 page_first, u32 page_last, bool invalidated_by_gpu);
  /** Unlinks and frees the range and its nodes. Requires the lock from AcquireLock. */
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace rex::gpu
