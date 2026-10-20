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

// 2: kGpuThreadMicros, kSubmitMicros, kShaderTranslations,
// kShaderTranslateMicros and ScopedMicros. 3: kPresents. 4: kHostGpuMicros.
// 5: kHostGpuDrawMicros ... kHostGpuOtherMicros.
// A game built against any version can test this to know which counters exist.
#define REX_GRAPHICS_STATS_VERSION 5

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
  kTexturesReplaced,     // host textures created from a texture replacement pack
  kTexturesNotInPack,    // textures that could have been replaced, but aren't in the pack
  // The GPU emulation runs every guest command on one host thread. Its busy
  // time per frame next to the frame time says whether that thread is what
  // limits the frame rate (busy ~= frame time) or something else is.
  kGpuThreadMicros,      // time the GPU thread spent executing guest commands
                         //   (includes kGpuWaitMicros and the two below)
  kSubmitMicros,         //   of which: turning the recorded commands into a
                         //   host command buffer and submitting it
  kShaderTranslations,   // guest shaders translated while a draw waited for them
  kShaderTranslateMicros,//   time spent (the draw, and the frame, wait)
  // Pictures the window presented: the game's frames plus repaints for the
  // UI alone (an open settings window, an animation). Well above the game's
  // frame rate means the window repaints on its own, which costs a CPU core
  // and graphics card time.
  kPresents,
  // How long the graphics card worked on the GPU emulation's command buffers
  // (Vulkan timestamps at each one's start and end, added when it completes;
  // 0 where the device can't time them). Per frame next to the frame time,
  // it says how much room the card has: a card a tenth as fast takes ten
  // times as long.
  kHostGpuMicros,
  // The same time by what the card was doing (Vulkan timestamps where the
  // GPU emulation's work changes kind, attributed to the kind that ended):
  kHostGpuDrawMicros,      // the game's draws
  kHostGpuTransferMicros,  // EDRAM copies between render targets
  kHostGpuResolveMicros,   // resolves (EDRAM -> textures and memory)
  kHostGpuTextureMicros,   // texture loads and conversions
  kHostGpuUploadMicros,    // guest memory copied to the card's copy of it
  kHostGpuSwapMicros,      // the picture shown: gamma, post-processing
  kHostGpuOtherMicros,     // anything else
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

// Adds the lifetime of the object, in microseconds, to a time counter
// (kGpuThreadMicros, kSubmitMicros, kShaderTranslateMicros). Two clock reads
// per object, so meant for whole batches of work, not for every draw.
class ScopedMicros {
 public:
  explicit ScopedMicros(Counter counter);
  ~ScopedMicros();
  ScopedMicros(const ScopedMicros&) = delete;
  ScopedMicros& operator=(const ScopedMicros&) = delete;

 private:
  Counter counter_;
  uint64_t start_us_;
};

}  // namespace rex::graphics::stats
