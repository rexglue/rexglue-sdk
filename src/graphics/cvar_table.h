/**
 * @file        graphics/cvar_table.h
 * @brief       graphics module cvars
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::graphics::cvars {

REXCVAR_BOOL(gpu, vsync, true, "Enable vertical sync");
REXCVAR_BOOL(gpu, clear_memory_page_state, true,
             "Refresh page-valid state from GPU-written memory at frame end. Disable for minor CPU "
             "overhead reduction, but may break memory coherency.");
REXCVAR_BOOL(gpu, occlusion_query_enable, true, "Enable host occlusion query handling");
REXCVAR_STRING(
    gpu, readback_resolve, "none",
    "Controls CPU readback of render-to-texture resolve results.\n none: Disable readback "
    "(default)\n fast: Read previous frame (delayed, copy every frame)\n some: Read previous frame "
    "(delayed, copy on cache miss)\n full: Immediate sync readback (accurate but stalls)")
    .allowed(REXCVAR_ALLOWED("none", "fast", "some", "full"));
REXCVAR_BOOL(gpu, readback_resolve_half_pixel_offset, false,
             "When draw resolution scaling is active, sample from the center of each scaled block "
             "during resolve readback downscale");
REXCVAR_BOOL(
    gpu, readback_memexport, true,
    "Enable CPU readback of shader memexport writes for guest memory coherency (can reduce "
    "correctness issues, but may add GPU/CPU sync cost)");
REXCVAR_BOOL(gpu, readback_memexport_fast, true,
             "Use fast double-buffered memexport readback when possible, with automatic fallback "
             "to full synchronous readback");
REXCVAR_INT32(gpu, query_occlusion_fake_sample_count, 1000,
              "Fake sample count for occlusion queries")
    .range(1, 100000);
REXCVAR_BOOL(
    gpu, async_shader_compilation, true,
    "Compile shaders and create pipelines asynchronously in background threads. This reduces "
    "stutter but may cause brief visual artifacts while pipelines are being prepared.");
REXCVAR_BOOL(gpu, d3d12_bindless, true, "Use bindless resources where available")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(gpu, d3d12_readback_memexport, false,
             "Read data written by memory export in shaders on the CPU");
REXCVAR_BOOL(gpu, d3d12_readback_resolve, false, "Read render-to-texture results on the CPU");
REXCVAR_BOOL(gpu, d3d12_submit_on_primary_buffer_end, true,
             "Submit command list when PM4 primary buffer ends");
REXCVAR_BOOL(gpu, d3d12_dxbc_disasm, false, "Dump DXBC disassembly");
REXCVAR_BOOL(gpu, d3d12_dxbc_disasm_dxilconv, false, "Dump DXIL conversion disassembly");
REXCVAR_INT32(gpu, d3d12_pipeline_creation_threads, -1,
              "Number of pipeline creation threads (-1 for auto)")
    .range(-1, 32)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(gpu, d3d12_tessellation_wireframe, false, "Render tessellation as wireframe");
REXCVAR_BOOL(gpu, native_stencil_value_output_d3d12_intel, false,
             "Native stencil value output for Intel D3D12");
REXCVAR_STRING(gpu, render_target_path_d3d12, "", "D3D12 render target implementation path")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(gpu, native_stencil_value_output, true, "Enable native stencil value output");
REXCVAR_BOOL(gpu, d3d12_tiled_shared_memory, true, "Use tiled shared memory on D3D12")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(gpu, allow_invalid_fetch_constants, false, "Allow invalid fetch constants");
REXCVAR_BOOL(gpu, native_2x_msaa, true, "Enable native 2x MSAA");
REXCVAR_BOOL(gpu, depth_float24_round, false, "Round float24 depth values");
REXCVAR_BOOL(gpu, depth_float24_convert_in_pixel_shader, false,
             "Convert float24 depth in pixel shader");
REXCVAR_BOOL(gpu, depth_transfer_not_equal_test, true, "Use not-equal test for depth transfer");
REXCVAR_BOOL(gpu, gamma_render_target_as_unorm16, true,
             "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)");
REXCVAR_STRING(gpu, dump_shaders, "", "Path to dump shaders to");
REXCVAR_BOOL(
    gpu, use_fuzzy_alpha_epsilon, false,
    "Use approximate compare for alpha test values to prevent flickering on NVIDIA graphics cards");
REXCVAR_BOOL(gpu, debug_markers, false,
             "Insert debug markers into GPU command streams for tools like PIX and RenderDoc. "
             "Automatically enabled when RenderDoc is detected.");
REXCVAR_STRING(gpu, swap_post_effect, "none", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed(REXCVAR_ALLOWED("none", "fxaa", "fxaa_extreme"))
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(gpu, store_shaders, true,
             "Store shaders persistently and load them when loading games to avoid runtime spikes "
             "and freezes when playing the game not for the first time.");
REXCVAR_BOOL(gpu, mrt_edram_used_range_clamp_to_min, true, "Clamp MRT EDRAM used range to minimum");
REXCVAR_BOOL(gpu, execute_unclipped_draw_vs_on_cpu_for_psi_render_backend, true,
             "Execute unclipped draw VS on CPU for PSI render backend");
REXCVAR_BOOL(gpu, snorm16_render_target_full_range, true,
             "Use full range for SNORM16 render targets");
REXCVAR_BOOL(gpu, direct_host_resolve, true,
             "Resolve from host render targets directly to shared memory when possible");
REXCVAR_BOOL(gpu, dxbc_switch, true, "Use switch statements in DXBC");
REXCVAR_BOOL(gpu, dxbc_source_map, false, "Generate source maps for DXBC");
REXCVAR_BOOL(gpu, draw_resolution_scaled_texture_offsets, true,
             "Scale texture offsets with draw resolution");
REXCVAR_INT32(gpu, texture_cache_memory_limit_render_to_texture, 24,
              "Texture cache memory limit for render-to-texture (MB)")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(gpu, texture_cache_memory_limit_soft, 384, "Soft texture cache memory limit (MB)")
    .range(64, 4096)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(gpu, texture_cache_memory_limit_hard, 768, "Hard texture cache memory limit (MB)")
    .range(128, 8192)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(gpu, texture_cache_memory_limit_soft_lifetime, 30,
              "Soft texture cache memory limit lifetime (seconds)")
    .range(1, 3600);
REXCVAR_BOOL(gpu, texture_3d_to_2d, true,
             "Sample problematic 3D textures through 2D-compatible wrappers");
REXCVAR_INT32(gpu, anisotropic_override, 3,
              "Forces anisotropic filtering for eligible textures.\nHigher values keep textures "
              "sharper at oblique angles, but increase texture sampling cost.\n -1 = No override\n "
              " 0 = Disable anisotropic filtering\n  1 = Force 1x anisotropic filtering\n  2 = "
              "Force 2x anisotropic filtering\n  3 = Force 4x anisotropic filtering\n  4 = Force "
              "8x anisotropic filtering\n  5 = Force 16x anisotropic filtering")
    .range(-1, 5);
REXCVAR_INT32(gpu, resolution_scale_x, 1, "Draw resolution scale X (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(gpu, resolution_scale_y, 1, "Draw resolution scale Y (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(gpu, pre_mask_resolve_l2_block, true,
             "Pre-mask scaled resolve L2 blocks to the write range before iterating");
REXCVAR_BOOL(gpu, force_convert_line_loops_to_strips, false, "Force convert line loops to strips");
REXCVAR_BOOL(gpu, force_convert_quad_lists_to_triangle_lists, false,
             "Force convert quad lists to triangle lists");
REXCVAR_BOOL(gpu, force_convert_triangle_fans_to_lists, false,
             "Force convert triangle fans to lists");
REXCVAR_INT32(gpu, primitive_processor_cache_min_indices, 0,
              "Minimum indices for primitive processor cache")
    .range(0, 1000000);
REXCVAR_BOOL(gpu, half_pixel_offset, true, "Enable half pixel offset");
REXCVAR_BOOL(gpu, resolve_resolution_scale_fill_half_pixel_offset, true,
             "Fill half pixel offset during resolution scale resolve");
REXCVAR_BOOL(gpu, execute_unclipped_draw_vs_on_cpu, false,
             "Execute unclipped draw vertex shader on CPU");
REXCVAR_BOOL(gpu, execute_unclipped_draw_vs_on_cpu_with_scissor, false,
             "Execute unclipped draw VS on CPU with scissor");
REXCVAR_BOOL(gpu, vulkan_readback_resolve, false, "Read render-to-texture results on the CPU");
REXCVAR_BOOL(gpu, vulkan_readback_memexport, false,
             "Read data written by memory export in shaders on the CPU");
REXCVAR_BOOL(gpu, vulkan_async_skip_incomplete_frames, true,
             "When async shader compilation is enabled, skip presenting frames that used "
             "placeholder pipelines to avoid visible flashing");
REXCVAR_BOOL(gpu, vulkan_submit_on_primary_buffer_end, true,
             "Submit command buffer when PM4 primary buffer ends");
REXCVAR_BOOL(gpu, vulkan_dynamic_rendering, true,
             "Use VK_KHR_dynamic_rendering for Vulkan GPU emulation when supported by the device "
             "(falls back to render passes otherwise)");
REXCVAR_INT32(
    gpu, vulkan_pipeline_creation_threads, -1,
    "Number of pipeline creation threads for Vulkan async pipeline creation (-1 for auto)")
    .range(-1, 32)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(gpu, vulkan_tessellation_wireframe, false, "Render tessellation as wireframe");
REXCVAR_BOOL(gpu, vulkan_force_expand_point_sprites_in_vs, false,
             "Force Vulkan point sprite expansion in the vertex shader, even when geometry shaders "
             "are available")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(gpu, vulkan_force_expand_rectangle_lists_in_vs, false,
             "Force Vulkan rectangle list expansion in the vertex shader, even when geometry "
             "shaders are available")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(gpu, vulkan_force_convert_quad_lists_to_triangle_lists, false,
             "Force Vulkan quad list conversion to triangle lists in primitive processing, even "
             "when geometry shaders are available")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_STRING(gpu, render_target_path_vulkan, "", "Vulkan render target implementation path")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(gpu, vulkan_sparse_shared_memory, true, "Use sparse shared memory on Vulkan")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(gpu, non_seamless_cube_map, false, "Use non-seamless cube map sampling");
REXCVAR_TABLE(kGPUCvars, vsync, clear_memory_page_state, occlusion_query_enable, readback_resolve,
              readback_resolve_half_pixel_offset, readback_memexport, readback_memexport_fast,
              query_occlusion_fake_sample_count, async_shader_compilation, d3d12_bindless,
              d3d12_readback_memexport, d3d12_readback_resolve, d3d12_submit_on_primary_buffer_end,
              d3d12_dxbc_disasm, d3d12_dxbc_disasm_dxilconv, d3d12_pipeline_creation_threads,
              d3d12_tessellation_wireframe, native_stencil_value_output_d3d12_intel,
              render_target_path_d3d12, native_stencil_value_output, d3d12_tiled_shared_memory,
              allow_invalid_fetch_constants, native_2x_msaa, depth_float24_round,
              depth_float24_convert_in_pixel_shader, depth_transfer_not_equal_test,
              gamma_render_target_as_unorm16, dump_shaders, use_fuzzy_alpha_epsilon, debug_markers,
              swap_post_effect, store_shaders, mrt_edram_used_range_clamp_to_min,
              execute_unclipped_draw_vs_on_cpu_for_psi_render_backend,
              snorm16_render_target_full_range, direct_host_resolve, dxbc_switch, dxbc_source_map,
              draw_resolution_scaled_texture_offsets, texture_cache_memory_limit_render_to_texture,
              texture_cache_memory_limit_soft, texture_cache_memory_limit_hard,
              texture_cache_memory_limit_soft_lifetime, texture_3d_to_2d, anisotropic_override,
              resolution_scale_x, resolution_scale_y, pre_mask_resolve_l2_block,
              force_convert_line_loops_to_strips, force_convert_quad_lists_to_triangle_lists,
              force_convert_triangle_fans_to_lists, primitive_processor_cache_min_indices,
              half_pixel_offset, resolve_resolution_scale_fill_half_pixel_offset,
              execute_unclipped_draw_vs_on_cpu, execute_unclipped_draw_vs_on_cpu_with_scissor,
              vulkan_readback_resolve, vulkan_readback_memexport,
              vulkan_async_skip_incomplete_frames, vulkan_submit_on_primary_buffer_end,
              vulkan_dynamic_rendering, vulkan_pipeline_creation_threads,
              vulkan_tessellation_wireframe, vulkan_force_expand_point_sprites_in_vs,
              vulkan_force_expand_rectangle_lists_in_vs,
              vulkan_force_convert_quad_lists_to_triangle_lists, render_target_path_vulkan,
              vulkan_sparse_shared_memory, non_seamless_cube_map);

}  // namespace rex::graphics::cvars
