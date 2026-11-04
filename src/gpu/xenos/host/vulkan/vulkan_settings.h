/**
 * @file        gpu/xenos/host/vulkan/vulkan_settings.h
 * @brief       Settings of the Vulkan driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/types.h>

namespace rex::gpu::vulkan {

/** The Vulkan graphics driver owns one. */
class VulkanSettings : public SettingOwner {
 public:
  VulkanSettings() : SettingOwner("gpu.vulkan") {}

  REX_SETTING(i32, device, -1, "Physical device index, -1 selects automatically", Range{-1, 15},
              kStartupOnly);
  REX_SETTING(bool, readback_resolve, false, "Read render to texture results on the CPU");
  REX_SETTING(bool, readback_memexport, false,
              "Read data written by memory export in shaders on the CPU");
  REX_SETTING(bool, async_skip_incomplete_frames, true,
              "When async shader compilation is enabled, skip presenting frames that used "
              "placeholder pipelines to avoid visible flashing");
  REX_SETTING(bool, submit_on_primary_buffer_end, true,
              "Submit command buffer when PM4 primary buffer ends");
  REX_SETTING(bool, dynamic_rendering, true,
              "Use VK_KHR_dynamic_rendering when the device supports it, render passes otherwise");
  REX_SETTING(i32, pipeline_creation_threads, -1,
              "Number of pipeline creation threads for async pipeline creation, -1 for auto",
              Range{-1, 32}, kRestart);
  REX_SETTING(bool, tessellation_wireframe, false, "Render tessellation as wireframe");
  REX_SETTING(bool, force_expand_point_sprites_in_vs, false,
              "Force point sprite expansion in the vertex shader, even when geometry shaders are "
              "available",
              kStartupOnly);
  REX_SETTING(bool, force_expand_rectangle_lists_in_vs, false,
              "Force rectangle list expansion in the vertex shader, even when geometry shaders "
              "are available",
              kStartupOnly);
  REX_SETTING(bool, force_convert_quad_lists_to_triangle_lists, false,
              "Force quad list conversion to triangle lists in primitive processing, even when "
              "geometry shaders are available",
              kStartupOnly);
  REX_SETTING(std::string, render_target_path, "", "Render target implementation path",
              kStartupOnly);
  REX_SETTING(bool, sparse_shared_memory, true, "Use sparse shared memory", kRestart);
  REX_SETTING(bool, cube_map_edge_filtering_off, false,
              "Sample cube maps without edge filtering across faces");
  REX_SETTING(bool, require_geometry_shader, !REX_PLATFORM_MAC,
              "Require geometryShader support, disable to allow the fallback primitive emulation "
              "paths",
              kStartupOnly);
  REX_SETTING(bool, require_fill_mode_non_solid, !REX_PLATFORM_MAC,
              "Require fillModeNonSolid support, disable to allow the fallback to solid fill for "
              "line and point polygon modes",
              kStartupOnly);
  REX_SETTING(bool, log_debug_messages, true, "Log Vulkan debug messages");
  REX_SETTING(bool, render_pass_clear, true, "Clear render pass during presentation");
  REX_SETTING(bool, allow_present_mode_immediate, true, "Allow immediate present mode (no vsync)");
  REX_SETTING(bool, allow_present_mode_mailbox, true,
              "Allow mailbox present mode (triple buffering)");
  REX_SETTING(bool, allow_present_mode_fifo_relaxed, true, "Allow FIFO relaxed present mode");
  REX_SETTING(bool, validation, false, "Enable Vulkan validation layers", kStartupOnly);
  REX_SETTING(bool, prefer_geometry_shader, true,
              "Prefer physical devices supporting geometryShader when selecting automatically",
              kStartupOnly);
  REX_SETTING(bool, prefer_fragment_stores_and_atomics, true,
              "Prefer physical devices supporting fragmentStoresAndAtomics when selecting "
              "automatically",
              kStartupOnly);
  REX_SETTING(bool, prefer_vertex_pipeline_stores_and_atomics, true,
              "Prefer physical devices supporting vertexPipelineStoresAndAtomics when selecting "
              "automatically",
              kStartupOnly);
  REX_SETTING(bool, prefer_fill_mode_non_solid, true,
              "Prefer physical devices supporting fillModeNonSolid when selecting automatically",
              kStartupOnly);
};

}  // namespace rex::gpu::vulkan
