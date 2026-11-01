/**
 * @file        tests/unit/base/profile_macros_on/profiling_enabled.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string>

#include <rex/platform/perf.h>

#if defined(REXGLUE_ENABLE_PROFILING) && !defined(TRACY_ENABLE)
#error Profiling is enabled without Tracy
#endif

void ProfilingEnabledMemory([[maybe_unused]] void* block,
                            [[maybe_unused]] unsigned long long size) {
  REX_PROFILE_ALLOC(block, size);
  REX_PROFILE_FREE(block);
  REX_PROFILE_ALLOC_NAMED(block, size, "ProfilingEnabledMemory");
  REX_PROFILE_FREE_NAMED(block, "ProfilingEnabledMemory");
  REX_PROFILE_ALLOC_STACK(block, size, 8);
  REX_PROFILE_FREE_STACK(block, 8);
  REX_PROFILE_ALLOC_NAMED_STACK(block, size, 8, "ProfilingEnabledMemory");
  REX_PROFILE_FREE_NAMED_STACK(block, 8, "ProfilingEnabledMemory");
}

bool ProfilingEnabledZones([[maybe_unused]] const std::string& name) {
  REX_PROFILE_SCOPE("ProfilingEnabledZones");
  REX_PROFILE_SCOPE_TEXT("ProfilingEnabledZones Text", "detail");
  REX_PROFILE_SCOPE_DYNAMIC(name);
  REX_PROFILE_THREAD("ProfilingEnabledZones");
  REX_PROFILE_PLOT("ProfilingEnabledZones Integer", 3);
  REX_PROFILE_PLOT_F64("ProfilingEnabledZones Float", 0.5);
  REX_PROFILE_FRAME();
  return REX_PROFILE_IS_CONNECTED();
}
