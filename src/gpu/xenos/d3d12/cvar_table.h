/**
 * @file        gpu/xenos/d3d12/cvar_table.h
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

REXCVAR_BOOL(gpu, debug_markers, false,
             "Insert debug markers into GPU command streams for tools like PIX and RenderDoc. "
             "Automatically enabled when RenderDoc is detected.");
REXCVAR_BOOL(xenos, d3d12_bindless, true, "Use bindless resources where available")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(xenos, d3d12_readback_memexport, false,
             "Read data written by memory export in shaders on the CPU");
REXCVAR_BOOL(xenos, d3d12_readback_resolve, false, "Read render-to-texture results on the CPU");
REXCVAR_BOOL(xenos, d3d12_submit_on_primary_buffer_end, true,
             "Submit command list when PM4 primary buffer ends");
REXCVAR_BOOL(xenos, d3d12_dxbc_disasm, false, "Dump DXBC disassembly");
REXCVAR_BOOL(xenos, d3d12_dxbc_disasm_dxilconv, false, "Dump DXIL conversion disassembly");
REXCVAR_INT32(xenos, d3d12_pipeline_creation_threads, -1,
              "Number of pipeline creation threads (-1 for auto)")
    .SetRange(-1, 32)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(xenos, d3d12_tessellation_wireframe, false, "Render tessellation as wireframe");
REXCVAR_BOOL(xenos, native_stencil_value_output_d3d12_intel, false,
             "Native stencil value output for Intel D3D12");
REXCVAR_STRING(xenos, render_target_path_d3d12, "", "D3D12 render target implementation path")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(xenos, native_stencil_value_output, true, "Enable native stencil value output");
REXCVAR_BOOL(xenos, d3d12_tiled_shared_memory, true, "Use tiled shared memory on D3D12")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(
    xenos, use_fuzzy_alpha_epsilon, false,
    "Use approximate compare for alpha test values to prevent flickering on NVIDIA graphics cards");
REXCVAR_BOOL(xenos, dxbc_switch, true, "Use switch statements in DXBC");
REXCVAR_BOOL(xenos, dxbc_source_map, false, "Generate source maps for DXBC");
REXCVAR_BOOL(xenos, draw_resolution_scaled_texture_offsets, true,
             "Scale texture offsets with draw resolution");
REXCVAR_BOOL(d3d12, allow_tearing, true, "Allow variable refresh rate and tearing");
REXCVAR_BOOL(d3d12, debug, false, "Enable Direct3D 12 and DXGI debug layer")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_BOOL(d3d12, break_on_error, false, "Break on Direct3D 12 validation errors");
REXCVAR_BOOL(d3d12, break_on_warning, false, "Break on Direct3D 12 validation warnings");
REXCVAR_INT32(d3d12, queue_priority, 1,
              "Graphics command queue priority (0=normal, 1=high, 2=realtime)")
    .SetRange(0, 2);

REXCVAR_TABLE(kD3D12Cvars, allow_tearing, debug, break_on_error, break_on_warning, queue_priority);
REXCVAR_TABLE(kD3D12XenosCvars, debug_markers, d3d12_bindless, d3d12_readback_memexport,
              d3d12_readback_resolve, d3d12_submit_on_primary_buffer_end, d3d12_dxbc_disasm,
              d3d12_dxbc_disasm_dxilconv, d3d12_pipeline_creation_threads,
              d3d12_tessellation_wireframe, native_stencil_value_output_d3d12_intel,
              render_target_path_d3d12, native_stencil_value_output, d3d12_tiled_shared_memory,
              use_fuzzy_alpha_epsilon, dxbc_switch, dxbc_source_map,
              draw_resolution_scaled_texture_offsets);

}  // namespace rex::gpu::cvars
