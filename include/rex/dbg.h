/**
 * @file        dbg.h
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
#include <string_view>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>

namespace rex::platform {

/**
 * Reports whether a debugger is attached to this process. The state can change at any time. The
 * check is expensive.
 *
 * @return True when a debugger is attached to this process.
 */
REX_API bool IsDebuggerAttached();

/**
 * Breaks into the attached debugger. Without a debugger, Windows raises a breakpoint exception and
 * POSIX raises SIGTRAP.
 */
REX_API void Break();

}  // namespace rex::platform

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
 * Disabled fiber macros without TRACY_FIBERS. The enter form type checks the name without
 * evaluating it, and the leave form expands to nothing.
 *
 * @param name The fiber name, never evaluated.
 *
 * @code
 * REX_PROFILE_FIBER_ENTER(fiber->name());
 * REX_PROFILE_FIBER_LEAVE();
 * @endcode
 */
#define REX_PROFILE_FIBER_ENTER(name) ((void)sizeof(decltype(name)))
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
 * @param ptr The address returned by the allocation.
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
 * @param ptr The address that was freed.
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
 * @param ptr The address returned by the allocation.
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
 * @param ptr The address that was freed.
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
 * @param ptr The address returned by the allocation.
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
 * @param ptr The address that was freed.
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
 * @param ptr The address returned by the allocation.
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
 * @param ptr The address that was freed.
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
 * Disabled profiling macros without REXGLUE_ENABLE_PROFILING. REX_PROFILE_FIBER_ENTER type checks
 * its name without evaluating it, and the others expand to nothing.
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
