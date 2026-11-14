/**
 * @file        gpu/xenos/xenos_settings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace rex::gpu {

/** The gpu.xenos settings. The Xenos graphics driver derives from it. */
class XenosSettings : public SettingOwner {
 public:
  REX_SETTING(bool, present_letterbox, true, "Keep the title's aspect ratio when presenting");
  REX_SETTING(i32, present_safe_area_x, 90, "Horizontal title safe area percent", Range{50, 100});
  REX_SETTING(i32, present_safe_area_y, 90, "Vertical title safe area percent", Range{50, 100});
  REX_SETTING(bool, present_dither, false, "Dither when presenting to an 8 bit swap chain");
  REX_SETTING(bool, present_allow_overscan_cutoff, false, "Allow cutting off overscan");
  REX_SETTING(std::string, readback_resolve, "none",
              "Controls CPU readback of render to texture resolve results",
              Allowed("none", "fast", "some", "full"));
  REX_SETTING(bool, readback_memexport, true,
              "Enable CPU readback of shader memexport writes. Keeps guest memory coherent and "
              "adds GPU and CPU sync cost");
  REX_SETTING(i32, query_occlusion_fake_sample_count, 1000,
              "Fake sample count for occlusion queries", Range{1, 100000});
  REX_SETTING(bool, allow_invalid_fetch_constants, false, "Allow invalid fetch constants");
  REX_SETTING(std::string, dump_shaders, "", "Path to dump shaders to");
  REX_SETTING(std::string, swap_post_effect, "none", "Swap post effect",
              Allowed("none", "fxaa", "fxaa_extreme"), kRestart);
  REX_SETTING(bool, store_shaders, true,
              "Keep compiled shaders on disk and load them on later runs to avoid stutter");
  REX_SETTING(bool, mrt_edram_used_range_clamp_to_min, true,
              "Clamp MRT EDRAM used range to minimum");
  REX_SETTING(bool, execute_unclipped_draw_vs_on_cpu_for_psi_render_backend, true,
              "Execute the unclipped draw vertex shader on the CPU for the pixel shader interlock "
              "render backend");
  REX_SETTING(i32, texture_cache_memory_limit_render_to_texture, 24,
              "Texture cache memory limit for render to texture (MB)", Range{1, 256}, kRestart);
  REX_SETTING(i32, texture_cache_memory_limit_soft, 384, "Soft texture cache memory limit (MB)",
              Range{64, 4096}, kRestart);
  REX_SETTING(i32, texture_cache_memory_limit_hard, 768, "Hard texture cache memory limit (MB)",
              Range{128, 8192}, kRestart);
  REX_SETTING(i32, texture_cache_memory_limit_soft_lifetime, 30,
              "Soft texture cache memory limit lifetime (seconds)", Range{1, 3600});
  REX_SETTING(bool, pre_mask_resolve_l2_block, true,
              "Mask scaled resolve L2 blocks to the write range before iterating");
  REX_SETTING(bool, force_convert_line_loops_to_strips, false,
              "Force convert line loops to strips");
  REX_SETTING(bool, force_convert_quad_lists_to_triangle_lists, false,
              "Force convert quad lists to triangle lists");
  REX_SETTING(bool, force_convert_triangle_fans_to_lists, false,
              "Force convert triangle fans to lists");
  REX_SETTING(i32, primitive_processor_cache_min_indices, 0,
              "Minimum indices for primitive processor cache", Range{0, 1000000});
  REX_SETTING(bool, half_pixel_offset, true, "Enable half pixel offset");
  REX_SETTING(bool, resolve_resolution_scale_fill_half_pixel_offset, true,
              "Fill half pixel offset during resolution scale resolve");
  REX_SETTING(bool, execute_unclipped_draw_vs_on_cpu, false,
              "Execute the unclipped draw vertex shader on the CPU");
  REX_SETTING(bool, execute_unclipped_draw_vs_on_cpu_with_scissor, false,
              "Execute the unclipped draw vertex shader on the CPU with scissor");
  REX_SETTING(bool, clear_memory_page_state, true,
              "Refresh the page valid state from GPU written memory at frame end. Turning it off "
              "lowers CPU cost but may break memory coherency");
  REX_SETTING(bool, occlusion_query_enable, true, "Enable host occlusion query handling");
  REX_SETTING(bool, readback_resolve_half_pixel_offset, false,
              "When draw resolution scaling is active, sample from the center of each scaled block "
              "during resolve readback downscale");
  REX_SETTING(bool, readback_memexport_fast, true,
              "Use fast double buffered memexport readback when possible, with automatic fallback "
              "to full synchronous readback");
  REX_SETTING(
      bool, async_shader_compilation, true,
      "Compile shaders and create pipelines asynchronously in background threads. This reduces "
      "stutter but may cause brief visual artifacts while pipelines are being prepared");
  REX_SETTING(bool, native_2x_msaa, true, "Enable native 2x MSAA");
  REX_SETTING(bool, depth_float24_round, false, "Round float24 depth values");
  REX_SETTING(bool, depth_float24_convert_in_pixel_shader, false,
              "Convert float24 depth in pixel shader");
  REX_SETTING(bool, depth_transfer_not_equal_test, true,
              "Use the not equal test for depth transfer");
  REX_SETTING(bool, gamma_render_target_as_unorm16, true,
              "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)");
  REX_SETTING(bool, snorm16_render_target_full_range, true,
              "Use full range for SNORM16 render targets");
  REX_SETTING(bool, direct_host_resolve, true,
              "Resolve from host render targets to shared memory when possible");
  REX_SETTING(bool, texture_3d_to_2d, true, "Sample 3D textures as 2D textures");

 protected:
  XenosSettings() : SettingOwner("gpu.xenos") {}
};

}  // namespace rex::gpu
