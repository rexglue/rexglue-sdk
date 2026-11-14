/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/types.h>

namespace rex::gpu::pipeline_priority {

// Higher values compile sooner.
constexpr u8 kPriorityLowest = 0; /**< Writes to unbound RTs only. */
constexpr u8 kPriorityDepthOnly = 1;
constexpr u8 kPriorityVisibleRT = 2;
constexpr u8 kPriorityRT0 = 3;

/**
 * Converts a normalized color mask to a 4 bit mask of bound RTs.
 *
 * @param normalized_color_mask The color mask with 4 bits per RT (RGBA).
 */
inline u32 GetBoundRTMaskFromNormalizedColorMask(u32 normalized_color_mask) {
  return (((normalized_color_mask >> 0) & 0xF) ? 1 : 0) |
         (((normalized_color_mask >> 4) & 0xF) ? 2 : 0) |
         (((normalized_color_mask >> 8) & 0xF) ? 4 : 0) |
         (((normalized_color_mask >> 12) & 0xF) ? 8 : 0);
}

/**
 * Returns the async compilation priority from shader outputs.
 *
 * @param shader_writes_color_targets The mask of color targets the shader writes.
 * @param shader_writes_depth True when the shader writes depth.
 */
inline u8 CalculatePipelinePriority(u32 bound_rts, u32 shader_writes_color_targets,
                                    bool shader_writes_depth) {
  u32 visible_writes = bound_rts & shader_writes_color_targets;
  if (visible_writes) {
    return (visible_writes & 1) ? kPriorityRT0 : kPriorityVisibleRT;
  }
  if (shader_writes_depth) {
    return kPriorityDepthOnly;
  }
  return kPriorityLowest;
}

}  // namespace rex::gpu::pipeline_priority
