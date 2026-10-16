/**
 * @file        gpu/xenos/cvar_table_pair.h
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

REXCVAR_INT32(gpu, anisotropic_override, 3,
              "Forces anisotropic filtering for eligible textures.\nHigher values keep textures "
              "sharper at oblique angles, but increase texture sampling cost.\n -1 = No override\n "
              " 0 = Disable anisotropic filtering\n  1 = Force 1x anisotropic filtering\n  2 = "
              "Force 2x anisotropic filtering\n  3 = Force 4x anisotropic filtering\n  4 = Force "
              "8x anisotropic filtering\n  5 = Force 16x anisotropic filtering")
    .SetRange(-1, 5);
REXCVAR_BOOL(xenos, clear_memory_page_state, true,
             "Refresh page-valid state from GPU-written memory at frame end. Disable for minor CPU "
             "overhead reduction, but may break memory coherency.");
REXCVAR_BOOL(xenos, occlusion_query_enable, true, "Enable host occlusion query handling");
REXCVAR_BOOL(xenos, readback_resolve_half_pixel_offset, false,
             "When draw resolution scaling is active, sample from the center of each scaled block "
             "during resolve readback downscale");
REXCVAR_BOOL(xenos, readback_memexport_fast, true,
             "Use fast double-buffered memexport readback when possible, with automatic fallback "
             "to full synchronous readback");
REXCVAR_BOOL(
    xenos, async_shader_compilation, true,
    "Compile shaders and create pipelines asynchronously in background threads. This reduces "
    "stutter but may cause brief visual artifacts while pipelines are being prepared.");
REXCVAR_BOOL(xenos, native_2x_msaa, true, "Enable native 2x MSAA");
REXCVAR_BOOL(xenos, depth_float24_round, false, "Round float24 depth values");
REXCVAR_BOOL(xenos, depth_float24_convert_in_pixel_shader, false,
             "Convert float24 depth in pixel shader");
REXCVAR_BOOL(xenos, depth_transfer_not_equal_test, true, "Use not-equal test for depth transfer");
REXCVAR_BOOL(xenos, gamma_render_target_as_unorm16, true,
             "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)");
REXCVAR_BOOL(xenos, snorm16_render_target_full_range, true,
             "Use full range for SNORM16 render targets");
REXCVAR_BOOL(xenos, direct_host_resolve, true,
             "Resolve from host render targets directly to shared memory when possible");
REXCVAR_BOOL(xenos, texture_3d_to_2d, true,
             "Sample problematic 3D textures through 2D-compatible wrappers");

REXCVAR_TABLE(kPairCvars, anisotropic_override, clear_memory_page_state, occlusion_query_enable,
              readback_resolve_half_pixel_offset, readback_memexport_fast, async_shader_compilation,
              native_2x_msaa, depth_float24_round, depth_float24_convert_in_pixel_shader,
              depth_transfer_not_equal_test, gamma_render_target_as_unorm16,
              snorm16_render_target_full_range, direct_host_resolve, texture_3d_to_2d);

}  // namespace rex::gpu::cvars
