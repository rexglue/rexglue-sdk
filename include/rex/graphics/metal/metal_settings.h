/**
 ******************************************************************************
 * Settings the ported Metal backend reads.
 ******************************************************************************
 *
 * Upstream replaced the cvar system with SettingOwner/REX_SETTING, so the three values the backend used to read by name
 * live here. Same defaults, same meaning; the difference is that they are registered with the runtime's settings
 * registry like every other backend's, which means they appear in the overlay and can be set per title.
 */
#pragma once

#include <string>

#include <rex/settings_registry.h>

namespace rex::graphics::metal {

class MetalSettings : public SettingOwner {
 public:
  MetalSettings() : SettingOwner("gpu.metal") {}

  REX_SETTING(bool, discrete_vram_mirror, false,
              "Mirror guest RAM into a discrete-VRAM buffer instead of using the unified memory view");
  REX_SETTING(bool, allow_invalid_upload_range, false,
              "Allow uploads whose source range falls outside guest RAM instead of dropping them");
  REX_SETTING(u32, texture_cache_memory_limit_soft, 256,
              "Soft ceiling for Metal texture memory in MB before the cache starts retiring entries", Range{64, 8192});
  REX_SETTING(u32, texture_cache_memory_limit_hard, 512,
              "Hard ceiling for Metal texture memory in MB", Range{64, 8192});
  REX_SETTING(bool, metal_dxil_cache, true,
              "Keep a DXIL cache on disk between runs");
  REX_SETTING(u64, metal_dxil_cache_verify_every_nth, 0,
              "Verify every Nth DXIL cache hit against a fresh conversion");
  REX_SETTING(u64, metal_dxil_cache_cap_mib, 512,
              "Ceiling for the DXIL cache in MiB");
  REX_SETTING(bool, metal_pipeline_metallib_cache, true,
              "Keep compiled metallibs on disk between runs");
  REX_SETTING(bool, metal_pipeline_binary_archive, true,
              "Use Metal binary archives for compiled pipelines");
  REX_SETTING(bool, protect_on_release, false,
              "Protect released ranges so a later read faults instead of returning stale bytes");
  // The two float24 depth behaviours the ported render target cache reads.
  REX_SETTING(bool, depth_float24_convert_in_pixel_shader, false,
              "Convert float24 depth in the pixel shader instead of a resolve pass");
  REX_SETTING(bool, depth_float24_round, false,
              "Round float24 depth instead of truncating");
  // These seven were cvars in the SDK's backend, read at their call sites by the bare name. They are settings here so they
  // register with the runtime like every other backend's; the defaults are the old declarations' own.
  REX_SETTING(bool, metal_serialize_submissions, false,
              "Finish each GPU submission before starting the next (one in flight); diagnostic, costs frame rate");
  REX_SETTING(u32, metal_max_in_flight, 0,
              "Cap on command buffers in flight, 0 for the backend default");
  REX_SETTING(std::string, metal_memexport_raster_mode, std::string("auto"),
              "Which titles route memory exports from rasterisation: auto, on or off");
  REX_SETTING(bool, metal_memexport_pixel, true,
              "Route memory exports from pixel shaders through the shared-memory buffer");
  REX_SETTING(bool, metal_pass_attachments_by_use, true,
              "Split render passes by attachment use instead of one pass per swap");
  REX_SETTING(bool, metal_present_format_convert, true,
              "Convert the guest output format during the present pass");
  REX_SETTING(bool, metal_completion_wait_exports, false,
              "Wait for the GPU before signalling completion while memory exports are pending");
  REX_SETTING(bool, gpu_allow_invalid_upload_range, false,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_async_pipelines, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(std::string, metal_completion_wait, std::string("auto"),
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_discrete_vram_mirror, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_guest_gamma_ramp, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_mark_draw_rows, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_msaa_clear_fold, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_point_sprite_vs_expansion, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_pwl_gamma, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_quad_lists_as_triangles, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_raster_mark_skip, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_raster_sync_skip, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_resolve_span_coalesce, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_stencil_export, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(bool, metal_texture_integer_scale, true,
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  REX_SETTING(std::string, metal_unwritten_interpolators, std::string("auto"),
              "Ported from the SDK backend unchanged; the old declaration set this default.");
  // Per-title hints the built-in title config sets before the guest starts; they steer the matching "auto" setting above.
  REX_SETTING(bool, metal_memexport_raster, false,
              "Per-title hint for metal_memexport_raster_mode auto (Child of Eden)");
  REX_SETTING(bool, metal_unwritten_interpolators_title, false,
              "Per-title hint for metal_unwritten_interpolators auto (Resident Evil 5)");
  REX_SETTING(bool, metal_completion_wait_title, false,
              "Per-title hint set by the built-in title config before the guest starts.");
  REX_SETTING(bool, metal_eager_publish_title, false,
              "Per-title hint set by the built-in title config before the guest starts.");
  REX_SETTING(bool, metal_rect_vs_title_off, false,
              "Per-title hint set by the built-in title config before the guest starts.");
  // Which occlusion-query strategy the guest sees: fake, fast, fast-alt or strict. The fork read this as a cvar.
  REX_SETTING(std::string, occlusion_query, std::string("fast"),
              "Occlusion query mode: fake, fast, fast-alt or strict");
  // Diagnostic: force depth clamping on regardless of the guest's clip control.
  REX_SETTING(bool, force_depth_clamp, false,
              "Treat every draw as depth-clamped, ignoring the guest's clip control");
  // Whether a fetch constant naming an invalid format is tolerated instead of dropping the draw.
  REX_SETTING(bool, gpu_allow_invalid_fetch_constants, false,
              "Allow fetch constants with an invalid format instead of rejecting the draw");
  // Read by the ported shader paths.
  REX_SETTING(bool, dxbc_implicit_lod, false,
              "Honour the guest's implicit-LOD bias when translating shaders");
  REX_SETTING(bool, implicit_early_z_write_allowed, true,
              "Treat depth-only draws as early-Z eligible");
  // Pipeline cache ceilings, read by the storage layer.
  REX_SETTING(u32, metal_pipeline_cache_title_cap_mib, 64,
              "Per-title pipeline cache ceiling in MiB");
  REX_SETTING(u32, metal_pipeline_cache_global_cap_mib, 512,
              "Global pipeline cache ceiling in MiB");
  // Whether translated shaders and pipelines are written to disk for reuse across runs.
  REX_SETTING(bool, store_shaders, true,
              "Store translated shaders and pipelines on disk");
  // Read by the pipeline storage layer.
  REX_SETTING(bool, mulsc_round_toward_zero, true,
              "Round MUL/SC results toward zero rather than to nearest");
  REX_SETTING(bool, texture_point_snap, true,
              "Snap point-sampled fetches to texel centres");
  // Macs with more than one GPU: the Metal device to render with, given as part of its name; empty uses the system default.
  REX_SETTING(std::string, metal_device, std::string(),
              "Metal device name substring to render with; empty uses the system default");
};

// One process-wide instance: the shared-memory paths that read these are constructed before the driver holds a settings
// object, and the values are process-scoped anyway.
inline MetalSettings& GetMetalSettings() {
  static MetalSettings settings;
  return settings;
}

}  // namespace rex::graphics::metal