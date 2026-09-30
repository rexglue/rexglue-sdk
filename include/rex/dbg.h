/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <fmt/format.h>

#include <cstring>

namespace rex::platform {

// Returns true if a debugger is attached to this process.
// The state may change at any time (attach after launch, etc), so do not
// cache this value. Determining if the debugger is attached is expensive,
// though, so avoid calling it frequently.
bool IsDebuggerAttached();

// Breaks into the debugger if it is attached.
// If no debugger is present, a signal will be raised.
void Break();

namespace detail {
void DebugPrint(const char* s);
}

// Prints a message to the attached debugger.
// This bypasses the normal logging mechanism. If no debugger is attached it's
// likely to no-op.
template <typename... Args>
void DebugPrint(fmt::string_view format, const Args&... args) {
  detail::DebugPrint(fmt::vformat(format, fmt::make_format_args(args...)).c_str());
}

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
 * REX_PROFILE_SCOPE_TEXT("AudioSystem::Submit", client_name);
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
#define REX_PROFILE_PLOT(name, value)               \
  do {                                              \
    if (TracyIsStarted)                             \
      TracyPlot(name, static_cast<int64_t>(value)); \
  } while (0)

#else  // !REXGLUE_ENABLE_PROFILING

#define REX_PROFILE_SCOPE(name)
#define REX_PROFILE_SCOPE_TEXT(name, detail)
#define REX_PROFILE_THREAD(name)
#define REX_PROFILE_FIBER_ENTER(name)
#define REX_PROFILE_FIBER_LEAVE()
#define REX_PROFILE_PLOT(name, value)

#endif  // REXGLUE_ENABLE_PROFILING
