/**
 * @file        perf/frame_recorder.h
 * @brief       Per-frame time breakdown, cheap enough to leave on while playing
 *
 * What it does:
 *   - Code on the frame path marks a section with REX_FRAME_ZONE(zone). The
 *     time in each section is added up per thread, with no lock, no file
 *     write and no allocation.
 *   - At each guest swap the totals become one row in a fixed block of memory
 *     that holds the last few thousand frames.
 *   - On request the rows are copied out and written to a CSV file on another
 *     thread, so a stutter that already happened can still be looked at.
 *
 * Times are exclusive: a section nested in another one is not counted in the
 * outer one. A section still open when a frame ends is counted in the frame
 * where it closes.
 *
 * Levels: 0 is off (one load and a branch per section), 1 records the frame
 * sections plus one section per draw, 2 also records the sections inside each
 * draw and costs several times more per draw.
 *
 * The functions are plain C exports, so the GPU plugin and the title can call
 * them across the runtime library boundary.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::perf {

enum class FrameZone : uint32_t {
  // Level 1.
  kIdle,           // the GPU thread waiting for the game to send commands
  kRegWait,        // WAIT_REG_MEM sleeps: the game waiting on a GPU register
  kLimiter,        // the frame limiter pacing the guest swap
  kSwap,           // IssueSwap
  kDraw,           // IssueDraw
  kCopy,           // IssueCopy (resolves)
  kEndSubmission,  // closing and executing a host command list
  kFenceWait,      // waiting for the host GPU to finish a submission
  kPipelineWait,   // waiting for a pipeline that is still being created
  kPresent,        // host paint and present, on whichever thread does it
  // Level 2: inside each draw. kBeginSubmission also runs for each copy, where
  // its time comes out of kCopy.
  kBeginSubmission,    // making sure a host command list is open
  kDrawPrimitive,      // primitive processing
  kDrawRenderTargets,  // render target update
  kDrawPipeline,       // pipeline lookup
  kDrawTextures,       // texture requests
  kDrawSharedMemory,   // guest memory ranges uploaded for the draw
  kCount
};

inline constexpr uint32_t kFrameZoneCount = static_cast<uint32_t>(FrameZone::kCount);

// One open section. Lives on the stack of the code being timed.
struct FrameZoneScope {
  uint64_t start_ticks;
  uint64_t accumulated_ticks;
  FrameZoneScope* parent;
  uint32_t zone;
};

// Totals over the most recent recorded frames.
struct FrameSummary {
  uint32_t frames;  // rows the numbers below cover
  float frame_ms_mean;
  float frame_ms_p50;
  float frame_ms_p95;
  float frame_ms_p99;
  float frame_ms_max;
  float other_ms_mean;  // frame-thread time in no section
  float zone_ms_mean[kFrameZoneCount];
  float zone_calls_mean[kFrameZoneCount];
};

}  // namespace rex::perf

extern "C" {
// The level is read on every section, so callers load it through this pointer
// and skip the call when the recorder is off.
const std::atomic<uint32_t>* rex_frame_recorder_level_ptr();
// 0 off, 1 frame sections, 2 frame sections and the sections inside each draw.
void rex_frame_recorder_set_level(uint32_t level);

void rex_frame_zone_begin(rex::perf::FrameZoneScope* scope, uint32_t zone);
void rex_frame_zone_end(rex::perf::FrameZoneScope* scope);
// Column name of a section, such as "draw", or "" when out of range.
const char* rex_frame_zone_name(uint32_t zone);

// Ends the current frame. Call from one thread, at the guest swap.
void rex_frame_recorder_end_frame();

// Writes the recorded frames, oldest first, to a CSV file at `path_utf8`.
// With `wait` 0 the file is written on its own thread and the call returns at
// once. Returns the number of frames taken, 0 when nothing is recorded.
uint32_t rex_frame_recorder_dump(const char* path_utf8, uint32_t wait);
// Fills `out` from the last `last_frames` rows (0 means all of them). Returns
// the number of rows used.
uint32_t rex_frame_recorder_summary(uint32_t last_frames, rex::perf::FrameSummary* out);
// Forgets every recorded frame. `capacity` is the number of frames to keep, 0
// for the default.
void rex_frame_recorder_reset(uint32_t capacity);
}

namespace rex::perf {

inline uint32_t FrameRecorderLevel() {
  static const std::atomic<uint32_t>* const level = rex_frame_recorder_level_ptr();
  return level->load(std::memory_order_relaxed);
}

class ScopedFrameZone {
 public:
  explicit ScopedFrameZone(FrameZone zone, uint32_t min_level = 1)
      : active_(FrameRecorderLevel() >= min_level) {
    if (active_)
      rex_frame_zone_begin(&scope_, static_cast<uint32_t>(zone));
  }
  ~ScopedFrameZone() {
    if (active_)
      rex_frame_zone_end(&scope_);
  }
  ScopedFrameZone(const ScopedFrameZone&) = delete;
  ScopedFrameZone& operator=(const ScopedFrameZone&) = delete;

 private:
  FrameZoneScope scope_;
  bool active_;
};

}  // namespace rex::perf

#define REX_FRAME_ZONE_CONCAT2(a, b) a##b
#define REX_FRAME_ZONE_CONCAT(a, b) REX_FRAME_ZONE_CONCAT2(a, b)
// Times the rest of the enclosing block as `zone` (a FrameZone name, such as kDraw).
#define REX_FRAME_ZONE(zone)                                        \
  rex::perf::ScopedFrameZone REX_FRAME_ZONE_CONCAT(rex_frame_zone_, \
                                                   __LINE__)(rex::perf::FrameZone::zone)
// The same, recorded only at level 2.
#define REX_FRAME_ZONE_DETAIL(zone)                                            \
  rex::perf::ScopedFrameZone REX_FRAME_ZONE_CONCAT(rex_frame_zone_, __LINE__)( \
      rex::perf::FrameZone::zone, 2)
