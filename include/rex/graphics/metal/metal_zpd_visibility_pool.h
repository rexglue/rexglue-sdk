/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ZPD_VISIBILITY_POOL_H_
#define XENIA_GPU_METAL_METAL_ZPD_VISIBILITY_POOL_H_

#include <cstddef>
#include <cstdint>

#include "rex/graphics/metal/metal_zpd_slot_pool.h"

namespace rex {
namespace graphics {
namespace metal {

// One Metal visibility result buffer operated as a pool of 8-byte counters.
// Each acquired slot backs one physical ZPD query segment; the guest report
// it belongs to is tracked by the shared CommandProcessor, never here.
//
// Metal requires the visibility buffer to be attached to a render pass
// descriptor BEFORE its encoder is created, so the buffer's lifetime is
// deliberately independent of any encoder: the command processor attaches this
// buffer to every main-draw pass it builds and then selects per-segment
// offsets with setVisibilityResultMode.
//
// Slot policy (free list, generations, offsets) lives in MetalZPDSlotPool so
// it is testable without a device; this class adds only buffer ownership and
// the completion-gated read.
class MetalZPDVisibilityPool {
 public:
  MetalZPDVisibilityPool() = default;
  MetalZPDVisibilityPool(const MetalZPDVisibilityPool&) = delete;
  MetalZPDVisibilityPool& operator=(const MetalZPDVisibilityPool&) = delete;
  ~MetalZPDVisibilityPool();

  // Allocates the buffer and builds the free list. Idempotent for an unchanged
  // capacity. Returns false and leaves the pool uninitialized on failure, which
  // the caller reports through IsZPDQueryPoolReady so the shared fake-sample
  // fallback stays in control.
  bool EnsureInitialized(void* device, uint32_t requested_capacity);
  void Shutdown();

  bool is_initialized() const {
    return buffer_ != nullptr && mapping_ != nullptr && slots_.capacity() != 0;
  }

  uint32_t capacity() const { return slots_.capacity(); }
  // id<MTLBuffer>, not retained by the caller.
  void* visibility_buffer() const { return buffer_; }
  bool has_free_indices() const { return slots_.has_free_indices(); }
  uint32_t free_count() const { return slots_.free_count(); }

  // Takes a slot and zeroes its counter, so a pass that ends up issuing no
  // draws reads a definite 0 rather than the previous occupant's count.
  bool Acquire(uint32_t& index, uint32_t& generation, size_t& offset);
  bool Release(uint32_t index, uint32_t generation) {
    return slots_.Release(index, generation);
  }
  bool IsGenerationCurrent(uint32_t index, uint32_t generation) const {
    return slots_.IsGenerationCurrent(index, generation);
  }

  // Reads a slot's sample count. The submission that wrote it MUST have
  // completed; the caller enforces that by draining only resolves whose
  // submission is already complete.
  uint64_t Read(uint32_t index) const;

 private:
  void* buffer_ = nullptr;      // id<MTLBuffer>, retained.
  uint64_t* mapping_ = nullptr;  // buffer_.contents(), shared storage.
  MetalZPDSlotPool slots_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ZPD_VISIBILITY_POOL_H_
