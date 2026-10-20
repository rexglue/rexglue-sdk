/**
 * @file        core/gpu_stats.cpp
 * @brief       See rex/graphics/gpu_stats.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/gpu_stats.h>

#include <atomic>
#include <chrono>

namespace rex::graphics::stats {

namespace {
std::atomic<uint64_t> g_counters[size_t(Counter::kCount)];

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace

void Add(Counter counter, uint64_t amount) {
  g_counters[size_t(counter)].fetch_add(amount, std::memory_order_relaxed);
}

Snapshot Read() {
  Snapshot snapshot;
  for (size_t i = 0; i < size_t(Counter::kCount); ++i)
    snapshot.values[i] = g_counters[i].load(std::memory_order_relaxed);
  snapshot.time_us = NowMicros();
  return snapshot;
}

const char* Name(Counter counter) {
  switch (counter) {
    case Counter::kFrames: return "frames";
    case Counter::kDraws: return "draws";
    case Counter::kResolves: return "resolves";
    case Counter::kReadbacks: return "readbacks";
    case Counter::kReadbackBytes: return "readback_bytes";
    case Counter::kOcclusionQueries: return "occlusion_queries";
    case Counter::kGpuWaits: return "gpu_waits";
    case Counter::kGpuWaitMicros: return "gpu_wait_us";
    case Counter::kSubmissions: return "submissions";
    case Counter::kRenderTargetTransfers: return "rt_transfers";
    case Counter::kUploadBytes: return "upload_bytes";
    case Counter::kTextureLoads: return "texture_loads";
    case Counter::kWatchFaults: return "watch_faults";
    case Counter::kTexturesReplaced: return "textures_replaced";
    case Counter::kTexturesNotInPack: return "textures_not_in_pack";
    case Counter::kGpuThreadMicros: return "gpu_thread_us";
    case Counter::kSubmitMicros: return "submit_us";
    case Counter::kShaderTranslations: return "shader_translations";
    case Counter::kShaderTranslateMicros: return "shader_translate_us";
    case Counter::kPresents: return "presents";
    case Counter::kHostGpuMicros: return "host_gpu_us";
    case Counter::kCount: break;
  }
  return "?";
}

ScopedGpuWait::ScopedGpuWait() : start_us_(NowMicros()) {}

ScopedGpuWait::~ScopedGpuWait() {
  Add(Counter::kGpuWaits);
  Add(Counter::kGpuWaitMicros, NowMicros() - start_us_);
}

ScopedMicros::ScopedMicros(Counter counter) : counter_(counter), start_us_(NowMicros()) {}

ScopedMicros::~ScopedMicros() {
  Add(counter_, NowMicros() - start_us_);
}

}  // namespace rex::graphics::stats
