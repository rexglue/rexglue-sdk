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

#include <algorithm>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/bit.h>
#include "gpu/xenos/host/graphics_upload_buffer_pool.h"
#include "gpu/xenos/host/page_list.h"

namespace rex {
namespace gpu {

GraphicsUploadBufferPool::~GraphicsUploadBufferPool() {
  ClearCache();
}

void GraphicsUploadBufferPool::Reclaim(u64 completed_submission_index) {
  ReclaimPages(submitted_first_, submitted_last_, writable_first_, writable_last_,
               completed_submission_index);
}

void GraphicsUploadBufferPool::ResetSubmissionTimeline() {
  ResetPageTimeline(submitted_first_, submitted_last_, writable_first_, writable_last_);
}

void GraphicsUploadBufferPool::ClearCache() {
  // Called from the destructor, so must not call virtual functions.
  current_page_flushed_ = 0;
  current_page_used_ = 0;
  while (submitted_first_) {
    Page* next = submitted_first_->next;
    delete submitted_first_;
    submitted_first_ = next;
  }
  submitted_last_ = nullptr;
  while (writable_first_) {
    Page* next = writable_first_->next;
    delete writable_first_;
    writable_first_ = next;
  }
  writable_last_ = nullptr;
}

GraphicsUploadBufferPool::Page::~Page() {}

void GraphicsUploadBufferPool::FlushWrites() {
  if (current_page_flushed_ >= current_page_used_) {
    return;
  }
  REX_ASSERT(writable_first_ != nullptr);
  FlushPageWrites(writable_first_, current_page_flushed_,
                  current_page_used_ - current_page_flushed_);
  current_page_flushed_ = current_page_used_;
}

GraphicsUploadBufferPool::Page* GraphicsUploadBufferPool::Request(u64 submission_index, size_t size,
                                                                  size_t alignment,
                                                                  size_t& offset_out) {
  alignment = std::max(alignment, static_cast<size_t>(1));
  REX_ASSERT(rex::is_pow2(alignment));
  size = rex::RoundUpPow2(size, alignment);
  REX_ASSERT(size <= page_size_);
  if (size > page_size_) {
    return nullptr;
  }
  REX_ASSERT(!current_page_used_ || submission_index >= writable_first_->last_submission_index);
  REX_ASSERT(!submitted_last_ || submission_index >= submitted_last_->last_submission_index);
  size_t current_page_used_aligned = rex::RoundUpPow2(current_page_used_, alignment);
  if (current_page_used_aligned + size > page_size_ || !writable_first_) {
    if (writable_first_) {
      FlushWrites();
      if (submitted_last_) {
        submitted_last_->next = writable_first_;
      } else {
        submitted_first_ = writable_first_;
      }
      submitted_last_ = writable_first_;
      writable_first_ = writable_first_->next;
      submitted_last_->next = nullptr;
      if (!writable_first_) {
        writable_last_ = nullptr;
      }
    }
    if (!writable_first_) {
      std::unique_ptr<Page> created = CreatePageImplementation();
      if (!created) {
        return nullptr;
      }
      writable_first_ = created.release();
      writable_first_->last_submission_index = submission_index;
      writable_first_->next = nullptr;
      writable_last_ = writable_first_;
    }
    current_page_used_ = 0;
    current_page_used_aligned = 0;
    current_page_flushed_ = 0;
  }
  writable_first_->last_submission_index = submission_index;
  offset_out = current_page_used_aligned;
  current_page_used_ = current_page_used_aligned + size;
  return writable_first_;
}

GraphicsUploadBufferPool::Page* GraphicsUploadBufferPool::RequestPartial(
    u64 submission_index, size_t size, size_t alignment, size_t& offset_out, size_t& size_out) {
  alignment = std::max(alignment, static_cast<size_t>(1));
  REX_ASSERT(rex::is_pow2(alignment));
  size = rex::RoundUpPow2(size, alignment);
  size = std::min(size, page_size_);
  size_t current_page_used_aligned = rex::RoundUpPow2(current_page_used_, alignment);
  if (current_page_used_aligned + alignment <= page_size_) {
    size = std::min(size, (page_size_ - current_page_used_aligned) & ~(alignment - 1));
  }
  Page* page = Request(submission_index, size, alignment, offset_out);
  if (!page) {
    return nullptr;
  }
  size_out = size;
  return page;
}

void GraphicsUploadBufferPool::FlushPageWrites([[maybe_unused]] Page* page,
                                               [[maybe_unused]] size_t offset,
                                               [[maybe_unused]] size_t size) {}

}  // namespace gpu
}  // namespace rex
