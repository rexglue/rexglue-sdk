/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: Coalesced uploads, blit-based managed sync
 */

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>

#include <rex/graphics/metal/shared_memory.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/ui/metal/metal_provider.h>
#include <rex/logging.h>
#include <rex/xenia_logging_compat.h>
#include <rex/memory.h>

namespace rex::graphics::metal {

MetalSharedMemory::MetalSharedMemory(MetalCommandProcessor& command_processor,
                                     memory::Memory& memory)
    : SharedMemory(memory),
      command_processor_(command_processor) {
  dirty_bitmap_.resize(kDirtyBitmapSize, 0);
  dirty_ranges_.reserve(256);
}

MetalSharedMemory::~MetalSharedMemory() {
  Shutdown(true);
}

bool MetalSharedMemory::Initialize() {
  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  const auto& caps = provider.caps();

  MTLResourceOptions options;
  if (caps.PreferSharedStorage()) {
    // Apple Silicon: shared memory, zero-copy CPU→GPU.
    options = MTLResourceStorageModeShared |
              MTLResourceHazardTrackingModeTracked |
              MTLResourceCPUCacheModeWriteCombined;
  } else {
    // Intel Mac: managed memory, explicit sync.
    options = MTLResourceStorageModeManaged |
              MTLResourceHazardTrackingModeTracked;
  }

  if (kBufferSize > caps.max_buffer_length) {
    XELOGE("MetalSharedMemory: Device max buffer length ({} MB) < "
           "required ({} MB)",
           caps.max_buffer_length / (1024 * 1024),
           kBufferSize / (1024 * 1024));
    return false;
  }

  buffer_ = [device newBufferWithLength:kBufferSize options:options];
  if (!buffer_) {
    XELOGE("MetalSharedMemory: Failed to allocate {} MB Metal buffer",
           kBufferSize / (1024 * 1024));
    return false;
  }
  buffer_.label = @"SharedMemory";

  // Metal guarantees new buffers are zero-filled, so skip the expensive
  // 512 MB memset. For managed mode there's nothing to sync since the
  // buffer contents are already zero on both CPU and GPU sides.

  XELOGI("MetalSharedMemory: Allocated {} MB buffer ({})",
         kBufferSize / (1024 * 1024),
         caps.PreferSharedStorage() ? "shared/write-combined" : "managed");

  // Don't mark all pages dirty — both the Metal buffer and guest memory
  // start zeroed, so there's nothing to upload. Pages will be marked dirty
  // as the game writes to memory via MemoryInvalidationCallback.

  return true;
}

void MetalSharedMemory::Shutdown(bool from_destructor) {
  buffer_ = nil;
}

void* MetalSharedMemory::buffer_contents() const {
  return buffer_ ? [buffer_ contents] : nullptr;
}

void MetalSharedMemory::CompletedSubmissionUpdated() {
  // Nothing to release — single persistent buffer.
}

void MetalSharedMemory::BeginSubmission() {
  UploadDirtyPages();
}

void MetalSharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0) return;

  uint32_t page_start = physical_address_start / kPageSize;
  uint32_t page_end =
      (physical_address_start + length + kPageSize - 1) / kPageSize;
  page_end = std::min(page_end, uint32_t(kPageCount));

  std::lock_guard<std::mutex> lock(dirty_mutex_);

  // Set dirty bits at word granularity (64 pages per word) for speed.
  uint32_t first_word = page_start / 64;
  uint32_t last_word = (page_end - 1) / 64;
  uint32_t first_bit = page_start % 64;
  uint32_t last_bit = (page_end - 1) % 64;

  if (first_word == last_word) {
    // All pages fall within one word.
    // Safe formulation: avoids UB when last_bit - first_bit == 63
    // (which would shift uint64_t(2) by 63, placing a 1 at bit 64).
    uint32_t bit_count = last_bit - first_bit + 1;
    uint64_t mask = (bit_count >= 64) ? ~uint64_t(0)
                                      : ((uint64_t(1) << bit_count) - 1);
    dirty_bitmap_[first_word] |= mask << first_bit;
  } else {
    // First partial word.
    dirty_bitmap_[first_word] |= ~uint64_t(0) << first_bit;
    // Full middle words.
    for (uint32_t w = first_word + 1; w < last_word; ++w) {
      dirty_bitmap_[w] = ~uint64_t(0);
    }
    // Last partial word.
    // Safe formulation: avoids UB when last_bit == 63.
    uint64_t last_mask = (last_bit >= 63) ? ~uint64_t(0)
                                          : ((uint64_t(1) << (last_bit + 1)) - 1);
    dirty_bitmap_[last_word] |= last_mask;
  }
}

size_t MetalSharedMemory::UploadDirtyPages() {
  if (!buffer_) return 0;

  uint8_t* buffer_ptr = static_cast<uint8_t*>([buffer_ contents]);
  const uint8_t* guest_memory_base =
      memory().TranslatePhysical<const uint8_t*>(0);

  // ---- Phase 1: Collect dirty page ranges under lock ----
  dirty_ranges_.clear();
  auto& ranges = dirty_ranges_;

  {
    std::lock_guard<std::mutex> lock(dirty_mutex_);

    for (size_t word = 0; word < kDirtyBitmapSize; ++word) {
      uint64_t dirty_word = dirty_bitmap_[word];
      if (dirty_word == 0) {
        continue;
      }

      // Clear all dirty bits we're about to process.
      dirty_bitmap_[word] = 0;

      // Iterate each contiguous run of set bits within the word to avoid
      // over-copying clean pages between non-adjacent dirty bits.
      while (dirty_word != 0) {
        int first_bit = __builtin_ctzll(dirty_word);
        // Shift out leading zeros + the contiguous run, find the run length.
        uint64_t shifted = dirty_word >> first_bit;
        int run_length = __builtin_ctzll(~shifted);

        uint32_t first_page = uint32_t(word * 64) + first_bit;
        uint32_t last_page = first_page + run_length - 1;

        // Clear the bits we just processed.
        uint64_t run_mask = ((uint64_t(1) << run_length) - 1) << first_bit;
        dirty_word &= ~run_mask;

        if (first_page >= kPageCount) break;
        if (last_page >= kPageCount) last_page = uint32_t(kPageCount) - 1;

        // Try to coalesce with previous range.
        if (!ranges.empty()) {
          uint32_t gap = first_page - ranges.back().page_end;
          if (gap <= kCoalesceGapPages) {
            // Extend existing range.
            ranges.back().page_end = last_page + 1;
            continue;
          }
        }

        // Start a new range.
        ranges.push_back({first_page, last_page + 1});
      }
    }
  }

  if (ranges.empty()) return 0;

  // ---- Phase 2: Perform coalesced memcpy for each range ----
  size_t total_bytes = 0;
  uint32_t sync_start = UINT32_MAX;
  uint32_t sync_end = 0;

  for (const auto& range : ranges) {
    uint32_t offset = range.page_start * kPageSize;
    uint32_t length = (range.page_end - range.page_start) * kPageSize;

    // Clamp to buffer size.
    if (offset + length > kBufferSize) {
      length = uint32_t(kBufferSize) - offset;
    }
    if (length == 0) continue;

    std::memcpy(buffer_ptr + offset, guest_memory_base + offset, length);
    total_bytes += length;

    if (offset < sync_start) sync_start = offset;
    if (offset + length > sync_end) sync_end = offset + length;
  }

  // ---- Phase 3: For managed storage, notify Metal of modifications ----
  if (total_bytes > 0 && buffer_.storageMode == MTLStorageModeManaged) {
    // Single didModifyRange covering the full dirty span.
    // This is cheaper than calling didModifyRange per-range.
    [buffer_ didModifyRange:NSMakeRange(sync_start, sync_end - sync_start)];
  }

  // Update stats.
  upload_stats_.total_bytes_uploaded.fetch_add(total_bytes,
                                               std::memory_order_relaxed);
  upload_stats_.total_uploads.fetch_add(1, std::memory_order_relaxed);
  upload_stats_.coalesced_ranges.fetch_add(dirty_ranges_.size(),
                                           std::memory_order_relaxed);

  return total_bytes;
}

void MetalSharedMemory::ResetStats() {
  upload_stats_.total_bytes_uploaded.store(0, std::memory_order_relaxed);
  upload_stats_.total_uploads.store(0, std::memory_order_relaxed);
  upload_stats_.coalesced_ranges.store(0, std::memory_order_relaxed);
}

bool MetalSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (!buffer_) return false;

  uint8_t* buffer_ptr = static_cast<uint8_t*>([buffer_ contents]);
  const uint8_t* guest_memory_base =
      memory().TranslatePhysical<const uint8_t*>(0);
  if (!buffer_ptr || !guest_memory_base) {
    return false;
  }

  uint32_t sync_start = UINT32_MAX;
  uint32_t sync_end = 0;
  size_t total_bytes = 0;

  for (const auto& upload_range : upload_page_ranges) {
    uint32_t page_start = upload_range.first;
    uint32_t page_count = upload_range.second;
    if (!page_count) {
      continue;
    }

    uint64_t offset_64 = uint64_t(page_start) << page_size_log2();
    uint64_t length_64 = uint64_t(page_count) << page_size_log2();
    if (offset_64 >= kBufferSize) {
      continue;
    }
    if (offset_64 + length_64 > kBufferSize) {
      length_64 = kBufferSize - offset_64;
    }

    uint32_t offset = uint32_t(offset_64);
    uint32_t length = uint32_t(length_64);

    // Mark range valid before copying to avoid losing invalidations that may
    // happen while the memcpy is in progress.
    MakeRangeValid(offset, length, false);

    std::memcpy(buffer_ptr + offset, guest_memory_base + offset, length);
    total_bytes += length;

    if (offset < sync_start) sync_start = offset;
    if (offset + length > sync_end) sync_end = offset + length;
  }

  if (total_bytes > 0 && buffer_.storageMode == MTLStorageModeManaged) {
    [buffer_ didModifyRange:NSMakeRange(sync_start, sync_end - sync_start)];
  }

  upload_stats_.total_bytes_uploaded.fetch_add(total_bytes,
                                               std::memory_order_relaxed);
  upload_stats_.total_uploads.fetch_add(1, std::memory_order_relaxed);
  upload_stats_.coalesced_ranges.fetch_add(upload_page_ranges.size(),
                                           std::memory_order_relaxed);

  return true;
}

bool MetalSharedMemory::AllocateSparseHostGpuMemoryRange(
    uint32_t offset_allocations, uint32_t length_allocations) {
  // Metal doesn't support sparse buffers — the entire buffer is allocated
  // up front. This is a no-op.
  return true;
}

}  // namespace rex::graphics::metal
