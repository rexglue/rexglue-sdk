/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_upload_arena.h"

#import <Metal/Metal.h>

#include <rex/logging/log_category.h>
#include "rex/math.h"

namespace rex {
namespace graphics {
namespace metal {

MetalUploadArena::MetalUploadArena(void* device, uint32_t size)
    : device_(device), size_(size) {}

MetalUploadArena::~MetalUploadArena() {
  if (buffer_) {
    id<MTLBuffer> released = (__bridge_transfer id<MTLBuffer>)buffer_;
    (void)released;
    buffer_ = nullptr;
  }
}

bool MetalUploadArena::Initialize() {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLBuffer> buffer =
      [device newBufferWithLength:size_
                          options:MTLResourceStorageModeShared |
                                  MTLResourceCPUCacheModeWriteCombined];
  if (!buffer) {
    REXLOG_ERROR("MetalUploadArena: failed to allocate {} byte arena", size_);
    return false;
  }
  buffer_ = (__bridge_retained void*)buffer;
  contents_ = static_cast<uint8_t*>([buffer contents]);
  gpu_base_ = [buffer gpuAddress];
  return true;
}

void MetalUploadArena::Reset() {
  head_ = 0;
  total_allocated_ = 0;
  total_reclaimed_ = 0;
  marks_.clear();
}

void MetalUploadArena::MarkSubmission(uint64_t submission) {
  // Nothing allocated since the previous mark: no region to stamp.
  if (!marks_.empty() && marks_.back().second == total_allocated_) {
    // Still record so Reclaim(submission) advances (zero-size region).
    marks_.emplace_back(submission, total_allocated_);
    return;
  }
  marks_.emplace_back(submission, total_allocated_);
}

void MetalUploadArena::Reclaim(uint64_t completed) {
  while (!marks_.empty() && marks_.front().first <= completed) {
    total_reclaimed_ = marks_.front().second;
    marks_.pop_front();
  }
}

bool MetalUploadArena::Allocate(uint32_t size, uint32_t alignment,
                                MetalUploadAllocation* out) {
  if (!contents_ || !size || size > size_) {
    return false;
  }
  uint32_t aligned = alignment ? rex::align(head_, alignment) : head_;
  uint64_t waste = aligned - head_;
  if (uint64_t(aligned) + size > size_) {
    // Wrap: the rest of the buffer is waste; restart at 0 (already aligned
    // for any power-of-two alignment).
    waste = size_ - head_;
    aligned = 0;
  }
  uint64_t live = total_allocated_ - total_reclaimed_;
  if (live + waste + size > size_) {
    // Ring full: the space is owned by in-flight submissions. The caller may
    // wait for a completion, Reclaim, and retry. (Log throttled: this is an
    // expected transient under async submission, not a leak.)
    static uint32_t s_full_logs = 0;
    if (s_full_logs++ < 8) {
      REXLOG_WARN("MetalUploadArena: ring full (live {} + {} > {})",
             live, waste + size, size_);
    }
    return false;
  }
  out->buffer = buffer_;
  out->offset = aligned;
  out->gpu_address = gpu_base_ + aligned;
  out->cpu_ptr = contents_ + aligned;
  head_ = aligned + size;
  total_allocated_ += waste + size;
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
