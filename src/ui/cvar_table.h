/**
 * @file        ui/cvar_table.h
 * @brief       ui module cvars
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

#include <rex/cvar.h>

namespace rex::ui {

/** Console command, echoes the arguments to the log. */
void ConsoleEcho(std::string_view args);
/** Console command, lists cvar and command names containing a substring. */
void ConsoleFind(std::string_view args);

}  // namespace rex::ui

namespace rex::ui::cvars {

REXCVAR_BOOL(d3d12, allow_tearing, true, "Allow variable refresh rate and tearing");
REXCVAR_BOOL(d3d12, debug, false, "Enable Direct3D 12 and DXGI debug layer")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(d3d12, break_on_error, false, "Break on Direct3D 12 validation errors");
REXCVAR_BOOL(d3d12, break_on_warning, false, "Break on Direct3D 12 validation warnings");
REXCVAR_INT32(d3d12, queue_priority, 1,
              "Graphics command queue priority (0=normal, 1=high, 2=realtime)")
    .range(0, 2);
REXCVAR_TABLE(kD3D12Cvars, allow_tearing, debug, break_on_error, break_on_warning, queue_priority);

REXCVAR_COMMAND_ARGS(console, echo, ConsoleEcho, "Echo arguments to the console");
REXCVAR_COMMAND_ARGS(console, find, ConsoleFind, "List cvar/command names containing a substring");
REXCVAR_TABLE(kConsoleCvars, echo, find);

REXCVAR_BOOL(ui, present_from_non_ui_thread, true, "Allow presentation from non-UI thread");
REXCVAR_BOOL(ui, present_letterbox, true, "Enable letterboxing for non-native aspect ratios");
REXCVAR_INT32(ui, present_safe_area_x, 90, "Horizontal safe area percentage (0-100)").range(0, 100);
REXCVAR_INT32(ui, present_safe_area_y, 90, "Vertical safe area percentage (0-100)").range(0, 100);
REXCVAR_STRING(ui, present_effect, "bilinear",
               "guest output effect: bilinear, cas, fsr, fsr2, fsr3")
    .allowed(REXCVAR_ALLOWED("bilinear", "cas", "fsr", "fsr2", "fsr3"))
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_DOUBLE(ui, present_cas_additional_sharpness, 0.0, "Additional CAS sharpness in [0, 1]")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(ui, present_fsr_max_upsampling_passes, 4, "Maximum chained FSR EASU passes")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_DOUBLE(ui, present_fsr_sharpness_reduction, 0.2, "FSR RCAS sharpness reduction in stops")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_STRING(ui, present_fsr_quality_mode, "auto",
               "Temporal FSR quality mode: auto, nativeaa, quality, balanced, performance, "
               "ultra_performance")
    .allowed(REXCVAR_ALLOWED("auto", "nativeaa", "quality", "balanced", "performance",
                             "ultra_performance"))
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(ui, present_dither, false, "Enable output dithering in the final present pass")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(ui, present_allow_overscan_cutoff, false,
             "Allow overscan cutoff based on safe area settings")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(ui, window_width, 0, "Window width in logical pixels (0 = use app default)")
    .range(0, 8192);
REXCVAR_INT32(ui, window_height, 0, "Window height in logical pixels (0 = use app default)")
    .range(0, 8192);
REXCVAR_BOOL(ui, fullscreen, true, "Start the window in fullscreen mode");
REXCVAR_BOOL(ui, fullscreen_exclusive, false,
             "Switch the display mode in fullscreen instead of going borderless at the desktop "
             "mode");
REXCVAR_INT32(ui, monitor, 0,
              "Monitor index to display on (0 = default, 1 = primary, 2 = second monitor, etc.)")
    .range(0, 16);
REXCVAR_INT32(ui, video_mode_width, 1280, "title video mode width in pixels")
    .range(640, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(ui, video_mode_height, 720, "title video mode height in pixels")
    .range(480, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_STRING(ui, resolution, "",
               "Common resolution preset for both title video mode and window (for example: 720p, "
               "1080p, 1440p, 4k, 1280x720)");
REXCVAR_DOUBLE(ui, video_mode_refresh_rate, 60.0, "title video mode refresh rate in Hz")
    .range(24.0, 240.0)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_TABLE(kUICvars, present_from_non_ui_thread, present_letterbox, present_safe_area_x,
              present_safe_area_y, present_effect, present_cas_additional_sharpness,
              present_fsr_max_upsampling_passes, present_fsr_sharpness_reduction,
              present_fsr_quality_mode, present_dither, present_allow_overscan_cutoff, window_width,
              window_height, fullscreen, fullscreen_exclusive, monitor, video_mode_width,
              video_mode_height, resolution, video_mode_refresh_rate);

REXCVAR_BOOL(vulkan, require_fragment_stores_and_atomics, true,
             "Deprecated and ignored for parity; fragmentStoresAndAtomics is always required for "
             "Vulkan GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, require_vertex_pipeline_stores_and_atomics, true,
             "Deprecated and ignored for parity; vertexPipelineStoresAndAtomics is always "
             "required for Vulkan GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, require_geometry_shader, !REX_PLATFORM_MAC,
             "Require geometryShader support for Vulkan GPU emulation (disable to allow fallback "
             "primitive emulation paths)")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, require_fill_mode_non_solid, !REX_PLATFORM_MAC,
             "Require fillModeNonSolid support for Vulkan GPU emulation (disable to allow "
             "fallback to solid fill for line/point polygon modes)")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, log_debug_messages, true, "Log Vulkan debug messages");
REXCVAR_BOOL(vulkan, render_pass_clear, true, "Clear render pass during presentation");
REXCVAR_BOOL(vulkan, allow_present_mode_immediate, true, "Allow immediate present mode (no vsync)");
REXCVAR_BOOL(vulkan, allow_present_mode_mailbox, true,
             "Allow mailbox present mode (triple buffering)");
REXCVAR_BOOL(vulkan, allow_present_mode_fifo_relaxed, true, "Allow FIFO relaxed present mode");
REXCVAR_BOOL(vulkan, validation, false, "Enable Vulkan validation layers")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, prefer_geometry_shader, true,
             "Prefer physical devices supporting geometryShader when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, prefer_fragment_stores_and_atomics, true,
             "Prefer physical devices supporting fragmentStoresAndAtomics when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, prefer_vertex_pipeline_stores_and_atomics, true,
             "Prefer physical devices supporting vertexPipelineStoresAndAtomics when "
             "auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_BOOL(vulkan, prefer_fill_mode_non_solid, true,
             "Prefer physical devices supporting fillModeNonSolid when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_TABLE(kVulkanCvars, require_fragment_stores_and_atomics,
              require_vertex_pipeline_stores_and_atomics, require_geometry_shader,
              require_fill_mode_non_solid, log_debug_messages, render_pass_clear,
              allow_present_mode_immediate, allow_present_mode_mailbox,
              allow_present_mode_fifo_relaxed, validation, prefer_geometry_shader,
              prefer_fragment_stores_and_atomics, prefer_vertex_pipeline_stores_and_atomics,
              prefer_fill_mode_non_solid);

}  // namespace rex::ui::cvars
