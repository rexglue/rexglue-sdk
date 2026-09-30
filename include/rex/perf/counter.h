/**
 * @file        perf/counter.h
 * @brief       Performance counter registry and profiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <string>

#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#endif

namespace rex::platform {

enum class CounterId : uint16_t {
  // Frame
  kFrameTimeUs,
  kFps,

  // GPU
  kDrawCalls,
  kCommandBufferStalls,
  kVerticesProcessed,

  // Audio
  kXMAFramesDecoded,
  kAudioFrameLatencyUs,
  kBufferQueueDepth,

  // Dispatch
  kFunctionsDispatched,
  kInterruptDispatches,

  // Threading
  kActiveThreads,
  kAPCQueueDepth,
  kCriticalRegionContentions,

  // Caches
  kTextureCacheHits,
  kTextureCacheMisses,
  kPipelineCacheHits,
  kPipelineCacheMisses,

  kCount  // sentinel -- must be last
};

// Returns human-readable name for a counter (e.g. "frame_time_us")
const char* CounterName(CounterId id);

// Set a counter to an absolute value
void SetCounter(CounterId id, int64_t value);

// Atomically add to a counter
void IncrementCounter(CounterId id, int64_t delta = 1);

// Read a counter's current live value
int64_t GetCounter(CounterId id);

// Snapshot current values into the read buffer and zero the live counters.
// Called once per frame by Profiler::Flip().
void ResetFrameCounters();

// Read a counter from the last-frame snapshot (stable between frames).
int64_t GetSnapshotCounter(CounterId id);

// Initialize the counter system (zeroes everything). Safe to call multiple times.
void Init();

// CSV logging
void SetCSVLogPath(const std::string& path);
void WriteCSVFrame();
void FlushCSV();

// Profiler -- coordinates Tracy frame marks and counter snapshots.
// Moved here from rex::debug to consolidate all perf code under rex::perf.
class Profiler {
 public:
  static void Startup() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (!tracy::IsProfilerStarted())
      tracy::StartupProfiler();
#endif
  }
  static void OnThreadEnter(const char* name = nullptr) {
#ifdef REXGLUE_ENABLE_PROFILING
    if (name && tracy::IsProfilerStarted())
      tracy::SetThreadName(name);
#else
    (void)name;
#endif
  }
  static void OnThreadExit() {}
  static void ThreadEnter(const char* name = nullptr) { OnThreadEnter(name); }
  static void ThreadExit() {}
  static void Flip() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted()) {
      FrameMark;
    }
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ResetFrameCounters();
    WriteCSVFrame();
#endif
  }
  static void Flush() {}
  static void Shutdown() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted())
      tracy::ShutdownProfiler();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    FlushCSV();
#endif
  }
  static bool is_enabled() {
#ifdef REXGLUE_ENABLE_PROFILING
    return tracy::IsProfilerStarted();
#else
    return false;
#endif
  }
};

}  // namespace rex::platform

#ifdef REXGLUE_ENABLE_PERF_COUNTERS

/**
 * Sets a perf counter to a value. Compiles to nothing when counters are disabled.
 *
 * @param id A rex::platform::CounterId enumerator name without its qualifier.
 * @param value The new value.
 *
 * @code
 * REX_PERF_SET(kFps, frames_per_second);
 * @endcode
 */
#define REX_PERF_SET(id, value) ::rex::platform::SetCounter(::rex::platform::CounterId::id, value)

/**
 * Adds 1 to a perf counter. Compiles to nothing when counters are disabled.
 *
 * @param id A rex::platform::CounterId enumerator name without its qualifier.
 *
 * @code
 * REX_PERF_INC(kDrawCalls);
 * @endcode
 */
#define REX_PERF_INC(id) ::rex::platform::IncrementCounter(::rex::platform::CounterId::id)

/**
 * Adds a delta to a perf counter. Compiles to nothing when counters are disabled.
 *
 * @param id A rex::platform::CounterId enumerator name without its qualifier.
 * @param delta The signed amount to add.
 *
 * @code
 * REX_PERF_ADD(kVerticesProcessed, vertex_count);
 * @endcode
 */
#define REX_PERF_ADD(id, delta) \
  ::rex::platform::IncrementCounter(::rex::platform::CounterId::id, delta)

#else

#define REX_PERF_SET(id, value)
#define REX_PERF_INC(id)
#define REX_PERF_ADD(id, delta)

#endif
