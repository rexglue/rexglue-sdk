/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ZPD_SLOT_POOL_H_
#define XENIA_GPU_METAL_METAL_ZPD_SLOT_POOL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex {
namespace graphics {
namespace metal {

// Slot-allocation policy for the ZPD visibility result buffer, extracted as
// pure logic so its safety invariants are testable without a Metal device.
// MetalZPDVisibilityPool owns the actual MTLBuffer and delegates every
// index/generation decision here, so production and tests share ONE policy.
//
// Each slot is one 8-byte visibility counter at a fixed buffer offset, and
// maps to one physical query segment -- never to a guest ZPD address.
//
// The generation is the mechanism that keeps a recycled slot from delivering a
// stale sample count to whichever report owns the slot next. A resolve records
// the (index, generation) it was queued with; if the generation has moved on by
// the time that resolve drains, the pending read is dropped instead of being
// attributed to the new owner.
//
// Two invariants are deliberately stronger than the XeniOS mechanism reference
// this was adapted from, because both are cheap and both turn a silent
// free-list corruption into a no-op:
//   1. Release only acts on a slot that is currently acquired. In the reference
//      a never-acquired index still carries the initial generation, so a
//      spurious Release(index, 0) would push a duplicate onto the free list and
//      hand the same offset to two live segments.
//   2. Release with a non-current generation is rejected rather than trusted,
//      so a double release cannot re-add an index.
class MetalZPDSlotPool {
 public:
  // One 64-bit visibility counter per slot, as Metal writes them.
  static constexpr uint32_t kSlotSize = sizeof(uint64_t);
  static constexpr uint32_t kInvalidIndex = UINT32_MAX;

  static constexpr size_t OffsetForIndex(uint32_t index) {
    return static_cast<size_t>(index) * kSlotSize;
  }
  static constexpr size_t BufferSizeForCapacity(uint32_t capacity) {
    return static_cast<size_t>(capacity) * kSlotSize;
  }

  // (Re)builds the pool with `capacity` free slots. Generations restart, which
  // is only sound when nothing is in flight -- the owner enforces that by
  // rebuilding solely from EnsureInitialized/Shutdown.
  void Reset(uint32_t capacity) {
    capacity_ = capacity;
    generations_.assign(capacity, 0);
    acquired_.assign(capacity, false);
    free_indices_.clear();
    free_indices_.reserve(capacity);
    // Descending push so the first Acquire hands out index 0, keeping the
    // buffer's hot region at its start and traces readable.
    for (uint32_t i = capacity; i > 0; --i) {
      free_indices_.push_back(i - 1);
    }
  }

  void Clear() {
    capacity_ = 0;
    generations_.clear();
    acquired_.clear();
    free_indices_.clear();
  }

  uint32_t capacity() const { return capacity_; }
  bool has_free_indices() const { return !free_indices_.empty(); }
  uint32_t free_count() const {
    return static_cast<uint32_t>(free_indices_.size());
  }
  uint32_t acquired_count() const { return capacity_ - free_count(); }

  // Takes a free slot. On success `index`/`generation` identify it for a later
  // Release, and `offset` is its byte offset in the visibility buffer.
  bool Acquire(uint32_t& index, uint32_t& generation, size_t& offset) {
    if (free_indices_.empty()) {
      index = kInvalidIndex;
      generation = 0;
      offset = 0;
      return false;
    }
    index = free_indices_.back();
    free_indices_.pop_back();
    generation = generations_[index];
    offset = OffsetForIndex(index);
    acquired_[index] = true;
    return true;
  }

  // Returns a slot acquired with this exact generation. Advancing the
  // generation is what invalidates any resolve still queued against it.
  // Returns false (and changes nothing) for an out-of-range, not-currently
  // acquired, or stale-generation release.
  bool Release(uint32_t index, uint32_t generation) {
    if (index >= capacity_ || !acquired_[index] ||
        generations_[index] != generation) {
      return false;
    }
    ++generations_[index];
    acquired_[index] = false;
    free_indices_.push_back(index);
    return true;
  }

  // True while `generation` still identifies the live occupant of `index`.
  bool IsGenerationCurrent(uint32_t index, uint32_t generation) const {
    return index < capacity_ && acquired_[index] &&
           generations_[index] == generation;
  }

 private:
  uint32_t capacity_ = 0;
  std::vector<uint32_t> free_indices_;
  // Bumped on release so a stale readback from a recycled slot is dropped.
  std::vector<uint32_t> generations_;
  // Guards a release of a slot nobody holds; see invariant 1 above.
  std::vector<bool> acquired_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ZPD_SLOT_POOL_H_
