/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_zpd_visibility_pool.h"

#import <Metal/Metal.h>

#include <cstring>

#include <rex/logging/log_category.h>

namespace rex {
namespace graphics {
namespace metal {

MetalZPDVisibilityPool::~MetalZPDVisibilityPool() { Shutdown(); }

bool MetalZPDVisibilityPool::EnsureInitialized(void* device,
                                               uint32_t requested_capacity) {
  if (is_initialized() && slots_.capacity() == requested_capacity) {
    return true;
  }
  if (is_initialized()) {
    // Resizing would orphan every in-flight resolve's index. The command
    // processor only reaches this with an idle pool; refuse otherwise rather
    // than recycling slots out from under pending reads.
    if (slots_.acquired_count() != 0) {
      REXLOG_WARN(
          "MetalZPDVisibilityPool: refusing to resize with {} slots still "
          "acquired; keeping the existing pool",
          slots_.acquired_count());
      return true;
    }
    Shutdown();
  }
  if (!requested_capacity || !device) {
    return false;
  }

  size_t buffer_size = MetalZPDSlotPool::BufferSizeForCapacity(
      requested_capacity);
  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
  // Shared storage: the counters are written by the GPU and read by the CPU
  // once the owning submission completes, which is the only access pattern.
  id<MTLBuffer> buffer =
      [mtl_device newBufferWithLength:buffer_size
                              options:MTLResourceStorageModeShared];
  if (!buffer) {
    REXLOG_WARN(
        "MetalZPDVisibilityPool: failed to allocate the {} byte visibility "
        "buffer; occlusion queries stay on the shared fake-sample path",
        buffer_size);
    return false;
  }
  buffer.label = @"zpd:visibility";

  buffer_ = (__bridge_retained void*)buffer;
  mapping_ = static_cast<uint64_t*>([buffer contents]);
  std::memset(mapping_, 0, buffer_size);
  slots_.Reset(requested_capacity);
  return true;
}

void MetalZPDVisibilityPool::Shutdown() {
  slots_.Clear();
  mapping_ = nullptr;
  if (buffer_) {
    id<MTLBuffer> released = (__bridge_transfer id<MTLBuffer>)buffer_;
    (void)released;
    buffer_ = nullptr;
  }
}

bool MetalZPDVisibilityPool::Acquire(uint32_t& index, uint32_t& generation,
                                     size_t& offset) {
  if (!is_initialized()) {
    index = MetalZPDSlotPool::kInvalidIndex;
    generation = 0;
    offset = 0;
    return false;
  }
  if (!slots_.Acquire(index, generation, offset)) {
    return false;
  }
  // The slot's prior submission has completed (a slot is only freed after its
  // resolve drained), so this CPU write cannot race the GPU.
  mapping_[index] = 0;
  return true;
}

uint64_t MetalZPDVisibilityPool::Read(uint32_t index) const {
  if (!mapping_ || index >= slots_.capacity()) {
    return 0;
  }
  return mapping_[index];
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
