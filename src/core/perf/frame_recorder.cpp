/**
 * @file        core/perf/frame_recorder.cpp
 * @brief       Per-frame time breakdown. See frame_recorder.h.
 *
 * Standard library only, so the unit test can build this file on its own.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/frame_recorder.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rex::perf {
namespace {

constexpr uint32_t kZones = kFrameZoneCount;
constexpr uint32_t kDefaultCapacity = 4096;  // about 68 s at 60 FPS, 650 KB

constexpr const char* kZoneNames[] = {
    "idle",
    "reg_wait",
    "limiter",
    "swap",
    "draw",
    "copy",
    "end_submission",
    "fence_wait",
    "pipeline_wait",
    "present",
    "begin_submission",
    "draw_primitive",
    "draw_render_targets",
    "draw_pipeline",
    "draw_textures",
    "draw_shared_memory",
};
static_assert(std::size(kZoneNames) == kZones, "kZoneNames must match FrameZone");

using Clock = std::chrono::steady_clock;

// Cheap and monotonic. Converted to time once per frame, from the frame's own
// length in ticks and in microseconds, so no calibration is needed.
inline uint64_t Ticks() {
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
  return __builtin_ia32_rdtsc();
#else
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
#endif
}

// Running totals of one thread. Only that thread writes `ticks` and `calls`,
// and they only grow; the frame thread reads them and keeps what it has
// already counted in `seen_*`. Blocks are never freed: a thread that exits
// hands its block to the next thread that needs one.
struct ThreadBlock {
  std::atomic<uint64_t> ticks[kZones] = {};
  std::atomic<uint64_t> calls[kZones] = {};
  std::atomic<bool> in_use{false};
  ThreadBlock* next = nullptr;
  uint64_t seen_ticks[kZones] = {};
  uint64_t seen_calls[kZones] = {};
};

std::atomic<uint32_t> g_level{0};
std::atomic<ThreadBlock*> g_blocks{nullptr};

struct ThreadSlot {
  ThreadBlock* block = nullptr;
  FrameZoneScope* current = nullptr;
  ~ThreadSlot() {
    if (block)
      block->in_use.store(false, std::memory_order_release);
  }
};
thread_local ThreadSlot t_slot;

ThreadBlock* AcquireBlock() {
  for (ThreadBlock* block = g_blocks.load(std::memory_order_acquire); block; block = block->next) {
    bool expected = false;
    if (block->in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return block;
    }
  }
  ThreadBlock* block = new ThreadBlock();
  block->in_use.store(true, std::memory_order_relaxed);
  block->next = g_blocks.load(std::memory_order_relaxed);
  while (!g_blocks.compare_exchange_weak(block->next, block, std::memory_order_release,
                                         std::memory_order_relaxed)) {}
  return block;
}

struct FrameRow {
  uint64_t index;
  uint64_t end_us;  // since the first recorded frame
  uint32_t frame_us;
  uint32_t self_us;  // section time on the thread that ended the frame
  uint32_t zone_us[kZones];
  uint32_t zone_calls[kZones];
};

struct Recorder {
  std::mutex mutex;
  std::vector<FrameRow> rows;  // ring, sized on first use
  uint32_t capacity = kDefaultCapacity;
  uint64_t written = 0;  // rows ever written; the ring holds the last `capacity`
  uint64_t frame_index = 0;
  bool have_last = false;
  bool have_epoch = false;
  uint64_t last_ticks = 0;
  Clock::time_point last_time;
  Clock::time_point epoch;
};
Recorder g;

uint32_t SaturateU32(double value) {
  if (!(value > 0))
    return 0;
  return value >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(value + 0.5);
}

// Oldest first. `last_frames` 0 means every row.
std::vector<FrameRow> Snapshot(uint32_t last_frames) {
  std::lock_guard<std::mutex> lock(g.mutex);
  const uint64_t held = std::min<uint64_t>(g.written, g.rows.size());
  uint64_t count = held;
  if (last_frames && last_frames < count)
    count = last_frames;
  std::vector<FrameRow> out;
  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = g.written - count; i < g.written; ++i) {
    out.push_back(g.rows[static_cast<size_t>(i % g.rows.size())]);
  }
  return out;
}

uint32_t MedianFrameUs(const std::vector<FrameRow>& rows) {
  if (rows.empty())
    return 0;
  std::vector<uint32_t> times;
  times.reserve(rows.size());
  for (const FrameRow& row : rows)
    times.push_back(row.frame_us);
  auto middle = times.begin() + times.size() / 2;
  std::nth_element(times.begin(), middle, times.end());
  return *middle;
}

uint32_t OtherUs(const FrameRow& row) {
  return row.frame_us > row.self_us ? row.frame_us - row.self_us : 0;
}

void AppendMs(std::string& text, uint32_t us) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), ",%u.%03u", us / 1000, us % 1000);
  text += buffer;
}

std::string ToCsv(const std::vector<FrameRow>& rows) {
  std::string text = "frame,end_ms,frame_ms,spike,other_ms";
  for (uint32_t z = 0; z < kZones; ++z)
    text += std::string(",") + kZoneNames[z] + "_ms";
  for (uint32_t z = 0; z < kZones; ++z)
    text += std::string(",") + kZoneNames[z] + "_n";
  text += '\n';
  // A spike is a frame that took more than twice the median of the file.
  const uint64_t spike_us = uint64_t(MedianFrameUs(rows)) * 2;
  char buffer[64];
  for (const FrameRow& row : rows) {
    std::snprintf(buffer, sizeof(buffer), "%llu,%llu.%03u",
                  static_cast<unsigned long long>(row.index),
                  static_cast<unsigned long long>(row.end_us / 1000),
                  static_cast<unsigned>(row.end_us % 1000));
    text += buffer;
    AppendMs(text, row.frame_us);
    text += row.frame_us > spike_us ? ",1" : ",0";
    AppendMs(text, OtherUs(row));
    for (uint32_t z = 0; z < kZones; ++z)
      AppendMs(text, row.zone_us[z]);
    for (uint32_t z = 0; z < kZones; ++z) {
      std::snprintf(buffer, sizeof(buffer), ",%u", row.zone_calls[z]);
      text += buffer;
    }
    text += '\n';
  }
  return text;
}

}  // namespace
}  // namespace rex::perf

using namespace rex::perf;

extern "C" {

const std::atomic<uint32_t>* rex_frame_recorder_level_ptr() {
  return &g_level;
}

void rex_frame_recorder_set_level(uint32_t level) {
  g_level.store(std::min<uint32_t>(level, 2), std::memory_order_relaxed);
}

void rex_frame_zone_begin(FrameZoneScope* scope, uint32_t zone) {
  ThreadSlot& slot = t_slot;
  const uint64_t now = Ticks();
  // The outer section stops counting while this one runs.
  if (FrameZoneScope* parent = slot.current) {
    parent->accumulated_ticks += now - parent->start_ticks;
  }
  scope->parent = slot.current;
  scope->zone = zone;
  scope->accumulated_ticks = 0;
  scope->start_ticks = now;
  slot.current = scope;
}

void rex_frame_zone_end(FrameZoneScope* scope) {
  ThreadSlot& slot = t_slot;
  const uint64_t now = Ticks();
  if (scope->zone < kZones) {
    if (!slot.block)
      slot.block = AcquireBlock();
    const uint64_t ticks = scope->accumulated_ticks + (now - scope->start_ticks);
    std::atomic<uint64_t>& total = slot.block->ticks[scope->zone];
    total.store(total.load(std::memory_order_relaxed) + ticks, std::memory_order_relaxed);
    std::atomic<uint64_t>& calls = slot.block->calls[scope->zone];
    calls.store(calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
  slot.current = scope->parent;
  if (scope->parent)
    scope->parent->start_ticks = now;
}

const char* rex_frame_zone_name(uint32_t zone) {
  return zone < kZones ? kZoneNames[zone] : "";
}

void rex_frame_recorder_end_frame() {
  const uint32_t level = g_level.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (level == 0) {
    // The next frame recorded after turning it back on starts a new run.
    g.have_last = false;
    return;
  }
  const uint64_t now_ticks = Ticks();
  const Clock::time_point now = Clock::now();

  // Take what every thread has added since the last frame.
  uint64_t zone_ticks[kZones] = {};
  uint64_t zone_calls[kZones] = {};
  uint64_t self_ticks = 0;
  const ThreadBlock* self = t_slot.block;
  for (ThreadBlock* block = g_blocks.load(std::memory_order_acquire); block; block = block->next) {
    for (uint32_t z = 0; z < kZones; ++z) {
      const uint64_t ticks = block->ticks[z].load(std::memory_order_relaxed);
      const uint64_t calls = block->calls[z].load(std::memory_order_relaxed);
      const uint64_t new_ticks = ticks - block->seen_ticks[z];
      zone_ticks[z] += new_ticks;
      zone_calls[z] += calls - block->seen_calls[z];
      if (block == self)
        self_ticks += new_ticks;
      block->seen_ticks[z] = ticks;
      block->seen_calls[z] = calls;
    }
  }

  if (!g.have_last) {
    // The first frame has no start to measure from; its totals are dropped.
    g.have_last = true;
    g.last_ticks = now_ticks;
    g.last_time = now;
    if (!g.have_epoch) {
      g.have_epoch = true;
      g.epoch = now;
    }
    return;
  }

  const double frame_us = std::chrono::duration<double, std::micro>(now - g.last_time).count();
  const uint64_t frame_ticks = now_ticks - g.last_ticks;
  const double us_per_tick = frame_ticks ? frame_us / static_cast<double>(frame_ticks) : 0.0;
  g.last_ticks = now_ticks;
  g.last_time = now;

  if (g.rows.empty())
    g.rows.resize(g.capacity);
  FrameRow& row = g.rows[static_cast<size_t>(g.written % g.rows.size())];
  row.index = g.frame_index++;
  row.end_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now - g.epoch).count());
  row.frame_us = SaturateU32(frame_us);
  row.self_us = SaturateU32(static_cast<double>(self_ticks) * us_per_tick);
  for (uint32_t z = 0; z < kZones; ++z) {
    row.zone_us[z] = SaturateU32(static_cast<double>(zone_ticks[z]) * us_per_tick);
    row.zone_calls[z] = zone_calls[z] > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(zone_calls[z]);
  }
  ++g.written;
}

uint32_t rex_frame_recorder_dump(const char* path_utf8, uint32_t wait) {
  if (!path_utf8 || !*path_utf8)
    return 0;
  try {
    std::vector<FrameRow> rows = Snapshot(0);
    if (rows.empty())
      return 0;
    const uint32_t count = static_cast<uint32_t>(rows.size());
#ifdef __cpp_char8_t
    std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t*>(path_utf8)));
#else
    // The SDK builds with -fno-char8_t; u8path reads the name as UTF-8 there.
    std::filesystem::path path = std::filesystem::u8path(path_utf8);
#endif
    auto write = [rows = std::move(rows), path = std::move(path)]() {
      try {
        const std::string text = ToCsv(rows);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (file)
          file.write(text.data(), static_cast<std::streamsize>(text.size()));
      } catch (const std::exception&) {
        // A failed dump must not take the game down.
      }
    };
    if (wait) {
      write();
    } else {
      std::thread(std::move(write)).detach();
    }
    return count;
  } catch (const std::exception&) {
    return 0;
  }
}

uint32_t rex_frame_recorder_summary(uint32_t last_frames, FrameSummary* out) {
  if (!out)
    return 0;
  *out = FrameSummary{};
  const std::vector<FrameRow> rows = Snapshot(last_frames);
  if (rows.empty())
    return 0;
  const double count = static_cast<double>(rows.size());
  std::vector<uint32_t> times;
  times.reserve(rows.size());
  double frame_total = 0, other_total = 0;
  double zone_total[kZones] = {};
  double calls_total[kZones] = {};
  for (const FrameRow& row : rows) {
    times.push_back(row.frame_us);
    frame_total += row.frame_us;
    other_total += OtherUs(row);
    for (uint32_t z = 0; z < kZones; ++z) {
      zone_total[z] += row.zone_us[z];
      calls_total[z] += row.zone_calls[z];
    }
  }
  std::sort(times.begin(), times.end());
  // Nearest-rank percentile: the smallest value with at least p of the rows at or below it.
  auto percentile = [&times](double p) {
    size_t rank = static_cast<size_t>(p * static_cast<double>(times.size()) + 0.999999);
    rank = std::clamp<size_t>(rank, 1, times.size());
    return static_cast<float>(times[rank - 1] / 1000.0);
  };
  out->frames = static_cast<uint32_t>(rows.size());
  out->frame_ms_mean = static_cast<float>(frame_total / count / 1000.0);
  out->frame_ms_p50 = percentile(0.50);
  out->frame_ms_p95 = percentile(0.95);
  out->frame_ms_p99 = percentile(0.99);
  out->frame_ms_max = static_cast<float>(times.back() / 1000.0);
  out->other_ms_mean = static_cast<float>(other_total / count / 1000.0);
  for (uint32_t z = 0; z < kZones; ++z) {
    out->zone_ms_mean[z] = static_cast<float>(zone_total[z] / count / 1000.0);
    out->zone_calls_mean[z] = static_cast<float>(calls_total[z] / count);
  }
  return out->frames;
}

void rex_frame_recorder_reset(uint32_t capacity) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.capacity = capacity ? capacity : kDefaultCapacity;
  g.rows.clear();
  g.rows.shrink_to_fit();
  g.written = 0;
  g.frame_index = 0;
  g.have_last = false;
  g.have_epoch = false;
}

}  // extern "C"
