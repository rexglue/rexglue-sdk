/**
 * @file        platform/perf.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/debugging.h (Ben Vanik, 2014).
 */

#pragma once

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::platform {

/** Performance counter slots. A module registers its own counters with RegisterCounter. */
enum class CounterID : u8 {
  kFrameTimeUs,
  kFps,
};

/** How ResetFrameCounters treats a counter. */
enum class CounterType : u8 {
  kAccumulator, /**< Zeroed each frame after its value is snapshotted. */
  kGauge,       /**< Keeps its live value across frames. */
};

/** The number of counter slots, registered counters included. */
inline constexpr size_t kMaxCounters = 64;

/**
 * Registers a counter, or finds the one registered under the name. CSV columns follow
 * registration order.
 *
 * @param name The snake case name, shorter than 48 characters.
 * @param type How ResetFrameCounters treats the counter. A repeated name keeps its first type.
 * @return The counter, or an error when all kMaxCounters slots are used and the name is new.
 */
[[nodiscard]] REX_API Result<CounterID> RegisterCounter(
    const char* name, CounterType type = CounterType::kAccumulator);

/**
 * Finds a registered counter.
 *
 * @return The counter, or empty when no counter has the name.
 */
REX_API std::optional<CounterID> FindCounter(std::string_view name);

/**
 * @return The snake case name, or "unknown".
 */
REX_API const char* GetCounterName(CounterID id);

REX_API void SetCounter(CounterID id, i64 value);

/**
 * Atomically adds to the live value of a counter.
 *
 * @param delta The signed amount to add.
 */
REX_API void IncrementCounter(CounterID id, i64 delta = 1);

/** Returns the live value of a counter. */
REX_API i64 GetCounter(CounterID id);

/** Copies every live value into the snapshot and zeroes the accumulators. Gauges keep theirs. */
REX_API void ResetFrameCounters();

/** Returns the value of a counter from the last snapshot, which stays stable between frames. */
REX_API i64 GetSnapshotCounter(CounterID id);

/** Zeroes every live and snapshot value. */
REX_API void InitCounters();

/**
 * Closes any open CSV log and opens a new one with a header row of counter names.
 *
 * @param path The CSV file path. Empty closes logging.
 */
REX_API void SetCSVLogPath(const std::string& path);

/** Appends a row of snapshot values to the CSV log. The file is flushed every 60 rows. */
REX_API void WriteCSVFrame();

REX_API void CloseCSV();

}  // namespace rex::platform

#ifdef REXGLUE_ENABLE_PERF_COUNTERS

/**
 * Looks the counter up once per call site, then applies an operation. Does nothing when the name
 * is not registered.
 *
 * @param name The snake case counter name, a string literal.
 * @param call The operation, which sees the counter as rex_perf_id.
 */
#define REX_PERF_APPLY(name, call)                                         \
  do {                                                                     \
    static const auto rex_perf_found = ::rex::platform::FindCounter(name); \
    if (rex_perf_found) {                                                  \
      const ::rex::platform::CounterID rex_perf_id = *rex_perf_found;      \
      call;                                                                \
    }                                                                      \
  } while (false)

/**
 * Sets a perf counter to a value. Compiles to nothing when counters are disabled.
 *
 * @param name The counter name in snake case, a string literal.
 * @param value Evaluated only when the counter exists.
 *
 * @code
 * REX_PERF_SET("fps", frames_per_second);
 * @endcode
 */
#define REX_PERF_SET(name, value) \
  REX_PERF_APPLY(name, ::rex::platform::SetCounter(rex_perf_id, value))

/**
 * Adds 1 to a perf counter. Compiles to nothing when counters are disabled.
 *
 * @param name The counter name in snake case, a string literal.
 *
 * @code
 * REX_PERF_INC("draw_calls");
 * @endcode
 */
#define REX_PERF_INC(name) REX_PERF_APPLY(name, ::rex::platform::IncrementCounter(rex_perf_id))

/**
 * Adds a delta to a perf counter. Compiles to nothing when counters are disabled.
 *
 * @param name The counter name in snake case, a string literal.
 * @param delta The signed amount to add.
 *
 * @code
 * REX_PERF_ADD("vertices_processed", vertex_count);
 * @endcode
 */
#define REX_PERF_ADD(name, delta) \
  REX_PERF_APPLY(name, ::rex::platform::IncrementCounter(rex_perf_id, delta))

#else

/**
 * Disabled perf counter macros, which expand to nothing without REXGLUE_ENABLE_PERF_COUNTERS.
 *
 * @param name Ignored.
 * @param value Ignored.
 * @param delta Ignored.
 *
 * @code
 * REX_PERF_INC("draw_calls");
 * @endcode
 */
#define REX_PERF_SET(name, value)
#define REX_PERF_INC(name)
#define REX_PERF_ADD(name, delta)

#endif

#ifdef REXGLUE_ENABLE_PROFILING

#include <tracy/Tracy.hpp>

/**
 * Opens a Tracy CPU zone that lasts until the end of the enclosing scope.
 *
 * @param name The zone name, a string literal.
 *
 * @code
 * REX_PROFILE_SCOPE("CommandProcessor::ExecutePacket");
 * @endcode
 */
#define REX_PROFILE_SCOPE(name) ZoneNamedN(___tracy_cpu_zone, name, TracyIsStarted)

/**
 * Opens a Tracy CPU zone with a text annotation that lasts until the end of the enclosing scope.
 *
 * @param name The zone name, a string literal.
 * @param detail A null terminated string attached to the zone.
 *
 * @code
 * REX_PROFILE_SCOPE_TEXT("APU::SubmitFrame", client_name);
 * @endcode
 */
#define REX_PROFILE_SCOPE_TEXT(name, detail)             \
  ZoneNamedN(___tracy_cpu_zone_i, name, TracyIsStarted); \
  ZoneTextV(___tracy_cpu_zone_i, detail, std::strlen(detail))

/**
 * Names the calling thread in the profiler.
 *
 * @param name A null terminated thread name.
 *
 * @code
 * REX_PROFILE_THREAD("XMA Decoder");
 * @endcode
 */
#define REX_PROFILE_THREAD(name)  \
  do {                            \
    if (TracyIsStarted)           \
      tracy::SetThreadName(name); \
  } while (0)

#ifdef TRACY_FIBERS
/**
 * Marks the calling thread as running the named fiber.
 *
 * @param name A null terminated fiber name that stays valid while the fiber exists.
 *
 * @code
 * REX_PROFILE_FIBER_ENTER(fiber->name());
 * @endcode
 */
#define REX_PROFILE_FIBER_ENTER(name) \
  do {                                \
    if (TracyIsStarted)               \
      TracyFiberEnter(name);          \
  } while (0)

/**
 * Marks the calling thread as leaving its current fiber.
 *
 * @code
 * REX_PROFILE_FIBER_LEAVE();
 * @endcode
 */
#define REX_PROFILE_FIBER_LEAVE() \
  do {                            \
    if (TracyIsStarted)           \
      TracyFiberLeave;            \
  } while (0)
#else
/**
 * Disabled fiber macros without TRACY_FIBERS. The enter form only type checks its name.
 *
 * @param name The fiber name, never evaluated.
 *
 * @code
 * REX_PROFILE_FIBER_ENTER(fiber->name());
 * REX_PROFILE_FIBER_LEAVE();
 * @endcode
 */
#define REX_PROFILE_FIBER_ENTER(name) ((void)sizeof(decltype(name)))
/**
 * Expands to nothing.
 *
 * @code
 * REX_PROFILE_FIBER_LEAVE();
 * @endcode
 */
#define REX_PROFILE_FIBER_LEAVE()
#endif

/**
 * Plots a value on a named Tracy graph.
 *
 * @param name The plot name, a string literal.
 * @param value The value, converted to a 64 bit integer.
 *
 * @code
 * REX_PROFILE_PLOT("GPU Queue Depth", queue.size());
 * @endcode
 */
#define REX_PROFILE_PLOT(name, value)                  \
  do {                                                 \
    if (TracyIsStarted)                                \
      TracyPlot(name, static_cast<::rex::i64>(value)); \
  } while (0)

/**
 * Plots a floating point value on a named Tracy graph.
 *
 * @param name The plot name, a string literal.
 * @param value The value, converted to a 64 bit float.
 *
 * @code
 * REX_PROFILE_PLOT_F64("Frame Time", frame_seconds);
 * @endcode
 */
#define REX_PROFILE_PLOT_F64(name, value)              \
  do {                                                 \
    if (TracyIsStarted)                                \
      TracyPlot(name, static_cast<::rex::f64>(value)); \
  } while (0)

/**
 * Reports an allocation to the unnamed Tracy memory pool.
 *
 * @param size The size in bytes, evaluated only while the profiler runs.
 *
 * @code
 * REX_PROFILE_ALLOC(block, size);
 * @endcode
 */
#define REX_PROFILE_ALLOC(ptr, size) \
  do {                               \
    if (TracyIsStarted)              \
      TracyAlloc(ptr, size);         \
  } while (0)

/**
 * Reports a free to the unnamed Tracy memory pool.
 *
 * @code
 * REX_PROFILE_FREE(block);
 * @endcode
 */
#define REX_PROFILE_FREE(ptr) \
  do {                        \
    if (TracyIsStarted)       \
      TracyFree(ptr);         \
  } while (0)

/**
 * Reports an allocation to a named Tracy memory pool.
 *
 * @param size The size in bytes, evaluated only while the profiler runs.
 * @param pool The pool name, a string literal or another pointer that stays valid and has the
 * same value for every call of the pool.
 *
 * @code
 * REX_PROFILE_ALLOC_NAMED(block, size, "GPU");
 * @endcode
 */
#define REX_PROFILE_ALLOC_NAMED(ptr, size, pool) \
  do {                                           \
    if (TracyIsStarted)                          \
      TracyAllocN(ptr, size, pool);              \
  } while (0)

/**
 * Reports a free to a named Tracy memory pool.
 *
 * @param pool The pool name given to REX_PROFILE_ALLOC_NAMED.
 *
 * @code
 * REX_PROFILE_FREE_NAMED(block, "GPU");
 * @endcode
 */
#define REX_PROFILE_FREE_NAMED(ptr, pool) \
  do {                                    \
    if (TracyIsStarted)                   \
      TracyFreeN(ptr, pool);              \
  } while (0)

/**
 * Reports an allocation to the unnamed pool with a call stack.
 *
 * @param size The size in bytes, evaluated only while the profiler runs.
 * @param depth The number of stack frames to capture, 0 captures none.
 *
 * @code
 * REX_PROFILE_ALLOC_STACK(block, size, 8);
 * @endcode
 */
#define REX_PROFILE_ALLOC_STACK(ptr, size, depth) \
  do {                                            \
    if (TracyIsStarted)                           \
      TracyAllocS(ptr, size, depth);              \
  } while (0)

/**
 * Reports a free to the unnamed pool with a call stack.
 *
 * @param depth The number of stack frames to capture, 0 captures none.
 *
 * @code
 * REX_PROFILE_FREE_STACK(block, 8);
 * @endcode
 */
#define REX_PROFILE_FREE_STACK(ptr, depth) \
  do {                                     \
    if (TracyIsStarted)                    \
      TracyFreeS(ptr, depth);              \
  } while (0)

/**
 * Reports an allocation to a named pool with a call stack.
 *
 * @param size The size in bytes, evaluated only while the profiler runs.
 * @param depth The number of stack frames to capture, 0 captures none.
 * @param pool The pool name, as for REX_PROFILE_ALLOC_NAMED.
 *
 * @code
 * REX_PROFILE_ALLOC_NAMED_STACK(block, size, 8, "GPU");
 * @endcode
 */
#define REX_PROFILE_ALLOC_NAMED_STACK(ptr, size, depth, pool) \
  do {                                                        \
    if (TracyIsStarted)                                       \
      TracyAllocNS(ptr, size, depth, pool);                   \
  } while (0)

/**
 * Reports a free to a named pool with a call stack.
 *
 * @param depth The number of stack frames to capture, 0 captures none.
 * @param pool The pool name given to REX_PROFILE_ALLOC_NAMED_STACK.
 *
 * @code
 * REX_PROFILE_FREE_NAMED_STACK(block, 8, "GPU");
 * @endcode
 */
#define REX_PROFILE_FREE_NAMED_STACK(ptr, depth, pool) \
  do {                                                 \
    if (TracyIsStarted)                                \
      TracyFreeNS(ptr, depth, pool);                   \
  } while (0)

/**
 * Opens a Tracy CPU zone named at run time that lasts until the end of the enclosing scope.
 *
 * @param name A std::string_view or std::string naming the zone, evaluated once.
 *
 * @code
 * REX_PROFILE_SCOPE_DYNAMIC(function->name());
 * @endcode
 */
#define REX_PROFILE_SCOPE_DYNAMIC(name)                 \
  ZoneNamed(___tracy_cpu_zone_d, TracyIsStarted);       \
  const std::string_view ___tracy_zone_name_d = (name); \
  ZoneNameV(___tracy_cpu_zone_d, ___tracy_zone_name_d.data(), ___tracy_zone_name_d.size())

/**
 * Marks the end of a frame on the main Tracy frame track.
 *
 * @code
 * REX_PROFILE_FRAME();
 * @endcode
 */
#define REX_PROFILE_FRAME() \
  do {                      \
    if (TracyIsStarted)     \
      FrameMark;            \
  } while (0)

/**
 * Reports whether a Tracy profiler is connected.
 *
 * @return True when a profiler is connected, always false without REXGLUE_ENABLE_PROFILING.
 *
 * @code
 * if (REX_PROFILE_IS_CONNECTED()) {
 *   CaptureExtraStatistics();
 * }
 * @endcode
 */
#define REX_PROFILE_IS_CONNECTED() (TracyIsStarted && TracyIsConnected)

#else

/**
 * Disabled profiling macros without REXGLUE_ENABLE_PROFILING. REX_PROFILE_FIBER_ENTER only type
 * checks its name.
 *
 * @param name Ignored, or never evaluated for REX_PROFILE_FIBER_ENTER.
 * @param detail Ignored.
 * @param value Ignored.
 * @param ptr Ignored.
 * @param size Ignored.
 * @param depth Ignored.
 * @param pool Ignored.
 *
 * @code
 * REX_PROFILE_SCOPE("CommandProcessor::ExecutePacket");
 * @endcode
 */
#define REX_PROFILE_SCOPE(name)
#define REX_PROFILE_SCOPE_TEXT(name, detail)
#define REX_PROFILE_THREAD(name)
#define REX_PROFILE_FIBER_ENTER(name) ((void)sizeof(decltype(name)))
#define REX_PROFILE_FIBER_LEAVE()
#define REX_PROFILE_PLOT(name, value)
#define REX_PROFILE_PLOT_F64(name, value)
#define REX_PROFILE_ALLOC(ptr, size)
#define REX_PROFILE_FREE(ptr)
#define REX_PROFILE_ALLOC_NAMED(ptr, size, pool)
#define REX_PROFILE_FREE_NAMED(ptr, pool)
#define REX_PROFILE_ALLOC_STACK(ptr, size, depth)
#define REX_PROFILE_FREE_STACK(ptr, depth)
#define REX_PROFILE_ALLOC_NAMED_STACK(ptr, size, depth, pool)
#define REX_PROFILE_FREE_NAMED_STACK(ptr, depth, pool)
#define REX_PROFILE_SCOPE_DYNAMIC(name)
#define REX_PROFILE_FRAME()
#define REX_PROFILE_IS_CONNECTED() (false)

#endif  // REXGLUE_ENABLE_PROFILING

namespace rex::platform {

/** Static hooks for Tracy frame marks and the per frame counter snapshot. */
class Profiler {
 public:
  /** Starts the Tracy profiler when compiled in and not running. */
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

  static void OnThreadExit() {}

  /** Ends a frame with a Tracy frame mark, a counter snapshot and a CSV row when compiled in. */
  static void EndFrame() {
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

  /** Stops the Tracy profiler and closes the CSV log when compiled in. */
  static void Shutdown() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted())
      tracy::ShutdownProfiler();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    CloseCSV();
#endif
  }

  /** Reports whether the Tracy profiler is running. */
  static bool IsEnabled() {
#ifdef REXGLUE_ENABLE_PROFILING
    return tracy::IsProfilerStarted();
#else
    return false;
#endif
  }
};

}  // namespace rex::platform
