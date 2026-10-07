/**
 * @file        dbg.h
 * @brief       Debugger queries, breakpoints, debugger output and Tracy profiling macros
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
#include <format>

#include <rex/macros.h>
#include <rex/types.h>

namespace rex::platform {

/**
 * Checks for an attached debugger. The state can change at any time, so do not cache it. The
 * check is expensive, so do not call it often.
 *
 * @return True when a debugger is attached to this process.
 */
REX_API bool IsDebuggerAttached();

/**
 * Breaks into the attached debugger. Without a debugger, Windows raises a breakpoint exception and
 * POSIX raises SIGTRAP.
 */
REX_API void Break();

namespace detail {
REX_API void DebugPrint(const char* s);
}

/**
 * Formats a message and sends it to the debugger output, bypassing the log. Windows writes it with
 * OutputDebugStringA and POSIX writes it to std::clog.
 *
 * @tparam Args The format argument types.
 * @param format The std::format string, checked at compile time.
 * @param args The format arguments.
 */
template <typename... Args>
void DebugPrint(std::format_string<const Args&...> format, const Args&... args) {
  detail::DebugPrint(std::format(format, args...).c_str());
}

}  // namespace rex::platform

#ifdef REXGLUE_ENABLE_PROFILING

#include <tracy/Tracy.hpp>

#include <rex/macros.h>

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
 * Disabled fiber macros, which expand to nothing without TRACY_FIBERS.
 *
 * @param name Ignored.
 *
 * @code
 * REX_PROFILE_FIBER_ENTER(fiber->name());
 * REX_PROFILE_FIBER_LEAVE();
 * @endcode
 */
#define REX_PROFILE_FIBER_ENTER(name)
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

#else  // !REXGLUE_ENABLE_PROFILING

/**
 * Disabled profiling macros, which expand to nothing without REXGLUE_ENABLE_PROFILING.
 *
 * @param name Ignored.
 * @param detail Ignored.
 * @param value Ignored.
 *
 * @code
 * REX_PROFILE_SCOPE("CommandProcessor::ExecutePacket");
 * @endcode
 */
#define REX_PROFILE_SCOPE(name)
#define REX_PROFILE_SCOPE_TEXT(name, detail)
#define REX_PROFILE_THREAD(name)
#define REX_PROFILE_FIBER_ENTER(name)
#define REX_PROFILE_FIBER_LEAVE()
#define REX_PROFILE_PLOT(name, value)

#endif  // REXGLUE_ENABLE_PROFILING
