/**
 * @file        gpu/xenos/vulkan/cvar_table.h
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

REXCVAR_BOOL(xenos, vulkan_readback_resolve, false, "Read render-to-texture results on the CPU");
REXCVAR_BOOL(xenos, vulkan_readback_memexport, false,
             "Read data written by memory export in shaders on the CPU");
REXCVAR_BOOL(xenos, vulkan_async_skip_incomplete_frames, true,
             "When async shader compilation is enabled, skip presenting frames that used "
             "placeholder pipelines to avoid visible flashing");
REXCVAR_BOOL(xenos, vulkan_submit_on_primary_buffer_end, true,
             "Submit command buffer when PM4 primary buffer ends");
REXCVAR_BOOL(xenos, vulkan_dynamic_rendering, true,
             "Use VK_KHR_dynamic_rendering for Vulkan GPU emulation when supported by the device "
             "(falls back to render passes otherwise)");
REXCVAR_INT32(
    xenos, vulkan_pipeline_creation_threads, -1,
    "Number of pipeline creation threads for Vulkan async pipeline creation (-1 for auto)")
    .SetRange(-1, 32)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(xenos, vulkan_tessellation_wireframe, false, "Render tessellation as wireframe");
REXCVAR_BOOL(xenos, vulkan_force_expand_point_sprites_in_vs, false,
             "Force Vulkan point sprite expansion in the vertex shader, even when geometry shaders "
             "are available")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(xenos, vulkan_force_expand_rectangle_lists_in_vs, false,
             "Force Vulkan rectangle list expansion in the vertex shader, even when geometry "
             "shaders are available")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(xenos, vulkan_force_convert_quad_lists_to_triangle_lists, false,
             "Force Vulkan quad list conversion to triangle lists in primitive processing, even "
             "when geometry shaders are available")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_STRING(xenos, render_target_path_vulkan, "", "Vulkan render target implementation path")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(xenos, vulkan_sparse_shared_memory, true, "Use sparse shared memory on Vulkan")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(xenos, non_seamless_cube_map, false, "Use non-seamless cube map sampling");
REXCVAR_BOOL(vulkan, require_geometry_shader, !REX_PLATFORM_MAC,
             "Require geometryShader support for Vulkan GPU emulation (disable to allow fallback "
             "primitive emulation paths)")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(vulkan, require_fill_mode_non_solid, !REX_PLATFORM_MAC,
             "Require fillModeNonSolid support for Vulkan GPU emulation (disable to allow "
             "fallback to solid fill for line/point polygon modes)")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(vulkan, log_debug_messages, true, "Log Vulkan debug messages");
REXCVAR_BOOL(vulkan, render_pass_clear, true, "Clear render pass during presentation");
REXCVAR_BOOL(vulkan, allow_present_mode_immediate, true, "Allow immediate present mode (no vsync)");
REXCVAR_BOOL(vulkan, allow_present_mode_mailbox, true,
             "Allow mailbox present mode (triple buffering)");
REXCVAR_BOOL(vulkan, allow_present_mode_fifo_relaxed, true, "Allow FIFO relaxed present mode");
REXCVAR_BOOL(vulkan, validation, false, "Enable Vulkan validation layers")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(vulkan, prefer_geometry_shader, true,
             "Prefer physical devices supporting geometryShader when auto-selecting")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(vulkan, prefer_fragment_stores_and_atomics, true,
             "Prefer physical devices supporting fragmentStoresAndAtomics when auto-selecting")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(vulkan, prefer_vertex_pipeline_stores_and_atomics, true,
             "Prefer physical devices supporting vertexPipelineStoresAndAtomics when "
             "auto-selecting")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(vulkan, prefer_fill_mode_non_solid, true,
             "Prefer physical devices supporting fillModeNonSolid when auto-selecting")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_TABLE(kVulkanCvars, require_geometry_shader, require_fill_mode_non_solid,
              log_debug_messages, render_pass_clear, allow_present_mode_immediate,
              allow_present_mode_mailbox, allow_present_mode_fifo_relaxed, validation,
              prefer_geometry_shader, prefer_fragment_stores_and_atomics,
              prefer_vertex_pipeline_stores_and_atomics, prefer_fill_mode_non_solid);
REXCVAR_TABLE(kVulkanXenosCvars, vulkan_readback_resolve, vulkan_readback_memexport,
              vulkan_async_skip_incomplete_frames, vulkan_submit_on_primary_buffer_end,
              vulkan_dynamic_rendering, vulkan_pipeline_creation_threads,
              vulkan_tessellation_wireframe, vulkan_force_expand_point_sprites_in_vs,
              vulkan_force_expand_rectangle_lists_in_vs,
              vulkan_force_convert_quad_lists_to_triangle_lists, render_target_path_vulkan,
              vulkan_sparse_shared_memory, non_seamless_cube_map);

}  // namespace rex::gpu::cvars
