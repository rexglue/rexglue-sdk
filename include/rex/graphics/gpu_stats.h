/**
 * @file        rex/graphics/gpu_stats.h
 *
 * @brief       Running totals of the GPU emulation's work, for a performance
 *              readout in Release builds.
 *
 * The counters only ever grow; a reader takes two snapshots some time apart
 * and divides the difference by the number of frames in between. Adding is a
 * relaxed atomic increment, cheap enough for every draw. (A function, not an
 * inline access to the counters: the totals live in the runtime library and
 * Windows DLLs export functions, not data.)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::graphics::stats {

enum class Counter : uint32_t {
  kFrames,               // guest frames presented (swaps)
  kDraws,                // guest draw calls
  kResolves,             // guest resolves (EDRAM -> memory copies)
  kReadbacks,            // resolves copied back to guest memory (readback_resolve)
  kReadbackBytes,        //   bytes copied
  kOcclusionQueries,     // guest occlusion queries completed
  kGpuWaits,             // times the GPU thread blocked waiting for the host GPU
  kGpuWaitMicros,        //   time spent blocked
  kSubmissions,          // host command buffer submissions
  kRenderTargetTransfers,// EDRAM ownership transfers between render targets
  kUploadBytes,          // guest memory uploaded to the GPU's copy
  kTextureLoads,         // textures (re)loaded from guest memory
  kWatchFaults,          // CPU writes caught by the GPU memory write watch
  kCount
};

struct Snapshot {
  uint64_t values[size_t(Counter::kCount)] = {};
  uint64_t time_us = 0;  // steady clock
  uint64_t operator[](Counter c) const { return values[size_t(c)]; }
};

void Add(Counter counter, uint64_t amount = 1);

// The current totals.
Snapshot Read();

// "draws", "resolves" ...
const char* Name(Counter counter);

// Measures how long the GPU thread blocks on the host GPU (kGpuWaits,
// kGpuWaitMicros) for the lifetime of the object.
class ScopedGpuWait {
 public:
  ScopedGpuWait();
  ~ScopedGpuWait();
  ScopedGpuWait(const ScopedGpuWait&) = delete;
  ScopedGpuWait& operator=(const ScopedGpuWait&) = delete;

 private:
  uint64_t start_us_;
};

}  // namespace rex::graphics::stats
