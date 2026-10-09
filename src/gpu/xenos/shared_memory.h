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
#include <rex/runtime/xmemory.h>

namespace rex::gpu {

/**
 * Manages memory for unconverted textures, resolve targets, vertex and index buffers accessed from
 * shaders with Xenon physical addresses, with system page size granularity.
 */
class SharedMemory {
 public:
  static constexpr uint32_t kBufferSizeLog2 = 29;
  static constexpr uint32_t kBufferSize = 1 << kBufferSizeLog2;

  virtual ~SharedMemory();
  /** Clears the cache. Implementations call it from their ClearCache. */
  virtual void ClearCache();
  void SetSystemPageBlocksValidWithGpuDataWritten();
  void InvalidateAllPages();

  using GlobalWatchCallback = void (*)(void* context, uint32_t address_first, uint32_t address_last,
                                       bool invalidated_by_gpu);
  using GlobalWatchHandle = void*;
  /**
   * Registers a callback invoked when something in the GPU memory copy is invalidated by the CPU
   * or, when triggered explicitly such as by a resolve, by the GPU. It fires for writes to pages
   * previously requested, and may also fire for memory the GPU emulation did not use, for example
   * when the title changes the protection of a memory range containing the watched range. The
   * callback runs with the memory watch lock held.
   *
   * @param callback The callback to invoke.
   * @param callback_context The context passed to the callback.
   * @return The watch handle.
   */
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback, void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  using WatchCallback = void (*)(void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu);
  using WatchHandle = void*;
  /**
   * Registers a callback invoked when the memory range is invalidated in the GPU memory copy by
   * the CPU or, when triggered explicitly such as by a resolve, by the GPU. It fires for writes to
   * pages previously requested, and may also fire for memory the GPU emulation did not use, for
   * example when the title changes the protection of a memory range containing the watched range.
   * The callback runs with the memory watch lock held and must not watch or unwatch ranges. The
   * watch is cancelled after the callback and the handle becomes invalid.
   *
   * @param lock The lock returned by AcquireLock, held by the caller.
   * @param start The start of the range.
   * @param length The length of the range.
   * @param callback The callback to invoke.
   * @param callback_context The subsystem pointer, for example the texture cache.
   * @param callback_data The object, for example a texture.
   * @param callback_argument Subsystem or object specific data.
   * @return The watch handle.
   */
  WatchHandle WatchMemoryRange(const std::unique_lock<rex::platform::RankedMutex>& lock,
                               uint32_t start, uint32_t length, WatchCallback callback,
                               void* callback_context, void* callback_data,
                               uint64_t callback_argument);

  /**
   * Takes the memory watch lock that guards page validity, watches and the texture state that
   * watch callbacks update.
   *
   * @return The held lock.
   */
  std::unique_lock<rex::platform::RankedMutex> AcquireLock() const {
    return std::unique_lock(memory_.watch_mutex());
  }
  /**
   * Unregisters a watched memory range.
   *
   * @param handle The handle returned by WatchMemoryRange.
   */
  void UnwatchMemoryRange(WatchHandle handle);

  /**
   * Uploads new data for ranges that changed and makes the host GPU memory backing them resident.
   *
   * @param ranges The ranges as start and length pairs.
   * @param count The number of ranges.
   * @return True when the ranges are updated and usable.
   */
  bool RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count);
  bool RequestRange(uint32_t start, uint32_t length);

  /**
   * Marks the range as modified by the CPU and invalidates GPU written pages inside it. Unless
   * exact_range, the range widens up to the nearest GPU written pages, at most 256 KB with 4 KB
   * pages, since GPU written data is out of sync with CPU memory and cannot be reuploaded.
   * Uploading excess data is much cheaper than catching access violations. In a software rendered
   * title, 4 KB callbacks run at 4 FPS, 64 KB takes 3 ms of CPU time per frame and 256 KB takes
   * 0.7 ms. Called with the memory watch lock held.
   *
   * @param physical_address_start The start of the modified range.
   * @param length The length of the modified range.
   * @param exact_range True to invalidate only the pages in the range.
   * @return The invalidated range as start address and length.
   */
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t physical_address_start,
                                                           uint32_t length, bool exact_range);

  /**
   * Marks the range as containing GPU generated data such as resolves, triggers modification
   * callbacks, makes the range valid so pages are not copied from main memory until the CPU
   * modifies them, and protects it. RequestRange must run before the GPU writes, so that when the
   * writes do not overwrite everything in the pages they touch, the CPU data is loaded to the
   * unmodified regions of those pages.
   *
   * @param start The start of the range.
   * @param length The length of the range.
   */
  void RangeWrittenByGpu(uint32_t start, uint32_t length);

 protected:
  SharedMemory(runtime::Memory& memory);
  /** Initializes the common state. Implementations call it from their initialization. */
  void InitializeCommon();
  void InitializeSparseHostGpuMemory(uint32_t granularity_log2);
  /** Shuts down the common state. Runs last in implementation shutdown and in the destructor. */
  void ShutdownCommon();

  /**
   * Sparse allocations are 4 MB, which keeps their count low without much padding. 16 MB pads
   * too much.
   */
  static constexpr uint32_t kHostGpuMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGpuMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  runtime::Memory& memory() const { return memory_; }

  uint32_t page_size_log2() const { return page_size_log2_; }

  uint32_t host_gpu_memory_sparse_granularity_log2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  /**
   * Allocates a range of sparse host GPU memory. Allocations in the host buffer are aligned the
   * same way as in guest physical memory. A 64 KB allocation can represent 0-64 KB, 64-128 KB and
   * 128-192 KB of guest memory, but not 16-80 KB. The texture cache data access rules assume this.
   *
   * @param offset_allocations The offset of the range in allocations.
   * @param length_allocations The length of the range in allocations.
   * @return True when the allocation succeeded.
   */
  virtual bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                uint32_t length_allocations);

  /**
   * Marks the memory range as updated and protects it.
   *
   * @param start The start of the range.
   * @param length The length of the range.
   * @param written_by_gpu True when the range holds GPU written data.
   */
  void MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu);

  /**
   * Uploads a range of host pages, only when the sparse host GPU memory allocation succeeded.
   * While uploading, MakeRangeValid must run for each uploaded range as early as possible, before
   * the memcpy, so invalidation during the CPU to GPU memcpy is not missed. The ranges are in
   * pages because MakeRangeValid has page granularity. They are sorted in ascending address order,
   * so front and back give the overall bounds of the pages to upload.
   *
   * @param upload_page_ranges The page ranges as start and count pairs.
   * @return True when the upload succeeded.
   */
  virtual bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) = 0;

 private:
  runtime::Memory& memory_;

  /**
   * Log2 of the invalidation granularity, the system page size. Touched pages of this size are
   * invalidated.
   */
  uint32_t page_size_log2_;

  bool EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length);
  uint32_t host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> host_gpu_memory_sparse_allocated_;
  uint32_t host_gpu_memory_sparse_allocations_ = 0;
  uint32_t host_gpu_memory_sparse_used_bytes_ = 0;

  void* memory_invalidation_callback_handle_ = nullptr;

  /** Ranges to upload, generated by RequestRanges. */
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;

  // Members below are guarded by the memory watch lock.

  /** Pages whose buffer contents are in sync with guest memory. */
  std::vector<uint64_t> system_page_flags_valid_;
  std::vector<uint64_t> system_page_flags_valid_and_gpu_written_;
  uint32_t num_system_page_flags_ = 0;

  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

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
        uint64_t callback_argument;
        WatchNode* node_first;
        uint32_t page_first;
        uint32_t page_last;
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
  static constexpr uint32_t kWatchBucketSizeLog2 = 22;
  static constexpr uint32_t kWatchBucketCount = 1 << (kBufferSizeLog2 - kWatchBucketSizeLog2);
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  /**
   * Pool sizes for WatchRanges and WatchNodes. Allocation takes from the free list first, then
   * creates a pool when the current one is used up, then allocates linearly from the current pool.
   */
  static constexpr uint32_t kWatchRangePoolSize = 8192;
  static constexpr uint32_t kWatchNodePoolSize = 8192;
  std::vector<std::unique_ptr<WatchRange[]>> watch_range_pools_;
  std::vector<std::unique_ptr<WatchNode[]>> watch_node_pools_;
  uint32_t watch_range_current_pool_allocated_ = 0;
  uint32_t watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;
  /**
   * Triggers the global and per range watches, removing the triggered range watches.
   *
   * @param page_first The first page.
   * @param page_last The last page.
   * @param invalidated_by_gpu True when the GPU invalidated the pages.
   */
  void FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu);
  void FireWatchesLocked(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu);
  /**
   * Unlinks and frees the range and its nodes. Requires the global critical region.
   *
   * @param range The range to unlink.
   */
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace rex::gpu
