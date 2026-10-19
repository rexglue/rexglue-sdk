#include <rex/perf/counter.h>

#include <rex/cvar.h>
#include <rex/logging/log.h>
#include <rex/string.h>

#include "core/cvar_table.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>

namespace rex::platform {

namespace {

constexpr size_t kNumCounters = static_cast<size_t>(CounterId::kCount);

std::array<std::atomic<i64>, kNumCounters> g_counters{};
std::array<std::atomic<i64>, kNumCounters> g_snapshot{};

constexpr const char* kCounterNames[] = {
    "frame_time_us",
    "fps",
    "draw_calls",
    "command_buffer_stalls",
    "vertices_processed",
    "xma_frames_decoded",
    "audio_frame_latency_us",
    "buffer_queue_depth",
    "functions_dispatched",
    "interrupt_dispatches",
    "active_threads",
    "apc_queue_depth",
    "critical_region_contentions",
    "texture_cache_hits",
    "texture_cache_misses",
    "pipeline_cache_hits",
    "pipeline_cache_misses",
};
static_assert(std::size(kCounterNames) == kNumCounters, "kCounterNames must match CounterId enum");

constexpr bool IsGauge(size_t index) {
  auto id = static_cast<CounterId>(index);
  return id == CounterId::kActiveThreads || id == CounterId::kCriticalRegionContentions;
}

std::ofstream& CSVFile() {
  static std::ofstream file;
  return file;
}

int g_csv_frame_count = 0;

}  // namespace

const char* GetCounterName(CounterId id) {
  auto idx = static_cast<size_t>(id);
  if (idx < kNumCounters)
    return kCounterNames[idx];
  return "unknown";
}

void SetCounter(CounterId id, i64 value) {
  g_counters[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

void IncrementCounter(CounterId id, i64 delta) {
  g_counters[static_cast<size_t>(id)].fetch_add(delta, std::memory_order_relaxed);
}

i64 GetCounter(CounterId id) {
  return g_counters[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void ResetFrameCounters() {
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (IsGauge(i)) {
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
  }
}

i64 GetSnapshotCounter(CounterId id) {
  return g_snapshot[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void InitCounters() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
}

void SetCSVLogPath(const std::string& path) {
  auto& csv = CSVFile();
  if (csv.is_open()) {
    csv.close();
  }
  g_csv_frame_count = 0;

  if (path.empty())
    return;

  csv.open(rex::to_path(path), std::ios::out | std::ios::trunc);
  if (!csv) {
    REXLOG_WARN(core, "perf: failed to open CSV log at {}", path);
    return;
  }

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      csv << ',';
    csv << kCounterNames[i];
  }
  csv << '\n';
}

void WriteCSVFrame() {
  auto& csv = CSVFile();
  if (!csv.is_open())
    return;

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      csv << ',';
    csv << std::format("{}", g_snapshot[i].load(std::memory_order_relaxed));
  }
  csv << '\n';

  if (++g_csv_frame_count % 60 == 0) {
    csv.flush();
  }
}

void CloseCSV() {
  if (CSVFile().is_open()) {
    CSVFile().close();
  }
}

}  // namespace rex::platform
