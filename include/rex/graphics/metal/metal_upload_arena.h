/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_UPLOAD_ARENA_H_
#define XENIA_GPU_METAL_METAL_UPLOAD_ARENA_H_

#include <cstdint>
#include <deque>

namespace rex {
namespace graphics {
namespace metal {

// A slice handed out by MetalUploadArena: where to write CPU-side, and the
// buffer/offset/gpuAddress to reference from the encoder / argument buffer.
struct MetalUploadAllocation {
  void* buffer = nullptr;     // id<MTLBuffer>
  uint32_t offset = 0;        // byte offset within buffer
  uint64_t gpu_address = 0;   // buffer.gpuAddress() + offset
  void* cpu_ptr = nullptr;    // buffer.contents() + offset
};

// Transient allocator over one shared MTLBuffer, operated as a RING (S8):
// allocations bump `head_`; MarkSubmission stamps the ring position at each
// command-buffer commit; Reclaim releases the stamped regions once their
// submissions complete on the GPU. Accounting is monotonic byte counters
// (allocated vs reclaimed, both including alignment/wrap waste), so "live
// bytes <= capacity" is the single overlap invariant — an Allocate that would
// exceed it fails instead of aliasing memory a in-flight submission reads.
class MetalUploadArena {
 public:
  MetalUploadArena(void* device, uint32_t size);
  ~MetalUploadArena();

  bool Initialize();

  // Hard reset (head to 0, all regions freed). Only safe when NOTHING is in
  // flight — shutdown or the synchronous-wait A/B path.
  void Reset();

  // Stamps the current ring position as the end of `submission`'s
  // allocations. Call at command-buffer commit, before encoding continues.
  void MarkSubmission(uint64_t submission);

  // Releases the regions of all marked submissions <= `completed`.
  void Reclaim(uint64_t completed);

  // Allocates `size` bytes aligned to `alignment`. Returns false (and leaves
  // `out` untouched) if the ring is full (live in-flight regions occupy the
  // space) — the caller may wait for a submission to complete, Reclaim, and
  // retry.
  bool Allocate(uint32_t size, uint32_t alignment, MetalUploadAllocation* out);

  // Ring capacity in bytes (diagnostic: ring-end proximity checks).
  uint32_t size() const { return size_; }

  // Bytes allocated since the last MarkSubmission (the open submission's
  // share of the ring, alignment and wrap waste included).
  uint64_t open_bytes() const {
    return total_allocated_ -
           (marks_.empty() ? total_reclaimed_ : marks_.back().second);
  }

 private:
  void* device_;   // id<MTLDevice>, not owned.
  uint32_t size_;
  uint32_t head_ = 0;  // ring write cursor
  // Monotonic byte counters (include alignment + end-of-buffer wrap waste).
  uint64_t total_allocated_ = 0;
  uint64_t total_reclaimed_ = 0;
  // (submission, total_allocated_ at its commit), oldest first.
  std::deque<std::pair<uint64_t, uint64_t>> marks_;
  void* buffer_ = nullptr;  // id<MTLBuffer>, retained.
  uint8_t* contents_ = nullptr;
  uint64_t gpu_base_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_UPLOAD_ARENA_H_
