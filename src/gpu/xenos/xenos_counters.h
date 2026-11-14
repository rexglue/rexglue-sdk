/**
 * @file        gpu/xenos/xenos_counters.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>

#include <rex/logging/log_category.h>
#include <rex/platform/perf.h>

namespace rex::gpu {

/**
 * Registers the Xenos driver counters. Repeated calls change nothing. A counter that does not fit
 * the table logs a warning and is left out.
 */
inline void RegisterXenosCounters() {
  constexpr std::array<const char*, 7> kNames = {
      "draw_calls",
      "command_buffer_stalls",
      "vertices_processed",
      "texture_cache_hits",
      "texture_cache_misses",
      "pipeline_cache_hits",
      "pipeline_cache_misses",
  };
  for (const char* name : kNames) {
    if (auto counter = platform::RegisterCounter(name); !counter) {
      REXGPU_WARN("{}", counter.error().message);
    }
  }
}

}  // namespace rex::gpu
