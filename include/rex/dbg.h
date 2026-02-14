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

// Stub Profiler class for compatibility
class Profiler {
 public:
  static void OnThreadEnter(const char* name = nullptr) { (void)name; }
  static void OnThreadExit() {}
  static void ThreadEnter(const char* name = nullptr) { (void)name; }
  static void ThreadExit() {}
  static void Flip() {}
  static void Flush() {}
  static void Shutdown() {}
  static bool is_enabled() { return false; }
};

}  // namespace rex::debug
