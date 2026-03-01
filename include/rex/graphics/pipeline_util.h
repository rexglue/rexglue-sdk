#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>

namespace rex::graphics::pipeline_util {

// Priority levels for async pipeline compilation.
// Higher values are compiled sooner.
constexpr uint8_t kPriorityLowest = 0;     // Writes to unbound RTs only.
constexpr uint8_t kPriorityDepthOnly = 1;  // Depth-only writes.
constexpr uint8_t kPriorityVisibleRT = 2;  // Writes to a visible RT.
constexpr uint8_t kPriorityRT0 = 3;        // Writes to RT0.

// Converts normalized_color_mask to a 4-bit bitmask of bound RTs.
// normalized_color_mask contains 4 bits per RT (RGBA).
inline uint32_t GetBoundRTMaskFromNormalizedColorMask(uint32_t normalized_color_mask) {
  return (((normalized_color_mask >> 0) & 0xF) ? 1 : 0) |
         (((normalized_color_mask >> 4) & 0xF) ? 2 : 0) |
         (((normalized_color_mask >> 8) & 0xF) ? 4 : 0) |
         (((normalized_color_mask >> 12) & 0xF) ? 8 : 0);
}

// Calculates async compilation priority from shader outputs.
inline uint8_t CalculatePipelinePriority(uint32_t bound_rts, uint32_t shader_writes_color_targets,
                                         bool shader_writes_depth) {
  uint32_t visible_writes = bound_rts & shader_writes_color_targets;
  if (visible_writes) {
    return (visible_writes & 1) ? kPriorityRT0 : kPriorityVisibleRT;
  }
  if (shader_writes_depth) {
    return kPriorityDepthOnly;
  }
  return kPriorityLowest;
}

}  // namespace rex::graphics::pipeline_util
