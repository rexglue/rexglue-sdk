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

namespace rex::debug {

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

}  // namespace rex::debug

#ifdef REXGLUE_ENABLE_PROFILING

#include <tracy/Tracy.hpp>

// CPU profiling zones
#define SCOPE_profile_cpu_f(name) ZoneNamedN(___tracy_cpu_zone, name, true)
#define SCOPE_profile_cpu_i(name, detail)      \
  ZoneNamedN(___tracy_cpu_zone_i, name, true); \
  ZoneTextV(___tracy_cpu_zone_i, detail, std::strlen(detail))

// GPU profiling stubs -- backend code uses TracyVkZone/TracyD3D12Zone directly.
#define SCOPE_profile_gpu_f(name)
#define SCOPE_profile_gpu_i(name, detail)

// Thread profiling
#define PROFILE_THREAD_ENTER(name) tracy::SetThreadName(name)
#define PROFILE_THREAD_EXIT()

// Counter profiling -- plot to Tracy
#define COUNT_profile_set(name, value) TracyPlot(name, static_cast<int64_t>(value))
#define COUNT_profile_add(name, value) TracyPlot(name, static_cast<int64_t>(value))

#else  // !REXGLUE_ENABLE_PROFILING

// CPU profiling stubs
#define SCOPE_profile_cpu_f(name)
#define SCOPE_profile_cpu_i(name, detail)

// GPU profiling stubs
#define SCOPE_profile_gpu_f(name)
#define SCOPE_profile_gpu_i(name, detail)

// Thread profiling stubs
#define PROFILE_THREAD_ENTER(name)
#define PROFILE_THREAD_EXIT()

// Counter profiling stubs
#define COUNT_profile_set(name, value)
#define COUNT_profile_add(name, value)

#endif  // REXGLUE_ENABLE_PROFILING
