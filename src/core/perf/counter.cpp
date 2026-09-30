/**
 * @file        core/perf/counter.cpp
 * @brief       Performance counter registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/counter.h>

#include <rex/cvar.h>
#include <rex/logging.h>
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

std::ofstream g_csv_file;
std::string g_csv_path;
int g_csv_frame_count = 0;

}  // namespace

const char* CounterName(CounterId id) {
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
  if (g_csv_file.is_open()) {
    g_csv_file.close();
  }
  g_csv_path = path;
  g_csv_frame_count = 0;

  if (path.empty())
    return;

  g_csv_file.open(rex::to_path(path), std::ios::out | std::ios::trunc);
  if (!g_csv_file) {
    REXLOG_WARN("perf: failed to open CSV log: {}", path);
    g_csv_path.clear();
    return;
  }

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      g_csv_file << ',';
    g_csv_file << kCounterNames[i];
  }
  g_csv_file << '\n';
}

void WriteCSVFrame() {
  if (!g_csv_file.is_open())
    return;

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      g_csv_file << ',';
    g_csv_file << std::format("{}", g_snapshot[i].load(std::memory_order_relaxed));
  }
  g_csv_file << '\n';

  if (++g_csv_frame_count % 60 == 0) {
    g_csv_file.flush();
  }
}

void FlushCSV() {
  if (g_csv_file.is_open()) {
    g_csv_file.close();
  }
  g_csv_path.clear();
}

}  // namespace rex::platform
