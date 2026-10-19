/**
 * @file        gpu/xenos/cvar_table_xenos.h
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

REXCVAR_STRING(
    xenos, readback_resolve, "none",
    "Controls CPU readback of render-to-texture resolve results.\n none (default) disables "
    "readback\n fast reads the previous frame (delayed, copy every frame)\n some reads the "
    "previous frame (delayed, copy on cache miss)\n full is an immediate sync readback (accurate "
    "but stalls)")
    .SetAllowed(REXCVAR_ALLOWED("none", "fast", "some", "full"));
REXCVAR_BOOL(xenos, readback_memexport, true,
             "Enable CPU readback of shader memexport writes for guest memory coherency (can "
             "reduce correctness issues, but may add GPU/CPU sync cost)");
REXCVAR_INT32(xenos, query_occlusion_fake_sample_count, 1000,
              "Fake sample count for occlusion queries")
    .SetRange(1, 100000);
REXCVAR_BOOL(xenos, allow_invalid_fetch_constants, false, "Allow invalid fetch constants");
REXCVAR_STRING(xenos, dump_shaders, "", "Path to dump shaders to");
REXCVAR_STRING(xenos, swap_post_effect, "none", "Swap post effect (none, fxaa, fxaa_extreme)")
    .SetAllowed(REXCVAR_ALLOWED("none", "fxaa", "fxaa_extreme"))
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(xenos, store_shaders, true,
             "Store shaders persistently and load them when loading games to avoid runtime spikes "
             "and freezes when playing the game not for the first time.");
REXCVAR_BOOL(xenos, mrt_edram_used_range_clamp_to_min, true,
             "Clamp MRT EDRAM used range to minimum");
REXCVAR_BOOL(xenos, execute_unclipped_draw_vs_on_cpu_for_psi_render_backend, true,
             "Execute unclipped draw VS on CPU for PSI render backend");
REXCVAR_INT32(xenos, texture_cache_memory_limit_render_to_texture, 24,
              "Texture cache memory limit for render-to-texture (MB)")
    .SetRange(1, 256)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_INT32(xenos, texture_cache_memory_limit_soft, 384, "Soft texture cache memory limit (MB)")
    .SetRange(64, 4096)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_INT32(xenos, texture_cache_memory_limit_hard, 768, "Hard texture cache memory limit (MB)")
    .SetRange(128, 8192)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_INT32(xenos, texture_cache_memory_limit_soft_lifetime, 30,
              "Soft texture cache memory limit lifetime (seconds)")
    .SetRange(1, 3600);
REXCVAR_BOOL(xenos, pre_mask_resolve_l2_block, true,
             "Pre-mask scaled resolve L2 blocks to the write range before iterating");
REXCVAR_BOOL(xenos, force_convert_line_loops_to_strips, false,
             "Force convert line loops to strips");
REXCVAR_BOOL(xenos, force_convert_quad_lists_to_triangle_lists, false,
             "Force convert quad lists to triangle lists");
REXCVAR_BOOL(xenos, force_convert_triangle_fans_to_lists, false,
             "Force convert triangle fans to lists");
REXCVAR_INT32(xenos, primitive_processor_cache_min_indices, 0,
              "Minimum indices for primitive processor cache")
    .SetRange(0, 1000000);
REXCVAR_BOOL(xenos, half_pixel_offset, true, "Enable half pixel offset");
REXCVAR_BOOL(xenos, resolve_resolution_scale_fill_half_pixel_offset, true,
             "Fill half pixel offset during resolution scale resolve");
REXCVAR_BOOL(xenos, execute_unclipped_draw_vs_on_cpu, false,
             "Execute unclipped draw vertex shader on CPU");
REXCVAR_BOOL(xenos, execute_unclipped_draw_vs_on_cpu_with_scissor, false,
             "Execute unclipped draw VS on CPU with scissor");
REXCVAR_BOOL(xenos, present_from_non_ui_thread, true, "Allow presentation from non-UI thread");

REXCVAR_TABLE(kXenosCvars, readback_resolve, readback_memexport, query_occlusion_fake_sample_count,
              allow_invalid_fetch_constants, dump_shaders, swap_post_effect, store_shaders,
              mrt_edram_used_range_clamp_to_min,
              execute_unclipped_draw_vs_on_cpu_for_psi_render_backend,
              texture_cache_memory_limit_render_to_texture, texture_cache_memory_limit_soft,
              texture_cache_memory_limit_hard, texture_cache_memory_limit_soft_lifetime,
              pre_mask_resolve_l2_block, force_convert_line_loops_to_strips,
              force_convert_quad_lists_to_triangle_lists, force_convert_triangle_fans_to_lists,
              primitive_processor_cache_min_indices, half_pixel_offset,
              resolve_resolution_scale_fill_half_pixel_offset, execute_unclipped_draw_vs_on_cpu,
              execute_unclipped_draw_vs_on_cpu_with_scissor, present_from_non_ui_thread);

}  // namespace rex::gpu::cvars
