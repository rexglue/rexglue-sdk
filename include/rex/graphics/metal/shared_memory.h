/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: Coalesced uploads, atomic tracking, blit path
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/graphics/shared_memory.h>

#ifdef __OBJC__
@protocol MTLBuffer;
@protocol MTLDevice;
@protocol MTLBlitCommandEncoder;
#endif

namespace rex::graphics::metal {

class MetalCommandProcessor;

class MetalSharedMemory : public SharedMemory {
 public:
  MetalSharedMemory(MetalCommandProcessor& command_processor,
                    memory::Memory& memory);
  ~MetalSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  void MemoryInvalidationCallback(uint32_t physical_address_start,
                                  uint32_t length, bool exact_range);

  // Upload modified memory ranges to the Metal buffer.
  // Returns the number of bytes uploaded.
  size_t UploadDirtyPages();

  static constexpr size_t kBufferSize = 512 * 1024 * 1024;  // 512 MB

#ifdef __OBJC__
  id<MTLBuffer> buffer() const { return buffer_; }
#else
  void* buffer() const { return buffer_; }
#endif

  void* buffer_contents() const;

  // --- Stats for profiling ---
  struct UploadStats {
    std::atomic<uint64_t> total_bytes_uploaded{0};
    std::atomic<uint64_t> total_uploads{0};
    std::atomic<uint64_t> coalesced_ranges{0};  // How many separate ranges were merged
  };
  const UploadStats& upload_stats() const { return upload_stats_; }
  void ResetStats();

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;
  bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>&
          upload_page_ranges) override;

 private:
  MetalCommandProcessor& command_processor_;

  // Page-level dirty tracking.
  static constexpr size_t kPageSize = 4096;
  static constexpr size_t kPageCount = kBufferSize / kPageSize;
  static constexpr size_t kDirtyBitmapSize = (kPageCount + 63) / 64;

  // Maximum gap (in pages) between dirty pages to still coalesce into
  // one memcpy. Avoids issuing thousands of tiny copies when there's
  // a small gap between two dirty regions.
  static constexpr uint32_t kCoalesceGapPages = 4;  // 16 KB gap tolerance

  struct DirtyRange {
    uint32_t page_start;
    uint32_t page_end;  // exclusive
  };
  std::vector<DirtyRange> dirty_ranges_;  // Pre-allocated, reused each call

  std::vector<uint64_t> dirty_bitmap_;
  std::mutex dirty_mutex_;

  UploadStats upload_stats_;

#ifdef __OBJC__
  id<MTLBuffer> buffer_ = nil;
#else
  void* buffer_ = nullptr;
#endif
};

}  // namespace rex::graphics::metal
