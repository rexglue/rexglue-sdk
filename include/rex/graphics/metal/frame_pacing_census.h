/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_FRAME_PACING_CENSUS_H_
#define XENIA_GPU_FRAME_PACING_CENSUS_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace rex {
namespace graphics {

// XE_FRAME_PACING_CENSUS=1 (diagnostic, default off): where a frame's time
// goes relative to the guest vblank grid. The vblank thread stamps every
// vblank, the command processor accumulates the time it sleeps in
// WAIT_REG_MEM, and the backend's swap reads both: vblanks between swaps,
// command-processor active time per frame (wall minus WAIT_REG_MEM sleep),
// and how long after the latest vblank the swap arrives.
inline bool FramePacingCensusEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("XE_FRAME_PACING_CENSUS");
    return v && v[0] == '1' && !v[1];
  }();
  return enabled;
}

struct FramePacingClock {
  std::atomic<uint64_t> vblank_index{0};
  std::atomic<int64_t> last_vblank_ns{0};
  std::atomic<uint64_t> cp_waited_ns{0};
  // Backend: time the command processor blocked waiting for GPU completion.
  std::atomic<uint64_t> gpu_waited_ns{0};
  // Command processor idle because the guest had not written more commands.
  std::atomic<uint64_t> ring_idle_ns{0};
};

inline FramePacingClock& GetFramePacingClock() {
  static FramePacingClock clock;
  return clock;
}

inline int64_t FramePacingNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace graphics
}  // namespace rex

#endif  // XENIA_GPU_FRAME_PACING_CENSUS_H_
