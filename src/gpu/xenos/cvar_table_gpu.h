/**
 * @file        gpu/xenos/cvar_table_gpu.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::gpu::cvars {

REXCVAR_BOOL(gpu, vsync, true, "Enable vertical sync");
REXCVAR_INT32(gpu, resolution_scale_x, 1, "Draw resolution scale X (1 = no scaling)")
    .SetRange(1, 8)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_INT32(gpu, resolution_scale_y, 1, "Draw resolution scale Y (1 = no scaling)")
    .SetRange(1, 8)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(gpu, present_letterbox, true, "Enable letterboxing for non-native aspect ratios");
REXCVAR_INT32(gpu, present_safe_area_x, 90, "Horizontal safe area percentage (0-100)")
    .SetRange(0, 100);
REXCVAR_INT32(gpu, present_safe_area_y, 90, "Vertical safe area percentage (0-100)")
    .SetRange(0, 100);
REXCVAR_BOOL(gpu, present_dither, false, "Enable output dithering in the final present pass")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(gpu, present_allow_overscan_cutoff, false,
             "Allow overscan cutoff based on safe area settings")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_TABLE(kGPUCvars, vsync, resolution_scale_x, resolution_scale_y, present_letterbox,
              present_safe_area_x, present_safe_area_y, present_dither,
              present_allow_overscan_cutoff);

}  // namespace rex::gpu::cvars
