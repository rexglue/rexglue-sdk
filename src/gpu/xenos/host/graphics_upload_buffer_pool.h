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

#include <cstddef>
#include <cstdint>
#include <memory>

#include <rex/types.h>
#include <rex/utility.h>
#include "gpu/xenos/xenos_export.h"

namespace rex {
namespace gpu {

/**
 * Pool of upload buffer pages reclaimed by submission index.
 *
 * A submission index is the fence value or a value derived from it.
 */
class REX_GPU_XENOS_API GraphicsUploadBufferPool : public rex::NonCopyable {
 public:
  /** Matches the Direct3D 12 MiniEngine sample LinearAllocator kCpuAllocatorPageSize. */
  static constexpr size_t kDefaultPageSize = 2_MiB;

  virtual ~GraphicsUploadBufferPool();

  void Reclaim(u64 completed_submission_index);
  void ResetSubmissionTimeline();
  void ClearCache();

  /** Flushes pending writes of the current page. */
  void FlushWrites();

 protected:
  struct REX_GPU_XENOS_API Page {
    virtual ~Page();
    u64 last_submission_index;
    Page* next;
  };

  GraphicsUploadBufferPool(size_t page_size) : page_size_(page_size) {}

  /**
   * Requests space for data written in a single piece.
   *
   * Creates a new page when the current one lacks free space.
   *
   * @param submission_index Submission using the space.
   * @param alignment Power of 2 alignment in bytes.
   * @param offset_out Receives the offset within the page.
   * @return The page, or null on failure.
   */
  Page* Request(u64 submission_index, size_t size, size_t alignment, size_t& offset_out);

  /**
   * Requests space for data written in multiple parts, filling the buffer.
   *
   * @param submission_index Submission using the space.
   * @param size Total size in bytes.
   * @param alignment Power of 2 alignment in bytes.
   * @param offset_out Receives the offset within the page.
   * @param size_out Receives the size granted in bytes.
   * @return The page, or null on failure.
   */
  Page* RequestPartial(u64 submission_index, size_t size, size_t alignment, size_t& offset_out,
                       size_t& size_out);

  virtual std::unique_ptr<Page> CreatePageImplementation() = 0;

  virtual void FlushPageWrites(Page* page, size_t offset, size_t size);

  /** May grow on creation or first allocation when the real allocation exceeds the page size. */
  size_t page_size_;

  Page* writable_first_ = nullptr;
  Page* writable_last_ = nullptr;
  Page* submitted_first_ = nullptr;
  Page* submitted_last_ = nullptr;

  size_t current_page_used_ = 0;
  size_t current_page_flushed_ = 0;
};

}  // namespace gpu
}  // namespace rex
