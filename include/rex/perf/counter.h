/**
 * @file        perf/counter.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>

#include <rex/macros.h>
#include <rex/types.h>

#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#endif

namespace rex::platform {

/**
 * Performance counter slots. ResetFrameCounters zeroes accumulators each frame and keeps gauges.
 */
enum class CounterId : u16 {
  kFrameTimeUs,               /**< Last frame time in microseconds. */
  kFps,                       /**< Frames per second. */
  kDrawCalls,                 /**< GPU draw calls this frame. */
  kCommandBufferStalls,       /**< GPU command buffer stalls this frame. */
  kVerticesProcessed,         /**< GPU vertices processed this frame. */
  kXMAFramesDecoded,          /**< XMA audio frames decoded this frame. */
  kAudioFrameLatencyUs,       /**< Audio frame latency in microseconds. */
  kBufferQueueDepth,          /**< Queued audio buffers. */
  kFunctionsDispatched,       /**< Functions dispatched this frame. */
  kInterruptDispatches,       /**< Interrupts dispatched this frame. */
  kActiveThreads,             /**< Live guest threads, a gauge. */
  kAPCQueueDepth,             /**< APCs queued this frame. */
  kCriticalRegionContentions, /**< Critical region contentions, a running total gauge. */
  kTextureCacheHits,          /**< Texture cache hits this frame. */
  kTextureCacheMisses,        /**< Texture cache misses this frame. */
  kPipelineCacheHits,         /**< Pipeline cache hits this frame. */
  kPipelineCacheMisses,       /**< Pipeline cache misses this frame. */
  kCount                      /**< Number of counters. It stays last. */
};

/**
 * @param id The counter.
 * @return The counter name in snake case, for example "frame_time_us", or "unknown".
 */
REX_API const char* CounterName(CounterId id);

/**
 * Sets the live value of a counter.
 *
 * @param id The counter.
 * @param value The new value.
 */
REX_API void SetCounter(CounterId id, i64 value);

/**
 * Atomically adds to the live value of a counter.
 *
 * @param id The counter.
 * @param delta The signed amount to add.
 */
REX_API void IncrementCounter(CounterId id, i64 delta = 1);

/**
 * @param id The counter.
 * @return The live value of the counter.
 */
REX_API i64 GetCounter(CounterId id);

/**
 * Copies every live value into the snapshot and zeroes the live accumulators. Gauges keep their
 * live values. Profiler::Flip calls it once per frame.
 */
REX_API void ResetFrameCounters();

/**
 * @param id The counter.
 * @return The counter value from the last snapshot, stable between frames.
 */
REX_API i64 GetSnapshotCounter(CounterId id);

/**
 * Zeroes every live and snapshot value. Repeated calls are safe.
 */
REX_API void InitCounters();

/**
 * Closes any open CSV log and opens a new one at the path with a header row of counter names.
 *
 * @param path The CSV file path. Empty closes logging.
 */
REX_API void SetCSVLogPath(const std::string& path);

/**
 * Appends a row of snapshot values to the CSV log. The file is flushed every 60 rows.
 */
REX_API void WriteCSVFrame();

/**
 * Flushes and closes the CSV log.
 */
REX_API void FlushCSV();

/**
 * Static hooks that emit Tracy frame marks and take the per-frame counter snapshot.
 */
class Profiler {
 public:
  /**
   * Starts the Tracy profiler when profiling is compiled in and it is not running.
   */
  static void Startup() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (!tracy::IsProfilerStarted())
      tracy::StartupProfiler();
#endif
  }

  /**
   * Names the calling thread in Tracy when the profiler is running.
   *
   * @param name The thread name. Null leaves the name unchanged.
   */
  static void OnThreadEnter(const char* name = nullptr) {
#ifdef REXGLUE_ENABLE_PROFILING
    if (name && tracy::IsProfilerStarted())
      tracy::SetThreadName(name);
#else
    (void)name;
#endif
  }

  /**
   * Thread exit hook. It does nothing.
   */
  static void OnThreadExit() {}

  /**
   * Same as OnThreadEnter.
   *
   * @param name The thread name. Null leaves the name unchanged.
   */
  static void ThreadEnter(const char* name = nullptr) { OnThreadEnter(name); }

  /**
   * Thread exit hook. It does nothing.
   */
  static void ThreadExit() {}

  /**
   * Ends a frame. It emits a Tracy frame mark and, with perf counters compiled in, snapshots the
   * counters and writes a CSV row.
   */
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

  /**
   * Flush hook. It does nothing.
   */
  static void Flush() {}

  /**
   * Stops the Tracy profiler and, with perf counters compiled in, closes the CSV log.
   */
  static void Shutdown() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted())
      tracy::ShutdownProfiler();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    FlushCSV();
#endif
  }

  /**
   * @return True when profiling is compiled in and the Tracy profiler is running.
   */
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

/**
 * Disabled perf counter macros, which expand to nothing without REXGLUE_ENABLE_PERF_COUNTERS.
 *
 * @param id Ignored.
 * @param value Ignored.
 * @param delta Ignored.
 *
 * @code
 * REX_PERF_INC(kDrawCalls);
 * @endcode
 */
#define REX_PERF_SET(id, value)
#define REX_PERF_INC(id)
#define REX_PERF_ADD(id, delta)

#endif
