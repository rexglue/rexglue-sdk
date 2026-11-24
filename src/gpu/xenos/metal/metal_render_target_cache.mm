/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_xenos_sample_positions.h"
#include "rex/graphics/metal/metal_render_target_cache.h"
#include "rex/graphics/metal/metal_pass_census.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <span>
#include <vector>

#include <rex/logging/log_category.h>
#include "rex/memory.h"
#include "rex/graphics/edram_layout.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_edram_msl.h"
#include "rex/graphics/render_target_transfer_class.h"
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

const uint64_t MetalRenderTargetCache::kColorFormat = MTLPixelFormatRGBA8Unorm;
// Combined depth+stencil: the guest's D24S8/D24FS8 stencil plane is real state
// (JC2 gates its half-screen passes on it); a stencil-less depth RT silently
// zeroes the stencil byte on every EDRAM round-trip.
const uint64_t MetalRenderTargetCache::kDepthFormat =
    MTLPixelFormatDepth32Float_Stencil8;

namespace {
// Metal feature gates use strict explicit-value semantics. Diagnostics default
// off; promoted pipeline features may default on while exact `NAME=0` remains
// a reliable rollback. Malformed values always fail closed.
bool IsMetalEnvironmentFlagEnabled(const char* name,
                                   bool default_enabled = false) {
  return ParseMetalEnvironmentFlag(std::getenv(name), default_enabled);
}

// Guest color resource format -> host working attachment. With the gamma gate
// off, the base class collapses gamma to plain 8888 before reaching here.
MTLPixelFormat ColorResourceFormatToMetal(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return MTLPixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return MTLPixelFormatRGBA16Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return MTLPixelFormatRGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return MTLPixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTLPixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTLPixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTLPixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTLPixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTLPixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTLPixelFormatRG32Float;
    default:
      return MTLPixelFormatRGBA8Unorm;
  }
}
}  // namespace

namespace {
// Sample-space -> pixel shifts for a key (Xenos MSAA: 2x doubles vertical
// samples, 4x doubles both).
void GetMsaaShifts(uint32_t msaa_samples, uint32_t& x_shift,
                   uint32_t& y_shift) {
  EdramMsaaShifts shifts = GetEdramMsaaShifts(msaa_samples);
  x_shift = shifts.x;
  y_shift = shifts.y;
}
// XE_METAL_RANGE_CENSUS=1: for every range operation, the rows it dispatched
// (clipped at the end tile) against the rows above its first tile row that it
// could also have skipped. Logged at shutdown.
struct RangeDispatchCensus {
  uint64_t dispatches = 0;
  uint64_t rows_full = 0;
  uint64_t rows_dispatched = 0;
  uint64_t rows_skippable = 0;
};
RangeDispatchCensus s_range_census;
const bool s_range_census_enabled =
    std::getenv("XE_METAL_RANGE_CENSUS") != nullptr;
NSUInteger GetCanonicalRangeTextureHeight(
    uint32_t base_tiles, uint32_t pitch_tiles, uint32_t msaa_samples,
    uint32_t start_tiles, uint32_t end_tiles, NSUInteger texture_height,
    uint32_t scale_y) {
  uint32_t x_shift, y_shift;
  GetMsaaShifts(msaa_samples, x_shift, y_shift);
  (void)x_shift;
  uint32_t height = ComputeCanonicalRangeTextureHeight(
      base_tiles, pitch_tiles, y_shift, end_tiles, uint32_t(texture_height),
      scale_y);
  if (s_range_census_enabled) {
    uint32_t row_start = std::min(
        height, ComputeCanonicalRangeTextureRowStart(
                    base_tiles, pitch_tiles, y_shift, start_tiles,
                    uint32_t(texture_height), scale_y));
    ++s_range_census.dispatches;
    s_range_census.rows_full += texture_height;
    s_range_census.rows_dispatched += height;
    s_range_census.rows_skippable += row_start;
  }
  return height;
}
// The first texture row a range operation can touch: every row above it lies
// in a tile row strictly above the one holding start_tiles, so the range
// shaders' in-range test rejects all of it. Range passes start their scissor
// here and range kernels their dispatch (XE_METAL_RANGE_ROW_START=0 restores
// row 0). FEAR: about 20 depth MSAA packs and 20 materializes a frame, each
// covering every row above its range (2026-10-18).
NSUInteger GetCanonicalRangeTextureRowStart(
    uint32_t base_tiles, uint32_t pitch_tiles, uint32_t msaa_samples,
    uint32_t start_tiles, NSUInteger work_height, NSUInteger texture_height,
    uint32_t scale_y) {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_RANGE_ROW_START");
    return !(v && v[0] == '0' && !v[1]);
  }();
  if (!s_enabled) {
    return 0;
  }
  uint32_t x_shift, y_shift;
  GetMsaaShifts(msaa_samples, x_shift, y_shift);
  (void)x_shift;
  uint32_t row_start = ComputeCanonicalRangeTextureRowStart(
      base_tiles, pitch_tiles, y_shift, start_tiles, uint32_t(texture_height),
      scale_y);
  return std::min<NSUInteger>(row_start, work_height);
}
// Compile options defining the scaled EDRAM tile geometry for the canonical
// EDRAM MSL (kEdramMsl's XE_EDRAM_TILE_WIDTH/HEIGHT_SAMPLES). At 1x they equal
// the source's own defaults, so the generated code is unchanged.
MTLCompileOptions* EdramMslCompileOptions(uint32_t tile_width_samples,
                                          uint32_t tile_height_samples) {
  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
  // XE_METAL_RESOLVE_AVERAGE_FLOAT=1: resolves average 8_8_8_8 samples in
  // float, as before the console's rule was measured (exact rollback).
  static const bool average_float = [] {
    const char* v = std::getenv("XE_METAL_RESOLVE_AVERAGE_FLOAT");
    return v && v[0] == '1' && !v[1];
  }();
  options.preprocessorMacros = @{
    @"XE_EDRAM_TILE_WIDTH_SAMPLES" :
        [NSString stringWithFormat:@"%uu", tile_width_samples],
    @"XE_EDRAM_TILE_HEIGHT_SAMPLES" :
        [NSString stringWithFormat:@"%uu", tile_height_samples],
    @"XE_RESOLVE_AVERAGE_FLOAT" : average_float ? @"1" : @"0",
  };
  return options;
}
bool IsCanonicalColorFormatSupported(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}
bool IsCanonicalColorRawViewFormat(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}
MTLPixelFormat CanonicalColorSyncPixelFormat(
    xenos::ColorRenderTargetFormat format, MTLPixelFormat storage_format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTLPixelFormatRG16Uint;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTLPixelFormatRGBA16Uint;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTLPixelFormatR32Uint;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTLPixelFormatRG32Uint;
    default:
      return storage_format;
  }
}
id<MTLTexture> GetCanonicalColorSyncTexture(
    id<MTLTexture> storage_texture, xenos::ColorRenderTargetFormat format) {
  if (!storage_texture || !IsCanonicalColorRawViewFormat(format)) {
    return storage_texture;
  }
  return [storage_texture
      newTextureViewWithPixelFormat:CanonicalColorSyncPixelFormat(
                                        format, storage_texture.pixelFormat)];
}
}  // namespace

MetalRenderTargetCache::MetalRenderTarget::~MetalRenderTarget() {
  if (texture_) {
    // Defer, don't free: in-flight encoders may still reference this texture
    // (see the class comment). The cache drains after the frame's GPU work
    // completes.
    cache_.deferred_texture_releases_.emplace_back(
        MetalCommandProcessor::S7TagsOff()
            ? 0
            : cache_.command_processor_.CurrentEncodingSubmission(),
        texture_);
    texture_ = nullptr;
  }
  if (msaa_texture_) {
    // A later texture may reuse the pointer: never let it hit this cache.
    cache_.InvalidateMsaaStencilCache(msaa_texture_, kMsaaStencilNew);
    cache_.deferred_texture_releases_.emplace_back(
        MetalCommandProcessor::S7TagsOff()
            ? 0
            : cache_.command_processor_.CurrentEncodingSubmission(),
        msaa_texture_);
    msaa_texture_ = nullptr;
  }
}

MetalRenderTargetCache::MetalRenderTargetCache(
    const RegisterFile& register_file, ::rex::IPhysicalMemory& memory,
    const ::rex::gpu::XenosSettings& settings, MetalCommandProcessor& command_processor,
    void* device, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y)
    : ::rex::gpu::RenderTargetCache(register_file, settings, memory,
                        draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      device_(device) {}

MetalRenderTargetCache::~MetalRenderTargetCache() { Shutdown(); }

bool MetalRenderTargetCache::CanonicalRasterConfigured() {
  return IsMetalEnvironmentFlagEnabled("XE_METAL_SAMPLE_EDRAM", true) &&
         IsMetalEnvironmentFlagEnabled("XE_METAL_SAMPLE_EDRAM_RASTER", true);
}

bool MetalRenderTargetCache::Initialize() {
  InitializeCommon();
  // Canonical per-sample EDRAM and real-MSAA raster views are the promoted
  // default. Exact `XE_METAL_SAMPLE_EDRAM=0` retains the collapsed rollback
  // through the following milestone; the raster flag can also be disabled
  // independently for sample-storage diagnostics.
  sample_edram_enabled_ =
      IsMetalEnvironmentFlagEnabled("XE_METAL_SAMPLE_EDRAM", true);
  const bool sample_edram_raster_configured =
      IsMetalEnvironmentFlagEnabled("XE_METAL_SAMPLE_EDRAM_RASTER", true);
  sample_edram_raster_enabled_ =
      sample_edram_enabled_ && sample_edram_raster_configured;
  // Only the canonical path is range-bounded everywhere a render target is
  // read or written; the collapsed rollback keeps full-height targets.
  // A window offset in the EDRAM bases (an experiment, off) starts ranges past
  // the base and adds next-period keys; not sized here.
  render_target_growth_enabled_ =
      sample_edram_enabled_ && !IsWindowOffsetInEdramSupported() &&
      IsMetalEnvironmentFlagEnabled("XE_METAL_RT_GROW", true);
  const bool sample_edram_raster_explicitly_requested =
      std::getenv("XE_METAL_SAMPLE_EDRAM_RASTER") &&
      IsMetalEnvironmentFlagEnabled("XE_METAL_SAMPLE_EDRAM_RASTER");
  if (sample_edram_raster_explicitly_requested && !sample_edram_enabled_) {
    REXLOG_WARN(
        "MetalRenderTargetCache: ignoring XE_METAL_SAMPLE_EDRAM_RASTER=1 "
        "because canonical EDRAM is disabled");
  } else if (!sample_edram_enabled_) {
    REXLOG_WARN(
        "MetalRenderTargetCache: canonical EDRAM disabled; using collapsed "
        "rollback path");
  } else if (!sample_edram_raster_enabled_) {
    REXLOG_WARN(
        "MetalRenderTargetCache: real-MSAA canonical raster disabled; using "
        "collapsed raster rollback path");
  }
  const bool canonical_restore_self_check_requested =
      IsMetalEnvironmentFlagEnabled("XE_METAL_SAMPLE_EDRAM_SELF_CHECK");
  const bool canonical_full_sync_requested =
      IsMetalEnvironmentFlagEnabled("XE_METAL_SAMPLE_EDRAM_FULL_SYNC");
  // Stage B (implement-metal-depth-semantics): host-depth ownership.
  // PROMOTED DEFAULT-ON 2026-08-01 (docs/dev/metal_depth_semantics_probe.md:
  // corpus + determinism + live gates green; matched-content perf gate shows
  // no regression with two valid samples per mode). Exact rollback:
  // XE_METAL_HOST_DEPTH=0. The predicate additionally fails closed while the
  // Stage A float24 PS conversion is engaged (both-gates arms self-disable).
  host_depth_gate_enabled_ =
      IsMetalEnvironmentFlagEnabled("XE_METAL_HOST_DEPTH", true);
  f24_ps_engaged_ = MetalF24PsEngaged(EffectiveMetalF24PsMode(
      std::getenv("XE_METAL_F24_PS"),
      ::rex::graphics::metal::GetMetalSettings().depth_float24_convert_in_pixel_shader.Get()));
  if (host_depth_gate_enabled_ && f24_ps_engaged_) {
    REXLOG_WARN(
        "MetalRenderTargetCache: host-depth ownership self-disables while "
        "float24 pixel-shader conversion is engaged (PS-converted depth "
        "carries no extra host precision); set "
        "depth_float24_convert_in_pixel_shader=false or use "
        "XE_METAL_F24_PS=0 to run Stage B arms");
  } else if (host_depth_gate_enabled_) {
    REXLOG_INFO("MetalRenderTargetCache: host-depth ownership ENGAGED (stage B)");
  }
  // Depth-to-depth host precision (RDR speckle, 2026-09-17): a float24 depth
  // target re-bound at another MSAA count (or another key of the same
  // encoding) is planned as a depth->depth transfer WITHOUT a separate
  // host-depth source (the planner drops it when it equals the transfer
  // source). The reference performs that transfer as a direct host-float
  // copy; our canonical round trip packs the source to 20e4 words and
  // materializes the destination from them, so every EQUAL test of the
  // following pass fails against the quantized value. Treat the transfer
  // source as the host-precision source: same in-plan lane refresh and the
  // same pointwise pack(host)==guest predicate. Exact rollback:
  // XE_METAL_HOST_DEPTH_D2D=0.
  host_depth_d2d_enabled_ =
      IsMetalEnvironmentFlagEnabled("XE_METAL_HOST_DEPTH_D2D", true);
  const bool canonical_depth_requested =
      IsMetalEnvironmentFlagEnabled("XE_METAL_CANONICAL_DEPTH");
  canonical_depth_enabled_ = sample_edram_enabled_ && canonical_depth_requested;
  if (canonical_depth_requested && !sample_edram_enabled_) {
    REXLOG_WARN(
        "MetalRenderTargetCache: ignoring XE_METAL_CANONICAL_DEPTH=1 because "
        "XE_METAL_SAMPLE_EDRAM=1 is not active");
  }
  if (sample_edram_enabled_ || canonical_depth_enabled_) {
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    id<MTLBuffer> buffer =
        [device newBufferWithLength:canonical_edram_size()
                            options:MTLResourceStorageModePrivate];
    if (!buffer) {
      REXLOG_ERROR(
          "MetalRenderTargetCache: canonical EDRAM allocation failed ({} MB); "
          "falling back to the collapsed path",
          canonical_edram_size() / (1024 * 1024));
      sample_edram_enabled_ = false;
      sample_edram_raster_enabled_ = false;
      render_target_growth_enabled_ = false;
      canonical_depth_enabled_ = false;
    } else {
      buffer.label = @"xe_canonical_edram";
      canonical_edram_ = (__bridge_retained void*)buffer;
      REXLOG_INFO(
          "MetalRenderTargetCache: canonical per-sample EDRAM enabled "
          "({} MB private buffer){}",
          canonical_edram_size() / (1024 * 1024),
          canonical_depth_enabled_ ? " [depth authority]" : "");
      if (sample_edram_raster_enabled_) {
        REXLOG_INFO(
            "MetalRenderTargetCache: canonical real-MSAA raster authority "
            "enabled (default pipeline; XE_METAL_SAMPLE_EDRAM=0 rolls back)");
      }
    }
  }
  if (sample_edram_enabled_) {
    // Build both real-MSAA depth/stencil pipeline sets while the gate is
    // initialized. This makes a gated corpus run validate the exact live PSOs
    // task 3.5 will schedule, rather than deferring the first compile until an
    // authority transition is already in flight.
    bool depth_2x_ready = EnsureShadowDepthPipelines(2u) != nullptr;
    bool depth_4x_ready = EnsureShadowDepthPipelines(4u) != nullptr;
    if (depth_2x_ready && depth_4x_ready) {
      REXLOG_INFO(
          "MetalRenderTargetCache: canonical per-sample depth/stencil "
          "pipelines ready (2x, 4x)");
    } else {
      REXLOG_ERROR(
          "MetalRenderTargetCache: canonical per-sample depth/stencil "
          "pipeline initialization failed (2x={}, 4x={})",
          depth_2x_ready, depth_4x_ready);
    }
  }
  canonical_restore_self_check_enabled_ =
      sample_edram_enabled_ && canonical_restore_self_check_requested;
  if (canonical_restore_self_check_enabled_) {
    REXLOG_WARN(
        "MetalRenderTargetCache: canonical restore SELF-CHECK diagnostic "
        "enabled (full 10 MiB dump/materialize/pack validation)");
  } else if (canonical_restore_self_check_requested) {
    REXLOG_WARN(
        "MetalRenderTargetCache: ignoring "
        "XE_METAL_SAMPLE_EDRAM_SELF_CHECK=1 because canonical EDRAM is not "
        "active");
  }
  canonical_full_sync_enabled_ =
      sample_edram_enabled_ && canonical_full_sync_requested;
  if (canonical_full_sync_enabled_) {
    REXLOG_WARN(
        "MetalRenderTargetCache: canonical FULL SYNC diagnostic enabled "
        "(commit+wait after every ownership-transfer batch)");
  } else if (canonical_full_sync_requested) {
    REXLOG_WARN(
        "MetalRenderTargetCache: ignoring XE_METAL_SAMPLE_EDRAM_FULL_SYNC=1 "
        "because XE_METAL_SAMPLE_EDRAM=1 is not active");
  }
  return true;
}

void MetalRenderTargetCache::Shutdown() {
  if (s_range_census_enabled && s_range_census.dispatches) {
    REXLOG_INFO(
        "MetalRangeCensus final: dispatches {} full_rows {} dispatched_rows {} "
        "({:.1f}% of full) skippable_above_start {} ({:.1f}% of dispatched)",
        s_range_census.dispatches, s_range_census.rows_full,
        s_range_census.rows_dispatched,
        100.0 * double(s_range_census.rows_dispatched) /
            double(s_range_census.rows_full),
        s_range_census.rows_skippable,
        100.0 * double(s_range_census.rows_skippable) /
            double(std::max<uint64_t>(s_range_census.rows_dispatched, 1)));
    s_range_census = RangeDispatchCensus();
  }
  for (auto& pair : resolve_snapshots_) {
    if (pair.second.texture) {
      id<MTLTexture> released =
          (__bridge_transfer id<MTLTexture>)pair.second.texture;
      (void)released;
    }
  }
  resolve_snapshots_.clear();
  auto release_render_pipeline = [](void*& opaque) {
    if (opaque) {
      id<MTLRenderPipelineState> released =
          (__bridge_transfer id<MTLRenderPipelineState>)opaque;
      (void)released;
      opaque = nullptr;
    }
  };
  auto release_compute_pipeline = [](void*& opaque) {
    if (opaque) {
      id<MTLComputePipelineState> released =
          (__bridge_transfer id<MTLComputePipelineState>)opaque;
      (void)released;
      opaque = nullptr;
    }
  };
  auto release_depth_state = [](void*& opaque) {
    if (opaque) {
      id<MTLDepthStencilState> released =
          (__bridge_transfer id<MTLDepthStencilState>)opaque;
      (void)released;
      opaque = nullptr;
    }
  };
  for (auto& pair : shadow_pipelines_) {
    ShadowDepthPipelines& pipelines = pair.second;
    release_render_pipeline(pipelines.materialize_depth);
    release_render_pipeline(pipelines.materialize_sten);
    release_compute_pipeline(pipelines.pack);
    release_compute_pipeline(pipelines.pack_cov);
    release_compute_pipeline(pipelines.pack_range);
    release_compute_pipeline(pipelines.pack_ms_range_px);
    release_render_pipeline(pipelines.mat_depth_range);
    release_render_pipeline(pipelines.mat_sten_range);
    release_compute_pipeline(pipelines.pack_ms_range);
    release_compute_pipeline(pipelines.lane_store);
    release_render_pipeline(pipelines.mat_depth_ms_range);
    release_render_pipeline(pipelines.mat_sten_ms_range);
    release_render_pipeline(pipelines.mat_depth_sten_range);
    release_depth_state(pipelines.depth_state);
    for (void*& bit_state : pipelines.bit_states) {
      release_depth_state(bit_state);
    }
    release_render_pipeline(pipelines.extract_sten);
    release_compute_pipeline(pipelines.pack_ms_range_cached);
    release_compute_pipeline(pipelines.pack_ms_range_px_cached);
    for (void*& extract_state : pipelines.extract_states) {
      release_depth_state(extract_state);
    }
  }
  shadow_pipelines_.clear();
  for (auto& pair : msaa_stencil_caches_) {
    if (pair.second.buffer) {
      id<MTLBuffer> released =
          (__bridge_transfer id<MTLBuffer>)pair.second.buffer;
      (void)released;
    }
    if (pair.second.fence) {
      id<MTLFence> released = (__bridge_transfer id<MTLFence>)pair.second.fence;
      (void)released;
    }
  }
  msaa_stencil_caches_.clear();
  if (msaa_stencil_verify_buffer_) {
    id<MTLBuffer> released =
        (__bridge_transfer id<MTLBuffer>)msaa_stencil_verify_buffer_;
    (void)released;
    msaa_stencil_verify_buffer_ = nullptr;
  }
  if (shadow_depth_library_) {
    id<MTLLibrary> released =
        (__bridge_transfer id<MTLLibrary>)shadow_depth_library_;
    (void)released;
    shadow_depth_library_ = nullptr;
  }
  if (canonical_edram_) {
    id<MTLBuffer> released = (__bridge_transfer id<MTLBuffer>)canonical_edram_;
    (void)released;
    canonical_edram_ = nullptr;
  }
  if (canonical_materialize_pipeline_) {
    id<MTLComputePipelineState> released =
        (__bridge_transfer id<MTLComputePipelineState>)canonical_materialize_pipeline_;
    (void)released;
    canonical_materialize_pipeline_ = nullptr;
  }
  if (canonical_pack_pipeline_) {
    id<MTLComputePipelineState> released =
        (__bridge_transfer id<MTLComputePipelineState>)canonical_pack_pipeline_;
    (void)released;
    canonical_pack_pipeline_ = nullptr;
  }
  release_compute_pipeline(canonical_materialize64_pipeline_);
  release_compute_pipeline(canonical_pack64_pipeline_);
  release_compute_pipeline(canonical_materialize_raw_pipeline_);
  release_compute_pipeline(canonical_pack_raw_pipeline_);
  release_compute_pipeline(canonical_materialize64_raw_pipeline_);
  release_compute_pipeline(canonical_pack64_raw_pipeline_);
  for (auto& pair : canonical_color_msaa_materialize_pipelines_) {
    id<MTLRenderPipelineState> released =
        (__bridge_transfer id<MTLRenderPipelineState>)pair.second;
    (void)released;
  }
  canonical_color_msaa_materialize_pipelines_.clear();
  for (auto& pair : canonical_resolve_pipelines_) {
    id<MTLRenderPipelineState> released =
        (__bridge_transfer id<MTLRenderPipelineState>)pair.second;
    (void)released;
  }
  canonical_resolve_pipelines_.clear();
  for (auto& pair : attachment_clear_pipelines_) {
    id<MTLRenderPipelineState> released =
        (__bridge_transfer id<MTLRenderPipelineState>)pair.second;
    (void)released;
  }
  attachment_clear_pipelines_.clear();
  release_compute_pipeline(attachment_clear_cs_pipeline_);
  release_compute_pipeline(canonical_depth_raw_resolve_pipeline_);
  release_compute_pipeline(canonical_color_packed_resolve_pipeline_);
  release_compute_pipeline(canonical_depth_raw_fwd_pipeline_);
  release_compute_pipeline(canonical_color_packed_fwd_pipeline_);
  release_compute_pipeline(direct_color_packed_fwd_pipelines_[0]);
  release_compute_pipeline(direct_color_packed_fwd_pipelines_[1]);
  release_compute_pipeline(canonical_resolve_clear_pipeline_);
  if (canonical_color_msaa_pack_pipeline_) {
    id<MTLComputePipelineState> released =
        (__bridge_transfer id<MTLComputePipelineState>)
            canonical_color_msaa_pack_pipeline_;
    (void)released;
    canonical_color_msaa_pack_pipeline_ = nullptr;
  }
  release_compute_pipeline(canonical_color64_msaa_pack_pipeline_);
  release_compute_pipeline(canonical_color_raw_msaa_pack_pipeline_);
  release_compute_pipeline(canonical_color64_raw_msaa_pack_pipeline_);
  release_render_pipeline(clear_color_pipeline_);
  release_render_pipeline(clear_gamma_color_pipeline_);
  release_render_pipeline(clear_depth_pipeline_);
  release_depth_state(clear_depth_state_);
  if (canonical_materialize_library_) {
    id<MTLLibrary> released =
        (__bridge_transfer id<MTLLibrary>)canonical_materialize_library_;
    (void)released;
    canonical_materialize_library_ = nullptr;
  }
  presentable_color_target_ = nullptr;
  presentable_color_rt_ = nullptr;
  // Destroys all MetalRenderTargets (retiring their textures to the deferred
  // list), then the common ownership state; drain LAST so those retirements
  // are actually freed.
  DestroyAllRenderTargets();
  ShutdownCommon();
  DrainDeferredReleases(UINT64_MAX);
}

void MetalRenderTargetCache::ClearTextureAtCreation(void* texture,
                                                    bool is_depth) {
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)
      command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kClearAtCreation));
  if (!cb || !texture) {
    return;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  if (is_depth) {
    pass.depthAttachment.texture = (__bridge id<MTLTexture>)texture;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0.0;  // Reverse-z far (JC2 convention).
    pass.stencilAttachment.texture = (__bridge id<MTLTexture>)texture;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;
  } else {
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)texture;
    pass.colorAttachments[0].slice = 0;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  }
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  enc.label = @"rtcache:clear-at-creation";
  [enc endEncoding];
}

std::unique_ptr<::rex::gpu::RenderTargetCache::RenderTarget>
MetalRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  ++render_target_epoch_;
  MTLPixelFormat pixel_format =
      key.is_depth ? (MTLPixelFormat)kDepthFormat
                   : ColorResourceFormatToMetal(key.GetColorFormat());
  if (sample_edram_enabled_ && !key.is_depth &&
      key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_16_16) {
    // RG16Snorm collapses the guest's fixed -32..32 domain to -1..1 and cannot
    // distinguish every raw signed-16 code. RG32Float holds the decoded values
    // losslessly for the canonical path; the generic pack kernel reconstructs
    // both original signed words exactly.
    pixel_format = MTLPixelFormatRG32Float;
  }
  if (sample_edram_enabled_ && !key.is_depth &&
      key.GetColorFormat() ==
          xenos::ColorRenderTargetFormat::k_16_16_16_16) {
    // The four-component fixed format has the same -32..32 guest domain. A
    // normalized 16-bit host target would clamp it to -1..1 before canonical
    // packing, so retain the decoded values in a float32 working view.
    pixel_format = MTLPixelFormatRGBA32Float;
  }
  // Mainline sizing: pitch_tiles_at_32bpp is SAMPLE-space (a 4x key's pitch is
  // doubled), GetWidth()/GetRenderTargetHeight() convert to msaa-aware PIXEL
  // dimensions. Textures are still single-sample (no host MSAA) — MSAA'd guest
  // surfaces just render aliased at their pixel size; all tile math converts
  // sample coordinates to pixels with the key's shifts. Host textures are the
  // guest size times the draw resolution scale.
  uint32_t width = key.GetWidth() * GetDrawResolutionScaleX();
  uint32_t height = GetFullRenderTargetTextureHeight(key);
  if (!width || !height) {
    return nullptr;
  }
  if (render_target_growth_enabled_) {
    // One tile row, or what the key had grown to; the first Update binding
    // the target sizes it to the rows in use.
    uint32_t initial_height =
        (::rex::xenos::kEDRAMTileHeightSamples >>
         uint32_t(key.msaa_samples >= xenos::MSAASamples::k2X)) *
        GetDrawResolutionScaleY();
    auto grown_it = render_target_grown_heights_.find(key.key);
    if (grown_it != render_target_grown_heights_.end()) {
      initial_height = std::max(initial_height, grown_it->second);
    }
    height = std::min(height, initial_height);
  }
  id<MTLTexture> texture =
      (__bridge_transfer id<MTLTexture>)NewRenderTargetTexture(
          key, uint64_t(pixel_format), height);
  if (!texture) {
    REXLOG_ERROR("MetalRenderTargetCache: failed to create RT {}",
           key.GetDebugName());
    return nullptr;
  }
  REXLOG_INFO("MetalRenderTargetCache: new RT {} ({}x{} mtl={})", key.GetDebugName(),
         width, height, uint32_t(pixel_format));
  ClearTextureAtCreation((__bridge void*)texture, key.is_depth != 0);
  std::unique_ptr<RenderTarget> rt(
      new MetalRenderTarget(key, *this, (__bridge_retained void*)texture,
                            uint64_t(pixel_format), height));
  return rt;
}

uint32_t MetalRenderTargetCache::GetFullRenderTargetTextureHeight(
    RenderTargetKey key) const {
  return GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) *
         GetDrawResolutionScaleY();
}

void* MetalRenderTargetCache::NewRenderTargetTexture(RenderTargetKey key,
                                                     uint64_t pixel_format,
                                                     uint32_t height) {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pixel_format
                                   width:key.GetWidth() *
                                         GetDrawResolutionScaleX()
                                  height:height
                               mipmapped:NO];
  // Sampled by the depth-convert / snapshot-blit paths and by transfers.
  // Canonical color materialization also writes single-sample color targets
  // from compute, so their declared usage must include ShaderWrite.
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  if (!key.is_depth) {
    desc.usage |= MTLTextureUsageShaderWrite;
  }
  if (key.is_depth ||
      (sample_edram_enabled_ &&
       IsCanonicalColorRawViewFormat(key.GetColorFormat()))) {
    // Canonical and legacy depth/stencil transfer paths create an
    // X32_Stencil8 view even under the exact collapsed rollback. Raw color
    // pack paths reinterpret through their canonical uint view.
    desc.usage |= MTLTextureUsagePixelFormatView;
  }
  desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  return (__bridge_retained void*)texture;
}

void* MetalRenderTargetCache::NewMsaaCompanionTexture(RenderTargetKey key,
                                                      uint64_t pixel_format,
                                                      uint32_t sample_count,
                                                      uint32_t height) {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
  descriptor.textureType = MTLTextureType2DMultisample;
  descriptor.pixelFormat = (MTLPixelFormat)pixel_format;
  descriptor.width = key.GetWidth() * GetDrawResolutionScaleX();
  descriptor.height = height;
  descriptor.sampleCount = sample_count;
  descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  if (key.is_depth || IsCanonicalColorRawViewFormat(key.GetColorFormat())) {
    // Depth MSAA pack reads stencil through an X32_Stencil8 view; raw color
    // MSAA pack uses the corresponding canonical uint view.
    descriptor.usage |= MTLTextureUsagePixelFormatView;
  }
  descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (texture) {
    texture.label = [NSString
        stringWithFormat:@"xe_rt_msaa_%ux_%u", sample_count, height];
  }
  return (__bridge_retained void*)texture;
}

bool MetalRenderTargetCache::GrowRenderTarget(MetalRenderTarget* target,
                                              uint32_t height) {
  const RenderTargetKey key = target->GetKey();
  const uint32_t old_height = target->texture_height();
  if (height <= old_height) {
    return true;
  }
  id<MTLTexture> old_texture = (__bridge id<MTLTexture>)target->texture();
  id<MTLTexture> old_msaa_texture =
      (__bridge id<MTLTexture>)target->msaa_texture();
  id<MTLTexture> texture =
      (__bridge_transfer id<MTLTexture>)NewRenderTargetTexture(
          key, target->pixel_format(), height);
  id<MTLTexture> msaa_texture = nil;
  if (texture && old_msaa_texture) {
    msaa_texture = (__bridge_transfer id<MTLTexture>)NewMsaaCompanionTexture(
        key, target->pixel_format(), target->sample_count(), height);
  }
  if (!texture || (old_msaa_texture && !msaa_texture)) {
    REXLOG_ERROR("MetalRenderTargetCache: failed to grow RT {} to {} rows",
           key.GetDebugName(), height);
    return false;
  }
  // Ends a render encoder the old textures may be attached to; the old rows
  // are then final in command buffer order.
  ClearTextureAtCreation((__bridge void*)texture, key.is_depth != 0);
  if (msaa_texture) {
    ClearTextureAtCreation((__bridge void*)msaa_texture, key.is_depth != 0);
  }
  id<MTLCommandBuffer> command_buffer = (__bridge id<MTLCommandBuffer>)
      command_processor_.PrepareTransferCommandBuffer(
          uint32_t(MetalTransferSite::kRenderTargetGrowth));
  if (!command_buffer) {
    REXLOG_ERROR("MetalRenderTargetCache: no command buffer to grow RT {}",
           key.GetDebugName());
    return false;
  }
  // Raw copies: every row the old textures hold, bit for bit (depth and
  // stencil planes, each sample of a multisample texture).
  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  blit.label = @"rtcache:grow";
  MTLSize copy_size = MTLSizeMake(old_texture.width, old_height, 1);
  [blit copyFromTexture:old_texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:copy_size
              toTexture:texture
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
  if (msaa_texture) {
    [blit copyFromTexture:old_msaa_texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:copy_size
                toTexture:msaa_texture
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
  }
  [blit endEncoding];

  uint64_t submission = MetalCommandProcessor::S7TagsOff()
                            ? 0
                            : command_processor_.CurrentEncodingSubmission();
  void* retired = target->ReplaceTexture((__bridge_retained void*)texture,
                                         height);
  if (presentable_color_target_ == retired) {
    presentable_color_target_ = target->texture();
  }
  deferred_texture_releases_.emplace_back(submission, retired);
  if (msaa_texture) {
    void* retired_msaa = target->ReplaceMsaaTexture(
        (__bridge_retained void*)msaa_texture, target->sample_count(), height);
    // Extracted stencil is cached by texture pointer: the old pointer may be
    // reused and the new one may have been.
    for (void* replaced : {retired_msaa, target->msaa_texture()}) {
      auto it = msaa_stencil_caches_.find(replaced);
      if (it != msaa_stencil_caches_.end()) {
        it->second.valid = false;
        it->second.why = kMsaaStencilReplace;
      }
    }
    deferred_texture_releases_.emplace_back(submission, retired_msaa);
  }
  // Texture pointers changed under the same RenderTarget addresses.
  ++render_target_epoch_;
  ++render_target_growth_count_;
  render_target_grown_heights_[key.key] = height;
  REXLOG_INFO("MetalRenderTargetCache: RT {} height {} -> {} (full {})",
         key.GetDebugName(), old_height, height,
         GetFullRenderTargetTextureHeight(key));
  return true;
}

bool MetalRenderTargetCache::EnsureCurrentRenderTargetHeights() {
  if (!render_target_growth_enabled_) {
    return true;
  }
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  const uint32_t* lengths = GetLastUpdateUsedRenderTargetLengthsTiles();
  const uint32_t* starts = GetLastUpdateUsedRenderTargetStartsTiles();
  // Rows of the ranges this draw uses. Every range operation on a target
  // (materialize, pack, transfer, the draw itself) stays inside a range the
  // target was bound with, so these are the rows that can ever hold content.
  uint32_t required_height = 0;
  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    RenderTarget* target = used[slot];
    if (!target || !lengths[slot]) {
      continue;
    }
    const RenderTargetKey key = target->GetKey();
    required_height = std::max(
        required_height,
        ComputeCanonicalRangeTextureHeight(
            key.base_tiles, key.GetPitchTiles(),
            uint32_t(key.msaa_samples >= xenos::MSAASamples::k2X),
            key.base_tiles + starts[slot] + lengths[slot],
            GetFullRenderTargetTextureHeight(key),
            GetDrawResolutionScaleY()));
  }
  // The pass's attachments, each at least that tall (the render area is the
  // smallest of them): the accumulated targets, and the used ones in case a
  // slot's binding is not accumulated.
  for (RenderTarget* const* set : {accumulated, used}) {
    for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
      auto* target = static_cast<MetalRenderTarget*>(set[slot]);
      if (!target || target->texture_height() >= required_height) {
        continue;
      }
      const RenderTargetKey key = target->GetKey();
      const uint32_t tile_row_height =
          (::rex::xenos::kEDRAMTileHeightSamples >>
           uint32_t(key.msaa_samples >= xenos::MSAASamples::k2X)) *
          GetDrawResolutionScaleY();
      uint32_t height = required_height;
      if (target->texture_height() > tile_row_height) {
        // A target already sized by use is outgrown: leave a quarter of
        // headroom so a range creeping down a row of tiles at a time does not
        // copy the target for each.
        height += height / 4;
      }
      height =
          (height + tile_row_height - 1) / tile_row_height * tile_row_height;
      height = std::min(height, GetFullRenderTargetTextureHeight(key));
      if (!GrowRenderTarget(target, height)) {
        return false;
      }
    }
  }
  return true;
}

bool MetalRenderTargetCache::EnsureCurrentMsaaCompanions() {
  if (!sample_edram_raster_enabled_) {
    return true;
  }

  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  const uint32_t* lengths = GetLastUpdateUsedRenderTargetLengthsTiles();
  const uint32_t* starts = GetLastUpdateUsedRenderTargetStartsTiles();
  static const bool s_memo_enabled =
      IsMetalEnvironmentFlagEnabled("XE_METAL_MSAA_COMPANION_MEMO", true);
  constexpr size_t kSlots = xenos::kMaxColorRenderTargets + 1;
  MsaaCompanionMemo& memo = msaa_companion_memo_;
  if (s_memo_enabled && memo.valid && memo.epoch == render_target_epoch_ &&
      !std::memcmp(memo.used, used, sizeof(memo.used)) &&
      !std::memcmp(memo.accumulated, accumulated, sizeof(memo.accumulated)) &&
      !std::memcmp(memo.lengths, lengths, sizeof(memo.lengths))) {
    return true;
  }
  auto remember_satisfied = [&]() {
    memo.valid = true;
    memo.epoch = render_target_epoch_;
    std::memcpy(memo.used, used, kSlots * sizeof(RenderTarget*));
    std::memcpy(memo.accumulated, accumulated, kSlots * sizeof(RenderTarget*));
    std::memcpy(memo.lengths, lengths, kSlots * sizeof(uint32_t));
  };
  uint32_t requested_height = 0;
  uint32_t full_height = 0;
  uint32_t sample_count = 0;

  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    auto* target = static_cast<MetalRenderTarget*>(used[slot]);
    if (!target || !lengths[slot] ||
        target->GetKey().msaa_samples == xenos::MSAASamples::k1X) {
      continue;
    }
    const RenderTargetKey& key = target->GetKey();
    uint32_t target_sample_count = uint32_t(1) << uint32_t(key.msaa_samples);
    if (sample_count && sample_count != target_sample_count) {
      REXLOG_ERROR("MetalRenderTargetCache: mixed MSAA sample counts in one pass");
      return false;
    }
    sample_count = target_sample_count;
    // Host rows: the guest rows times the vertical draw resolution scale.
    full_height = GetRenderTargetHeight(key.pitch_tiles_at_32bpp,
                                        key.msaa_samples) *
                  GetDrawResolutionScaleY();
    uint32_t x_shift, y_shift;
    GetMsaaShifts(uint32_t(key.msaa_samples), x_shift, y_shift);
    (void)x_shift;
    requested_height = std::max(
        requested_height,
        ComputeCanonicalRangeTextureHeight(
            key.base_tiles, key.GetPitchTiles(), y_shift,
            key.base_tiles + starts[slot] + lengths[slot], full_height,
            GetDrawResolutionScaleY()));
  }
  if (!sample_count || !requested_height || !full_height) {
    remember_satisfied();
    return true;
  }

  // At most one target per slot (colour + depth); a fixed array keeps this
  // per-draw check off the heap.
  MetalRenderTarget* target_storage[xenos::kMaxColorRenderTargets + 1];
  uint32_t target_count = 0;
  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    auto* target = static_cast<MetalRenderTarget*>(accumulated[slot]);
    if (!target) {
      continue;
    }
    uint32_t target_sample_count =
        uint32_t(1) << uint32_t(target->GetKey().msaa_samples);
    if (target_sample_count != sample_count) {
      REXLOG_ERROR("MetalRenderTargetCache: incompatible accumulated MSAA target");
      return false;
    }
    target_storage[target_count++] = target;
  }
  const std::span<MetalRenderTarget* const> targets(target_storage,
                                                    target_count);
  // A companion resolves into its target's texture and holds the same rows
  // (all of the guest surface's, unless targets grow with use).
  bool allocation_needed = false;
  for (MetalRenderTarget* target : targets) {
    if (!target->msaa_texture() ||
        target->msaa_texture_height() < target->texture_height()) {
      allocation_needed = true;
      break;
    }
  }
  if (!allocation_needed) {
    remember_satisfied();
    return true;
  }

  // Any attachment content that will be replaced must first be made canonical.
  // PrepareTransferCommandBuffer also closes a render encoder that may still
  // reference an old view; deferred release then keeps it alive through the
  // current submission.
  if (!PrepareCanonicalEdramForReadback() ||
      !command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kMsaaCompanions))) {
    return false;
  }

  struct PendingCompanion {
    MetalRenderTarget* target;
    void* texture;
  };
  std::vector<PendingCompanion> pending;
  for (MetalRenderTarget* target : targets) {
    const uint32_t allocation_height = target->texture_height();
    if (target->msaa_texture() &&
        target->msaa_texture_height() >= allocation_height) {
      continue;
    }
    const RenderTargetKey& key = target->GetKey();
    id<MTLTexture> texture =
        (__bridge_transfer id<MTLTexture>)NewMsaaCompanionTexture(
            key, target->pixel_format(), sample_count, allocation_height);
    if (!texture) {
      REXLOG_WARN(
          "MetalRenderTargetCache: {}x MSAA companion alloc failed for {} "
          "({}x{})",
          sample_count, key.GetDebugName(),
          key.GetWidth() * GetDrawResolutionScaleX(), allocation_height);
      uint64_t submission =
          MetalCommandProcessor::S7TagsOff()
              ? 0
              : command_processor_.CurrentEncodingSubmission();
      for (const PendingCompanion& entry : pending) {
        deferred_texture_releases_.emplace_back(submission, entry.texture);
      }
      return false;
    }
    ClearTextureAtCreation((__bridge void*)texture, key.is_depth != 0);
    pending.push_back({target, (__bridge_retained void*)texture});
  }

  uint64_t submission =
      MetalCommandProcessor::S7TagsOff()
          ? 0
          : command_processor_.CurrentEncodingSubmission();
  for (const PendingCompanion& entry : pending) {
    MetalRenderTarget* target = entry.target;
    uint32_t old_height = target->msaa_texture_height();
    const uint32_t allocation_height = target->texture_height();
    void* old_texture = target->ReplaceMsaaTexture(
        entry.texture, sample_count, allocation_height);
    for (void* replaced : {old_texture, entry.texture}) {
      auto it = msaa_stencil_caches_.find(replaced);
      if (it != msaa_stencil_caches_.end()) {
        it->second.valid = false;
        it->second.why = kMsaaStencilReplace;
      }
    }
    if (old_texture) {
      deferred_texture_releases_.emplace_back(submission, old_texture);
    }
    canonical_authority_.InvalidateAttachment(
        GetCanonicalAttachmentId(target));
    REXLOG_INFO(
        "MetalRenderTargetCache: RT {} {}x MSAA companion height {} -> {} "
        "(full {})",
        target->GetKey().GetDebugName(), sample_count, old_height,
        allocation_height, GetFullRenderTargetTextureHeight(target->GetKey()));
  }
  return true;
}

bool MetalRenderTargetCache::UpdateWithWindowOffset(bool is_rasterization_done,
                                    reg::RB_DEPTHCONTROL normalized_depth_control,
                                    uint32_t normalized_color_mask,
                                    const ::rex::gpu::Shader& vertex_shader,
                                    int32_t window_offset_tiles) {
  if (!::rex::gpu::RenderTargetCache::Update(is_rasterization_done,
                                 normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }
  if (!EnsureCurrentRenderTargetHeights() || !EnsureCurrentMsaaCompanions()) {
    return false;
  }
  static const bool s_rt_census = std::getenv("XE_METAL_RT_CENSUS") != nullptr;
  if (s_rt_census) {
    NoteRenderTargetCensus();
  }
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  {
    // Diagnostic census (XE_METAL_MSAA_STENCIL_CENSUS=1): how often 2x/4x
    // depth targets are drawn with stencil on, packed and materialized.
    static const bool s_census =
        std::getenv("XE_METAL_MSAA_STENCIL_CENSUS") != nullptr;
    if (s_census) {
      ++msaa_stencil_census_[0];
      if (accumulated[0] &&
          accumulated[0]->GetKey().msaa_samples != xenos::MSAASamples::k1X) {
        ++msaa_stencil_census_[1];
        msaa_census_last_draw_[accumulated[0]->GetKey().key] =
            msaa_stencil_census_[0];
        if (normalized_depth_control.stencil_enable) {
          ++msaa_stencil_census_[2];
        }
      }
      if ((msaa_stencil_census_[0] & 0x3FFFFu) == 0) {
        REXLOG_INFO("MetalMsaaStencilCensus: updates {}, on 2x/4x depth {}, with "
               "stencil {}; 2x/4x depth packs {} (no draw to the target since "
               "a pack covering the range {}, since any pack {}), "
               "materializes {}",
               msaa_stencil_census_[0], msaa_stencil_census_[1],
               msaa_stencil_census_[2], msaa_stencil_census_[3],
               msaa_stencil_census_[5], msaa_stencil_census_[6],
               msaa_stencil_census_[4]);
      }
    }
  }
  PerformTransfers(accumulated, GetLastUpdateTransfers());
  if (!msaa_stencil_caches_.empty() && accumulated[0]) {
    // The draw may write the target's stencil (or open its pass with a
    // clear), so its cached stencil is stale -- after the transfers, which
    // may have packed this target and rebuilt the cache. (Invalidating only
    // after stencil-writing draws saves ~nothing: Gears 3, RDR and Judgment
    // rebuild after those.)
    auto* depth = static_cast<MetalRenderTarget*>(accumulated[0]);
    InvalidateMsaaStencilCache(depth->msaa_texture(), kMsaaStencilDraw);
  }
  // Track the presentable (BGRA8) color target for the pre-resolve fallback.
  for (uint32_t i = 1; i <= xenos::kMaxColorRenderTargets; ++i) {
    auto* rt = static_cast<MetalRenderTarget*>(accumulated[i]);
    if (rt && rt->pixel_format() == kColorFormat) {
      presentable_color_target_ = rt->texture();
      presentable_color_rt_ = rt;
      break;
    }
  }
  return true;
}

void MetalRenderTargetCache::NoteRenderTargetCensus() {
  uint64_t now_ms = uint64_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  const uint32_t* lengths = GetLastUpdateUsedRenderTargetLengthsTiles();
  const uint32_t* starts = GetLastUpdateUsedRenderTargetStartsTiles();
  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    if (!used[slot] || !lengths[slot]) {
      continue;
    }
    RenderTargetCensusEntry& entry = rt_census_[used[slot]->GetKey().key];
    entry.last_used_ms = now_ms;
    entry.max_end_tiles =
        std::max(entry.max_end_tiles, starts[slot] + lengths[slot]);
  }
  if (!rt_census_last_report_ms_) {
    rt_census_last_report_ms_ = now_ms;
  } else if (now_ms - rt_census_last_report_ms_ >= 10000) {
    rt_census_last_report_ms_ = now_ms;
    ReportRenderTargetCensus(now_ms);
  }
}

void MetalRenderTargetCache::ReportRenderTargetCensus(uint64_t now_ms) {
  struct Row {
    RenderTargetKey key;
    uint64_t bytes;
    uint64_t used_bytes;
    uint32_t owned_tiles;
    uint32_t width, height, used_height, owned_height, samples;
    // Seconds since a draw used it; UINT32_MAX if none has (transfers only).
    uint32_t idle_s;
  };
  std::vector<Row> rows;
  // Totals in bytes: all, without EDRAM ownership, not drawn to for 10 s and
  // 60 s, never drawn to, and what the rows draws have reached would take.
  uint64_t total = 0, unowned = 0, idle_10 = 0, idle_60 = 0, never = 0,
           reached = 0;
  // Targets whose texture is shorter than the rows of a range they were used
  // with or own: none, if growth keeps its invariant.
  uint32_t short_targets = 0;
  ForEachRenderTargetWithOwnedTiles(
      [&](RenderTarget* render_target, uint32_t owned_tiles,
          uint32_t owned_end_tiles) {
        auto* rt = static_cast<MetalRenderTarget*>(render_target);
        id<MTLTexture> texture = (__bridge id<MTLTexture>)rt->texture();
        id<MTLTexture> msaa = (__bridge id<MTLTexture>)rt->msaa_texture();
        RenderTargetKey key = rt->GetKey();
        Row row = {};
        row.key = key;
        row.bytes = uint64_t(texture.allocatedSize) +
                    (msaa ? uint64_t(msaa.allocatedSize) : 0);
        row.owned_tiles = owned_tiles;
        row.width = uint32_t(texture.width);
        row.height = uint32_t(texture.height);
        row.samples = msaa ? rt->sample_count() : 1;
        row.idle_s = UINT32_MAX;
        const uint32_t full_height = GetFullRenderTargetTextureHeight(key);
        const uint32_t y_shift =
            uint32_t(key.msaa_samples >= xenos::MSAASamples::k2X);
        row.owned_height = ComputeCanonicalRangeTextureHeight(
            key.base_tiles, key.GetPitchTiles(), y_shift,
            key.base_tiles + owned_end_tiles, full_height,
            GetDrawResolutionScaleY());
        auto found = rt_census_.find(key.key);
        if (found != rt_census_.end()) {
          row.idle_s = uint32_t((now_ms - found->second.last_used_ms) / 1000);
          row.used_height = ComputeCanonicalRangeTextureHeight(
              key.base_tiles, key.GetPitchTiles(), y_shift,
              key.base_tiles + found->second.max_end_tiles, full_height,
              GetDrawResolutionScaleY());
        }
        if (std::max(row.used_height, row.owned_height) > row.height) {
          ++short_targets;
        }
        row.used_bytes = row.height ? row.bytes *
                                          std::min(row.used_height, row.height) /
                                          row.height
                                    : 0;
        total += row.bytes;
        reached += row.used_bytes;
        if (!owned_tiles) {
          unowned += row.bytes;
        }
        if (row.idle_s == UINT32_MAX) {
          never += row.bytes;
        } else {
          if (row.idle_s >= 10) {
            idle_10 += row.bytes;
          }
          if (row.idle_s >= 60) {
            idle_60 += row.bytes;
          }
        }
        rows.push_back(row);
      });
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
  constexpr double kMb = 1.0 / (1024.0 * 1024.0);
  REXLOG_INFO("MetalRtCensus {} targets {:.1f} MB | owning no EDRAM {:.1f} | not "
         "drawn to for 10 s {:.1f}, 60 s {:.1f}, ever {:.1f} | rows draws "
         "reached {:.1f} | {} shorter than their rows, {} grown",
         rows.size(), total * kMb, unowned * kMb, idle_10 * kMb, idle_60 * kMb,
         never * kMb, reached * kMb, short_targets,
         render_target_growth_count_);
  for (size_t i = 0; i < rows.size() && i < 48; ++i) {
    const Row& row = rows[i];
    REXLOG_INFO("MetalRtCensus #{} {:.1f} MB {}x{} x{} reached {} rows owns {}t "
           "to row {} idle {} | {}",
           i, row.bytes * kMb, row.width, row.height, row.samples,
           row.used_height, row.owned_tiles, row.owned_height,
           row.idle_s == UINT32_MAX ? std::string("never drawn")
                                    : fmt::format("{} s", row.idle_s),
           row.key.key);
  }
}

bool MetalRenderTargetCache::IsWindowOffsetInEdramSupported() const {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_WINDOW_OFFSET_IN_EDRAM");
    return v && v[0] == '1' && !v[1];
  }();
  return s_enabled;
}

void* MetalRenderTargetCache::current_depth_texture() const {
  auto* rt = static_cast<MetalRenderTarget*>(
      GetLastUpdateAccumulatedRenderTargets()[0]);
  return rt ? rt->texture() : nullptr;
}

void* MetalRenderTargetCache::current_color_texture(
    uint32_t index, uint64_t* pixel_format_out) const {
  auto* rt = static_cast<MetalRenderTarget*>(
      GetLastUpdateAccumulatedRenderTargets()[1 + index]);
  if (!rt) {
    return nullptr;
  }
  if (pixel_format_out) {
    *pixel_format_out = rt->pixel_format();
  }
  return rt->texture();
}

void* MetalRenderTargetCache::current_depth_raster_texture() const {
  auto* rt = static_cast<MetalRenderTarget*>(
      GetLastUpdateAccumulatedRenderTargets()[0]);
  if (!rt) {
    return nullptr;
  }
  return current_raster_sample_count() > 1u ? rt->msaa_texture()
                                            : rt->texture();
}

void* MetalRenderTargetCache::current_color_raster_texture(
    uint32_t index, uint64_t* pixel_format_out) const {
  auto* rt = static_cast<MetalRenderTarget*>(
      GetLastUpdateAccumulatedRenderTargets()[1 + index]);
  if (!rt) {
    return nullptr;
  }
  if (pixel_format_out) {
    *pixel_format_out = rt->pixel_format();
  }
  return current_raster_sample_count() > 1u ? rt->msaa_texture()
                                            : rt->texture();
}

uint32_t MetalRenderTargetCache::current_raster_sample_count() const {
  if (!sample_edram_raster_enabled_) {
    return 1u;
  }
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  uint32_t common_sample_count = 0u;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    auto* rt = static_cast<MetalRenderTarget*>(accumulated[i]);
    if (!rt) {
      continue;
    }
    uint32_t required_sample_count =
        1u << uint32_t(rt->GetKey().msaa_samples);
    if (required_sample_count == 1u || !rt->msaa_texture() ||
        rt->sample_count() != required_sample_count) {
      return 1u;
    }
    if (common_sample_count && common_sample_count != required_sample_count) {
      return 1u;
    }
    common_sample_count = required_sample_count;
  }
  return common_sample_count ? common_sample_count : 1u;
}

MetalEdramAuthorityTracker::AttachmentId
MetalRenderTargetCache::GetCanonicalAttachmentId(
    const RenderTarget* render_target) {
  // RenderTargetKey is the stable cache identity. Zero is reserved by the
  // authority tracker for "no attachment".
  return render_target ? render_target->GetKey().key + 1u : 0u;
}

::rex::gpu::RenderTargetCache::RenderTarget*
MetalRenderTargetCache::FindCanonicalAttachment(
    MetalEdramAuthorityTracker::AttachmentId attachment) const {
  if (!attachment) {
    return nullptr;
  }
  return FindCachedRenderTarget(uint32_t(attachment - 1u));
}

bool MetalRenderTargetCache::EncodeCanonicalAuthorityOperation(
    const void* command_buffer,
    const MetalEdramAuthorityTracker::SyncOperation& operation) {
  RenderTarget* render_target = FindCanonicalAttachment(operation.attachment);
  if (!render_target) {
    return false;
  }
  bool materialize =
      operation.kind ==
      MetalEdramAuthorityTracker::SyncKind::kMaterializeFromCanonical;
  const RenderTargetKey& key = render_target->GetKey();
  // Diagnostic (XE_METAL_CANONICAL_SYNC_LOG=1): every pack into and
  // materialize out of canonical EDRAM, to find who reads tiles that a direct
  // colour resolve left stale (interleave with --replay_draw_log).
  static const bool s_sync_log =
      std::getenv("XE_METAL_CANONICAL_SYNC_LOG") != nullptr;
  if (s_sync_log) {
    REXLOG_WARN("MetalCanonicalSync {} req={} depth={} fmt={} msaa={} base={} "
           "pitch={} tiles=[{},{})",
           materialize ? "materialize" : "pack", uint32_t(sync_requester_),
           key.is_depth, key.resource_format, uint32_t(key.msaa_samples),
           key.base_tiles, key.GetPitchTiles(), operation.start_tiles,
           operation.end_tiles);
  }
  if (!materialize) {
    // A pack rewrites canonical: no longer an untouched clear.
    SetTileClear(operation.start_tiles, operation.end_tiles, false, 0, 0);
  }
  if (MetalCommandProcessor::PassCensusEnabled()) {
    const uint32_t r = uint32_t(sync_requester_);
    const uint32_t tiles = operation.end_tiles - operation.start_tiles;
    if (materialize) {
      ++pack_census_.materialize_ops[r];
      pack_census_.materialize_tiles[r] += tiles;
      for (uint32_t t = operation.start_tiles;
           t < operation.end_tiles && t < ::rex::xenos::kEDRAMTileCount; ++t) {
        pack_census_.materialize_tiles_from_clear += census_clear_tiles_[t];
      }
    } else {
      std::vector<uint8_t>& dirty = pack_dirty_tiles_[operation.attachment];
      if (dirty.size() != ::rex::xenos::kEDRAMTileCount) {
        // First sight of this attachment: everything counts as dirty.
        dirty.assign(::rex::xenos::kEDRAMTileCount, 1);
      }
      uint32_t redundant = 0;
      for (uint32_t t = operation.start_tiles; t < operation.end_tiles; ++t) {
        redundant += dirty[t] ? 0 : 1;
        dirty[t] = 0;
      }
      ++pack_census_.pack_ops[r];
      pack_census_.pack_tiles[r] += tiles;
      // Raster-pair census: packs from resolves / readbacks also decide who
      // last wrote canonical (raster-view packs are noted at sync time).
      if (sync_requester_ != CanonicalSyncRequester::kRasterViews) {
        if (census_last_packer_.size() != ::rex::xenos::kEDRAMTileCount) {
          census_last_packer_.assign(::rex::xenos::kEDRAMTileCount, 0);
        }
        for (uint32_t t = operation.start_tiles;
             t < operation.end_tiles && t < ::rex::xenos::kEDRAMTileCount; ++t) {
          census_last_packer_[t] = operation.attachment;
        }
      }
      pack_census_.pack_tiles_redundant[r] += redundant;
      if (redundant == tiles) {
        ++pack_census_.pack_ops_all_redundant[r];
      }
    }
  }
  if (key.is_depth) {
    if (key.msaa_samples == xenos::MSAASamples::k1X) {
      return materialize
                 ? EncodeCanonicalDepthMaterialize(
                       command_buffer, render_target, operation.start_tiles,
                       operation.end_tiles)
                 : EncodeCanonicalDepthPack(command_buffer, render_target,
                                            operation.start_tiles,
                                            operation.end_tiles);
    }
    return materialize
               ? EncodeCanonicalDepthMsaaMaterialize(
                     command_buffer, render_target, operation.start_tiles,
                     operation.end_tiles)
               : EncodeCanonicalDepthMsaaPack(command_buffer, render_target,
                                              operation.start_tiles,
                                              operation.end_tiles);
  }
  if (key.msaa_samples == xenos::MSAASamples::k1X) {
    return materialize
               ? EncodeCanonicalColorMaterialize(
                     command_buffer, render_target, operation.start_tiles,
                     operation.end_tiles)
               : EncodeCanonicalColorPack(command_buffer, render_target,
                                          operation.start_tiles,
                                          operation.end_tiles);
  }
  return materialize
             ? EncodeCanonicalColorMsaaMaterialize(
                   command_buffer, render_target, operation.start_tiles,
                   operation.end_tiles)
             : EncodeCanonicalColorMsaaPack(command_buffer, render_target,
                                            operation.start_tiles,
                                            operation.end_tiles);
}

bool MetalRenderTargetCache::PrepareCurrentRasterViews() {
  if (!sample_edram_raster_enabled_) {
    return true;
  }
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  const uint32_t* lengths = GetLastUpdateUsedRenderTargetLengthsTiles();
  const uint32_t* starts = GetLastUpdateUsedRenderTargetStartsTiles();
  id<MTLCommandBuffer> command_buffer = nil;
  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    RenderTarget* render_target = used[slot];
    if (!render_target || !lengths[slot]) {
      continue;
    }
    // The used span may start past the base (window offset in the EDRAM
    // bases); draw rows are still counted from the base (texture row 0).
    const uint32_t rt_base_tiles = render_target->GetKey().base_tiles;
    const uint32_t start_tiles = rt_base_tiles + starts[slot];
    const uint32_t end_tiles = start_tiles + lengths[slot];
    const uint64_t attachment = GetCanonicalAttachmentId(render_target);
    static const bool s_sync_skip =
        MetalSwitchEnabled(::rex::graphics::metal::GetMetalSettings().metal_raster_sync_skip.Get(),
                           std::getenv("XE_METAL_RASTER_SYNC_SKIP"));
    RasterSyncMemo& memo = raster_sync_memo_[slot];
    if (s_sync_skip && memo.valid && memo.attachment == attachment &&
        memo.start_tiles == start_tiles && memo.end_tiles == end_tiles &&
        memo.version == canonical_authority_.version()) {
      continue;
    }
    // A deferral left by a pass that never opened: materialize it now, the
    // tracker already counts the attachment as clean.
    if (pending_clear_load_[slot].valid) {
      if (!command_buffer) {
        command_buffer = (__bridge id<MTLCommandBuffer>)
            command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kRasterViews));
      }
      if (!FlushPendingClearLoad(slot, (__bridge const void*)command_buffer)) {
        return false;
      }
    }
    auto operations = canonical_authority_.SynchronizeAttachment(
        attachment, start_tiles, end_tiles, ++canonical_authority_order_);
    sync_requester_ = CanonicalSyncRequester::kRasterViews;
    if (MetalCommandProcessor::PassCensusEnabled() && !operations.empty()) {
      NoteRasterPairs(attachment, operations);
    }
    // Clear load action: when every tile of this colour attachment is about
    // to be materialized from canonical and canonical holds one untouched
    // whole-tile clear value there, defer the materializes and let the pass
    // open with a clear of that value (TakePendingClearLoad). Packs for other
    // attachments in the list still encode first, and they reset the tile
    // state they rewrite, so the check below sees their effect.
    size_t materialize_first = operations.size();
    for (size_t i = 0; i < operations.size(); ++i) {
      if (operations[i].kind ==
          MetalEdramAuthorityTracker::SyncKind::kMaterializeFromCanonical) {
        materialize_first = std::min(materialize_first, i);
      }
    }
    bool defer_clear = false;
    uint32_t defer_lo = 0, defer_hi = 0;
    // XE_METAL_CLEAR_LOAD_LOG=1: why a materialize did not become a clear.
    static const bool s_clear_load_log =
        std::getenv("XE_METAL_CLEAR_LOAD_LOG") != nullptr;
    auto clear_load_decline = [&](const char* why, uint32_t a, uint32_t b) {
      static uint32_t s_logs = 0;
      if (s_clear_load_log && s_logs++ < 64) {
        REXLOG_WARN(
            "MetalClearLoad decline slot={} fmt={} depth={} tiles=[{},{}) "
            "ops={} first_mat={} why={} ({}, {})",
            slot, uint32_t(render_target->GetKey().resource_format),
            render_target->GetKey().is_depth ? 1 : 0, start_tiles, end_tiles,
            operations.size(), materialize_first, why, a, b);
      }
    };
    const bool clear_load_format =
        render_target->GetKey().is_depth
            ? slot == 0
            : (slot != 0 &&
               (render_target->GetKey().GetColorFormat() ==
                    xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                render_target->GetKey().GetColorFormat() ==
                    xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA));
    if (ClearLoadEnabled() && materialize_first < operations.size() &&
        !clear_load_format) {
      clear_load_decline("format", 0, 0);
    }
    // Overwrite skip: this draw writes every sample of the whole span, so the
    // materialized content is never observed (any format: the clear value is
    // arbitrary).
    bool overwrite = false;
    if (((draw_overwrite_slots_ >> slot) & 1) &&
        materialize_first < operations.size() && end_tiles > start_tiles &&
        end_tiles <= ::rex::xenos::kEDRAMTileCount) {
      const RenderTargetKey& rt_key = render_target->GetKey();
      const uint32_t pitch = rt_key.GetPitchTiles();
      const uint32_t tile_width =
          ::rex::xenos::kEDRAMTileWidthSamples >>
          uint32_t(rt_key.msaa_samples >= xenos::MSAASamples::k4X);
      const uint32_t tile_height =
          ::rex::xenos::kEDRAMTileHeightSamples >>
          uint32_t(rt_key.msaa_samples >= xenos::MSAASamples::k2X);
      if (pitch && start_tiles >= rt_base_tiles) {
        const float y0 =
            float((start_tiles - rt_base_tiles) / pitch * tile_height);
        const float y1 = float(
            (end_tiles - rt_base_tiles + pitch - 1) / pitch * tile_height);
        overwrite = draw_overwrite_rect_[0] <= 0.0f &&
                    draw_overwrite_rect_[2] >= float(pitch * tile_width) &&
                    draw_overwrite_rect_[1] <= y0 &&
                    draw_overwrite_rect_[3] >= y1;
        static const bool s_ow_log =
            std::getenv("XE_METAL_OVERWRITE_LOG") != nullptr;
        static uint32_t s_ow_logs[1 + xenos::kMaxColorRenderTargets] = {};
        if (s_ow_log && s_ow_logs[slot] < 100) {
          ++s_ow_logs[slot];
          REXLOG_WARN("MetalOverwrite slot={} {} rect=[{},{})x[{},{}) region "
                 "w={} rows=[{},{}) msaa={} ops={} first_mat={}",
                 slot, overwrite ? "COVERED" : "short", draw_overwrite_rect_[0],
                 draw_overwrite_rect_[2], draw_overwrite_rect_[1],
                 draw_overwrite_rect_[3], pitch * tile_width, y0, y1,
                 uint32_t(rt_key.msaa_samples), operations.size(),
                 materialize_first);
        }
      }
    }
    PendingClearLoad defer_value;
    if (((ClearLoadEnabled() && clear_load_format) || overwrite) &&
        materialize_first < operations.size() && end_tiles > start_tiles &&
        end_tiles <= ::rex::xenos::kEDRAMTileCount) {
      // Encode the packs (they precede the materializes), then judge.
      for (size_t i = 0; i < materialize_first; ++i) {
        if (!command_buffer) {
          command_buffer = (__bridge id<MTLCommandBuffer>)
              command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kRasterViews));
        }
        if (!command_buffer ||
            !EncodeCanonicalAuthorityOperation((__bridge const void*)command_buffer,
                                               operations[i])) {
          canonical_authority_.ResetUnknown();
          ResetTileClears();
          return false;
        }
      }
      // Coverage: the materializes must tile [start, end) exactly, in order.
      uint32_t cursor = start_tiles;
      bool covered = true;
      for (size_t i = materialize_first; i < operations.size() && covered; ++i) {
        const auto& op = operations[i];
        if (op.kind != MetalEdramAuthorityTracker::SyncKind::kMaterializeFromCanonical ||
            op.attachment != attachment || op.start_tiles != cursor ||
            op.end_tiles <= op.start_tiles) {
          covered = false;
          break;
        }
        cursor = op.end_tiles;
      }
      covered = covered && cursor == end_tiles;
      if (!covered) {
        clear_load_decline("coverage", cursor,
                           uint32_t(operations.size() - materialize_first));
      }
      if (covered && overwrite) {
        defer_value = PendingClearLoad{};
        defer_clear = true;
        ++pack_census_.overwrite_skips;
        pack_census_.overwrite_skip_tiles += end_tiles - start_tiles;
      } else if (covered && !(ClearLoadEnabled() && clear_load_format)) {
        // Overwrite-only candidate whose materializes are not one clean run.
      } else if (covered) {
        const TileClear& first = tile_clear_[start_tiles];
        bool uniform = first.uniform != 0;
        uint32_t first_bad = start_tiles;
        for (uint32_t t = start_tiles + 1; t < end_tiles && uniform; ++t) {
          const TileClear& tc = tile_clear_[t];
          uniform = tc.uniform && tc.lo == first.lo && tc.hi == first.hi;
          if (!uniform) {
            first_bad = t;
          }
        }
        if (uniform) {
          if (ComputeClearLoadValue(render_target->GetKey(), first.lo, first.hi,
                                    &defer_value)) {
            defer_clear = true;
            defer_lo = first.lo;
            defer_hi = first.hi;
          } else {
            clear_load_decline("value", first.lo, first.hi);
          }
        } else {
          clear_load_decline("uniform", first_bad, first.uniform);
        }
      }
      if (!defer_clear) {
        // Fall through to the ordinary encode of the materializes.
        operations.erase(operations.begin(),
                         operations.begin() + ptrdiff_t(materialize_first));
      }
    }
    if (defer_clear) {
      if (slot == 0) {
        // The pass opens on the clear (or the draw overwrites) instead of a
        // materialize, which would have invalidated the cached stencil.
        InvalidateMsaaStencilCache(
            static_cast<MetalRenderTarget*>(render_target)->msaa_texture(),
            kMsaaStencilClearLoad);
      }
      PendingClearLoad& pending = pending_clear_load_[slot];
      pending.valid = true;
      pending.attachment = attachment;
      for (uint32_t c = 0; c < 4; ++c) {
        pending.color[c] = defer_value.color[c];
      }
      pending.depth = defer_value.depth;
      pending.stencil = defer_value.stencil;
      (void)defer_lo;
      (void)defer_hi;
      pending.operations.assign(operations.begin() + ptrdiff_t(materialize_first),
                                operations.end());
      // The clear needs a NEW pass: close the open render encoder exactly as
      // a materialize transfer would have.
      if (!command_buffer) {
        command_buffer = (__bridge id<MTLCommandBuffer>)
            command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kRasterViews));
      }
      memo = {attachment, start_tiles, end_tiles, canonical_authority_.version(),
              true};
      continue;
    }
    for (const auto& operation : operations) {
      if (MetalCommandProcessor::PassCensusEnabled() &&
          operation.kind == MetalEdramAuthorityTracker::SyncKind::
                                kMaterializeFromCanonical &&
          operation.attachment == attachment) {
        NoteMaterializeFate(slot, attachment, operation.start_tiles,
                            operation.end_tiles);
      }
      // Attribution probe (frame output is wrong):
      // XE_METAL_PROBE_SKIP_MATERIALIZE=1 drops raster-view materializes to
      // size their GPU cost (GTA IV M4 mini 55.5 -> 60 fps cap, 2026-10-29).
      static const bool s_skip_materialize =
          std::getenv("XE_METAL_PROBE_SKIP_MATERIALIZE") != nullptr;
      if (s_skip_materialize &&
          operation.kind == MetalEdramAuthorityTracker::SyncKind::
                                kMaterializeFromCanonical) {
        continue;
      }
      if (!command_buffer) {
        command_buffer = (__bridge id<MTLCommandBuffer>)
            command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kRasterViews));
      }
      if (!command_buffer || !EncodeCanonicalAuthorityOperation(
                                 (__bridge const void*)command_buffer,
                                 operation)) {
        REXLOG_ERROR(
            "MetalRenderTargetCache: canonical authority operation failed "
            "for attachment {:016X}, tiles [{},{})",
            operation.attachment, operation.start_tiles,
            operation.end_tiles);
        // The scheduler has already advanced optimistically. Forget all
        // authority rather than claiming a conversion that wasn't encoded.
        canonical_authority_.ResetUnknown();
    ResetTileClears();
        return false;
      }
    }
    memo = {attachment, start_tiles, end_tiles, canonical_authority_.version(),
            true};
  }
  draw_overwrite_slots_ = 0;
  return true;
}

bool MetalRenderTargetCache::PrepareCanonicalEdramForReadback() {
  if (!sample_edram_raster_enabled_) {
    return true;
  }
  auto operations = canonical_authority_.SynchronizeCanonical(
      0, ::rex::xenos::kEDRAMTileCount, ++canonical_authority_order_);
  if (operations.empty()) {
    return true;
  }
  sync_requester_ = CanonicalSyncRequester::kReadback;
  id<MTLCommandBuffer> command_buffer = (__bridge id<MTLCommandBuffer>)
      command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kCanonicalReadback));
  if (!command_buffer) {
    canonical_authority_.ResetUnknown();
    ResetTileClears();
    return false;
  }
  for (const auto& operation : operations) {
    if (!EncodeCanonicalAuthorityOperation(
            (__bridge const void*)command_buffer, operation)) {
      REXLOG_ERROR(
          "MetalRenderTargetCache: canonical readback pack failed for "
          "attachment {:016X}, tiles [{},{})",
          operation.attachment, operation.start_tiles, operation.end_tiles);
      canonical_authority_.ResetUnknown();
    ResetTileClears();
      return false;
    }
  }
  return true;
}

bool MetalRenderTargetCache::PrepareCanonicalEdramForResolve(
    uint32_t span_base, uint32_t row_length_used, uint32_t rows,
    uint32_t span_pitch) {
  if (!sample_edram_raster_enabled_ || !row_length_used || !rows) {
    return true;
  }
  id<MTLCommandBuffer> command_buffer = nil;
  auto synchronize = [&](uint32_t start, uint32_t end) {
    auto operations = canonical_authority_.SynchronizeCanonical(
        start, end, ++canonical_authority_order_);
    sync_requester_ = CanonicalSyncRequester::kResolve;
    for (const auto& operation : operations) {
      if (!command_buffer) {
        command_buffer = (__bridge id<MTLCommandBuffer>)
            command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kCanonicalResolve));
      }
      if (!command_buffer || !EncodeCanonicalAuthorityOperation(
                                 (__bridge const void*)command_buffer,
                                 operation)) {
        REXLOG_ERROR(
            "MetalRenderTargetCache: canonical resolve pack failed for "
            "attachment {:016X}, tiles [{},{})",
            operation.attachment, operation.start_tiles,
            operation.end_tiles);
        canonical_authority_.ResetUnknown();
    ResetTileClears();
        return false;
      }
    }
    return true;
  };
  // A full-width span (every row covers its whole pitch) is one contiguous
  // tile interval, so synchronize it at once: the tracker then emits one pack
  // per source run instead of one per row (a 48-row resolve issued 48 packs,
  // each dispatching every row above its range), and the command processor
  // walks the tracker once instead of once per row. The packed tile set is
  // identical. XE_METAL_RESOLVE_SPAN_COALESCE=0 restores per-row sync.
  static const bool s_coalesce =
      MetalSwitchEnabled(::rex::graphics::metal::GetMetalSettings().metal_resolve_span_coalesce.Get(),
                         std::getenv("XE_METAL_RESOLVE_SPAN_COALESCE"));
  if (s_coalesce && row_length_used == span_pitch) {
    uint64_t total = std::min<uint64_t>(uint64_t(rows) * span_pitch,
                                        ::rex::xenos::kEDRAMTileCount);
    uint32_t start = span_base & (::rex::xenos::kEDRAMTileCount - 1u);
    uint32_t first_length =
        uint32_t(std::min<uint64_t>(total, ::rex::xenos::kEDRAMTileCount - start));
    if (!synchronize(start, start + first_length)) {
      return false;
    }
    if (first_length < total &&
        !synchronize(0u, uint32_t(total - first_length))) {
      return false;
    }
    return true;
  }
  for (uint32_t row = 0; row < rows; ++row) {
    uint32_t start =
        (span_base + row * span_pitch) & (::rex::xenos::kEDRAMTileCount - 1u);
    uint32_t first_length =
        std::min(row_length_used, ::rex::xenos::kEDRAMTileCount - start);
    if (!synchronize(start, start + first_length)) {
      return false;
    }
    if (first_length < row_length_used &&
        !synchronize(0u, row_length_used - first_length)) {
      return false;
    }
  }
  return true;
}

void MetalRenderTargetCache::MarkCurrentRasterTargetsWritten(
    uint32_t color_write_mask, bool depth_or_stencil_written,
    uint32_t host_y0, uint32_t host_y1) {
  if (!sample_edram_raster_enabled_) {
    return;
  }
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  const uint32_t* lengths = GetLastUpdateUsedRenderTargetLengthsTiles();
  const uint32_t* starts = GetLastUpdateUsedRenderTargetStartsTiles();
  const bool census = MetalCommandProcessor::PassCensusEnabled();
  ++pack_census_.mark_calls;
  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    bool written = slot == 0 ? depth_or_stencil_written
                             : (color_write_mask &
                                (uint32_t(0xFu) << ((slot - 1u) * 4u))) != 0;
    RenderTarget* render_target = used[slot];
    if (!written || !render_target || !lengths[slot]) {
      continue;
    }
    // The used span may start past the base (window offset in the EDRAM
    // bases); draw rows are still counted from the base (texture row 0).
    const uint32_t rt_base_tiles = render_target->GetKey().base_tiles;
    uint32_t start_tiles = rt_base_tiles + starts[slot];
    uint32_t end_tiles = start_tiles + lengths[slot];
    const uint64_t attachment = GetCanonicalAttachmentId(render_target);
    // Tile rows covered by the draw's host scissor rows (whole rows). The
    // raster views are real MSAA textures at PIXEL size (the scissor is in
    // host pixels), and an EDRAM tile is 80x16 samples: 16 pixel rows at 1x,
    // 8 at 2x and 4x (the same convention as RenderTargetKey::GetWidth for
    // the tile width), times the draw resolution scale. An unknown extent
    // (host_y1 == UINT32_MAX) covers everything.
    const uint32_t pitch = render_target->GetKey().GetPitchTiles();
    const uint32_t rows_per_tile =
        (::rex::xenos::kEDRAMTileHeightSamples >>
         uint32_t(render_target->GetKey().msaa_samples >=
                  xenos::MSAASamples::k2X)) *
        GetDrawResolutionScaleY();
    uint32_t d0 = rt_base_tiles + (host_y0 / rows_per_tile) * pitch;
    uint32_t d1 = host_y1 == UINT32_MAX
                      ? end_tiles
                      : rt_base_tiles + ((host_y1 + rows_per_tile - 1) /
                                         rows_per_tile) * pitch;
    d0 = std::max(d0, start_tiles);
    d1 = std::min(std::min(d1, end_tiles), uint32_t(::rex::xenos::kEDRAMTileCount));
    // Draw-row marking (2026-10-04 introduced for USF4, DEFAULT ON
    // 2026-10-18; ::rex::graphics::metal::GetMetalSettings().metal_mark_draw_rows.Get(), XE_METAL_MARK_DRAW_ROWS=0/1): only the
    // draw-covered tile rows become host-authoritative, so a later pack to
    // canonical (before a resolve, or when another attachment takes a tile)
    // moves the rows a draw could have written instead of the whole bound
    // target. Tiles outside the draw's rows keep their previous state:
    // canonical and clean for this attachment (materialized at bind) or
    // already host-authoritative from an earlier draw. F.E.A.R.: resolve packs
    // 272 -> 220 a frame, the census's redundant pack tiles 15% -> 0.
    static const bool s_mark_draw_rows = MetalSwitchEnabled(
        ::rex::graphics::metal::GetMetalSettings().metal_mark_draw_rows.Get(), std::getenv("XE_METAL_MARK_DRAW_ROWS"));
    const uint32_t mark_start = s_mark_draw_rows ? d0 : start_tiles;
    const uint32_t mark_end = s_mark_draw_rows ? d1 : end_tiles;
    if (mark_end <= mark_start) {
      continue;
    }
    static const bool s_mark_skip =
        MetalSwitchEnabled(::rex::graphics::metal::GetMetalSettings().metal_raster_mark_skip.Get(),
                           std::getenv("XE_METAL_RASTER_MARK_SKIP"));
    RasterSyncMemo& memo = raster_mark_memo_[slot];
    ++pack_census_.mark_slots;
    if (s_mark_skip && memo.valid && memo.attachment == attachment &&
        memo.start_tiles == mark_start && memo.end_tiles == mark_end &&
        memo.version == canonical_authority_.version()) {
      ++pack_census_.mark_memo_hits;
      continue;
    }
    pack_census_.mark_tiles += mark_end - mark_start;
    canonical_authority_.MarkAttachment(attachment, mark_start, mark_end,
                                        ++canonical_authority_order_);
    if (census) {
      // Census: a lower bound on pack redundancy. Behind the memo: a draw
      // whose slot memo matched re-dirtied the same tiles, and the walk
      // itself (every tile of every bound target, every draw) was ~10% of
      // the command-processor thread in the attribution runs of GTA IV and
      // Burnout, inflating the very cost they measure (2026-10-17 samples).
      // The pack clears a tile's dirty bit through SynchronizeCanonical,
      // which bumps the authority version and so misses the memo.
      std::vector<uint8_t>& dirty = pack_dirty_tiles_[attachment];
      if (dirty.size() != ::rex::xenos::kEDRAMTileCount) {
        dirty.assign(::rex::xenos::kEDRAMTileCount, 1);
      }
      for (uint32_t t = d0; t < d1; ++t) {
        dirty[t] = 1;
      }
      for (uint32_t t = mark_start; t < mark_end && t < ::rex::xenos::kEDRAMTileCount;
           ++t) {
        census_clear_tiles_[t] = 0;
      }
    }
    memo = {attachment, mark_start, mark_end, canonical_authority_.version(),
            true};
  }
}

uint64_t MetalRenderTargetCache::current_attachment_key() const {
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  uint64_t key = 0;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    key = key * 1000003u ^
          (accumulated[i] ? uint64_t(accumulated[i]->GetKey().key) + 1 : 0);
  }
  return key;
}

uint32_t MetalRenderTargetCache::current_msaa_samples() const {
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  uint32_t samples = 1;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (accumulated[i]) {
      samples = std::max(
          samples, 1u << uint32_t(accumulated[i]->GetKey().msaa_samples));
    }
  }
  return samples;
}

void MetalRenderTargetCache::NoteRasterPairs(
    uint64_t dest,
    const std::vector<MetalEdramAuthorityTracker::SyncOperation>& operations) {
  if (census_last_packer_.size() != ::rex::xenos::kEDRAMTileCount) {
    census_last_packer_.assign(::rex::xenos::kEDRAMTileCount, 0);
  }
  for (const auto& op : operations) {
    const uint32_t start = std::min(op.start_tiles, ::rex::xenos::kEDRAMTileCount);
    const uint32_t end = std::min(op.end_tiles, ::rex::xenos::kEDRAMTileCount);
    if (end <= start) {
      continue;
    }
    if (op.kind ==
        MetalEdramAuthorityTracker::SyncKind::kMaterializeFromCanonical) {
      // Split the materialize by the attachment that last packed each run.
      uint32_t t = start;
      while (t < end) {
        const uint64_t source = census_last_packer_[t];
        uint32_t run_end = t + 1;
        while (run_end < end && census_last_packer_[run_end] == source) {
          ++run_end;
        }
        RasterPairCount& c = raster_pair_census_[{dest, source, true}];
        ++c.ops;
        c.tiles += run_end - t;
        t = run_end;
      }
    } else {
      RasterPairCount& c = raster_pair_census_[{dest, op.attachment, false}];
      ++c.ops;
      c.tiles += end - start;
      for (uint32_t t = start; t < end; ++t) {
        census_last_packer_[t] = op.attachment;
      }
    }
  }
}

std::string MetalRenderTargetCache::FormatRasterPairCensus(bool reset) {
  const double frames = double(std::max<uint64_t>(pack_census_.frames, 1));
  std::vector<std::pair<RasterPairKey, RasterPairCount>> rows(
      raster_pair_census_.begin(), raster_pair_census_.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second.tiles > b.second.tiles;
  });
  auto describe = [this](uint64_t attachment) -> std::string {
    if (!attachment) {
      return "canonical";
    }
    const RenderTarget* rt = FindCanonicalAttachment(attachment);
    return rt ? rt->GetKey().GetDebugName() : fmt::format("gone:{:X}", attachment);
  };
  std::string out = fmt::format("MetalRasterPairCensus frames={}", pack_census_.frames);
  for (size_t i = 0; i < rows.size() && i < 12; ++i) {
    const auto& [key, count] = rows[i];
    out += fmt::format(" | {} {} <- {}: {:.1f} ops {:.0f} tiles/frame",
                       key.materialize ? "mat" : "pack", describe(key.dest),
                       describe(key.source), double(count.ops) / frames,
                       double(count.tiles) / frames);
  }
  if (reset) {
    raster_pair_census_.clear();
  }
  return out;
}

void MetalRenderTargetCache::NoteMaterializeFate(uint32_t slot,
                                                 uint64_t attachment,
                                                 uint32_t start_tiles,
                                                 uint32_t end_tiles) {
  MaterializeFate& fate = materialize_fate_[slot];
  if (fate.pending.size() != ::rex::xenos::kEDRAMTileCount) {
    fate.pending.assign(::rex::xenos::kEDRAMTileCount, 0);
  }
  if (fate.attachment != attachment) {
    pack_census_.fate_untouched += fate.pending_count;
    std::fill(fate.pending.begin(), fate.pending.end(), uint8_t(0));
    fate.pending_count = 0;
    fate.attachment = attachment;
  }
  end_tiles = std::min(end_tiles, uint32_t(::rex::xenos::kEDRAMTileCount));
  for (uint32_t t = start_tiles; t < end_tiles; ++t) {
    if (!fate.pending[t]) {
      fate.pending[t] = 1;
      ++fate.pending_count;
    } else {
      // Materialized again before any draw touched it.
      ++pack_census_.fate_untouched;
    }
    ++pack_census_.fate_tiles;
  }
}

void MetalRenderTargetCache::NoteMaterializeFirstUse(
    uint32_t color_write_mask, uint32_t color_read_mask, bool depth_written,
    bool depth_read, bool full_width, bool rect, uint32_t host_y0,
    uint32_t host_y1) {
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  const uint32_t* lengths = GetLastUpdateUsedRenderTargetLengthsTiles();
  const uint32_t* starts = GetLastUpdateUsedRenderTargetStartsTiles();
  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    MaterializeFate& fate = materialize_fate_[slot];
    RenderTarget* render_target = used[slot];
    if (!fate.pending_count || !render_target || !lengths[slot] ||
        GetCanonicalAttachmentId(render_target) != fate.attachment) {
      continue;
    }
    const uint32_t nibble =
        slot ? (color_write_mask >> ((slot - 1u) * 4u)) & 0xFu : 0u;
    const bool reads =
        slot ? ((color_read_mask >> (slot - 1u)) & 1u) != 0 : depth_read;
    const bool writes = slot ? nibble != 0 : depth_written;
    if (!reads && !writes) {
      continue;
    }
    const bool overwrites =
        !reads && full_width && (slot == 0 || nibble == 0xFu);
    // The tile rows the draw's scissor covers, as in
    // MarkCurrentRasterTargetsWritten.
    // The used span may start past the base (window offset in the EDRAM
    // bases); draw rows are still counted from the base (texture row 0).
    const uint32_t rt_base_tiles = render_target->GetKey().base_tiles;
    const uint32_t start_tiles = rt_base_tiles + starts[slot];
    const uint32_t end_tiles = start_tiles + lengths[slot];
    const uint32_t pitch = render_target->GetKey().GetPitchTiles();
    const uint32_t rows_per_tile =
        (::rex::xenos::kEDRAMTileHeightSamples >>
         uint32_t(render_target->GetKey().msaa_samples >=
                  xenos::MSAASamples::k2X)) *
        GetDrawResolutionScaleY();
    uint32_t d0 = rt_base_tiles + (host_y0 / rows_per_tile) * pitch;
    uint32_t d1 = host_y1 == UINT32_MAX
                      ? end_tiles
                      : rt_base_tiles + ((host_y1 + rows_per_tile - 1) /
                                         rows_per_tile) * pitch;
    // A partly covered first or last tile row counts as read.
    const uint32_t partial_first =
        host_y0 % rows_per_tile ? start_tiles + (host_y0 / rows_per_tile) * pitch
                                : UINT32_MAX;
    const uint32_t partial_last =
        host_y1 != UINT32_MAX && host_y1 % rows_per_tile
            ? start_tiles + (host_y1 / rows_per_tile) * pitch
            : UINT32_MAX;
    d0 = std::max(d0, start_tiles);
    d1 = std::min(std::min(d1, end_tiles), uint32_t(::rex::xenos::kEDRAMTileCount));
    for (uint32_t t = d0; t < d1; ++t) {
      if (!fate.pending[t]) {
        continue;
      }
      fate.pending[t] = 0;
      --fate.pending_count;
      const uint32_t row = t - (t - start_tiles) % pitch;
      if (overwrites && row != partial_first && row != partial_last) {
        ++pack_census_.fate_overwritten;
        if (rect) {
          ++pack_census_.fate_overwritten_rect;
        }
      } else {
        ++pack_census_.fate_read;
      }
    }
  }
}

std::string MetalRenderTargetCache::FormatPackCensus(bool reset) {
  static const char* kNames[] = {"rasterviews", "readback", "resolve", "other"};
  PackCensus& c = pack_census_;
  const double frames = double(std::max<uint64_t>(c.frames, 1));
  const double tile_mb = double(::rex::xenos::kEDRAMTileWidthSamples * ::rex::xenos::kEDRAMTileHeightSamples * 4u) *
                         GetDrawResolutionScaleX() * GetDrawResolutionScaleY() /
                         (1024.0 * 1024.0);
  std::string out = fmt::format("MetalPackCensus frames={}", c.frames);
  for (uint32_t r = 0; r < 4; ++r) {
    if (!c.pack_ops[r] && !c.materialize_ops[r]) {
      continue;
    }
    out += fmt::format(
        " | {}: pack {:.1f}/frame {:.1f} MB/frame redundant {:.1f}% "
        "(all-redundant ops {:.1f}/frame) materialize {:.1f}/frame {:.1f} "
        "MB/frame",
        kNames[r], double(c.pack_ops[r]) / frames,
        double(c.pack_tiles[r]) * tile_mb / frames,
        c.pack_tiles[r] ? 100.0 * double(c.pack_tiles_redundant[r]) /
                              double(c.pack_tiles[r])
                        : 0.0,
        double(c.pack_ops_all_redundant[r]) / frames,
        double(c.materialize_ops[r]) / frames,
        double(c.materialize_tiles[r]) * tile_mb / frames);
  }
  out += fmt::format(
      " | clears={} clear_tiles={} whole_tile_clears={} "
      "materialize_tiles_from_clear={} attachment_clears={}",
      c.clears, c.clear_tiles, c.clears_whole_tiles,
      c.materialize_tiles_from_clear, c.attachment_clears);
  out += fmt::format(
      " | mark: calls/frame {:.0f} slots/frame {:.0f} memo-hit {:.1f}% "
      "tiles/frame {:.0f}",
      double(c.mark_calls) / frames, double(c.mark_slots) / frames,
      c.mark_slots ? 100.0 * double(c.mark_memo_hits) / double(c.mark_slots)
                   : 0.0,
      double(c.mark_tiles) / frames);
  out += fmt::format(" | clear-load: {:.2f}/frame ({:.0f} tiles/frame) flushed {:.2f}/frame",
                     double(c.clear_loads) / frames,
                     double(c.clear_load_tiles) / frames,
                     double(c.clear_load_flushes) / frames);
  out += fmt::format(" | overwrite-skip: {:.2f}/frame ({:.0f} tiles/frame)",
                     double(c.overwrite_skips) / frames,
                     double(c.overwrite_skip_tiles) / frames);
  if (c.fate_tiles) {
    out += fmt::format(
        " | materialize fate: tiles/frame {:.0f} read {:.1f}% overwritten "
        "{:.1f}% (rect {:.1f}%) untouched {:.1f}%",
        double(c.fate_tiles) / frames,
        100.0 * double(c.fate_read) / double(c.fate_tiles),
        100.0 * double(c.fate_overwritten) / double(c.fate_tiles),
        100.0 * double(c.fate_overwritten_rect) / double(c.fate_tiles),
        100.0 * double(c.fate_untouched) / double(c.fate_tiles));
  }
  if (reset) {
    c = PackCensus{};
  }
  return out;
}

std::string MetalRenderTargetCache::DescribeCurrentAttachments() const {
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  std::string out;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    // Index 0 is depth; 1..kMaxColorRenderTargets are colour (the same
    // indexing current_attachment_key hashes over).
    if (i) {
      out += ' ';
    }
    if (!accumulated[i]) {
      out += fmt::format("{}{}=none", i ? "color" : "depth", i ? i - 1 : 0);
      continue;
    }
    RenderTargetKey k = accumulated[i]->GetKey();
    out += fmt::format(
        "{}{}=(key={:08X} base_tiles={} pitch32={} msaa={} is_depth={} fmt={})",
        i ? "color" : "depth", i ? i - 1 : 0, k.key, uint32_t(k.base_tiles),
        uint32_t(k.pitch_tiles_at_32bpp), uint32_t(k.msaa_samples),
        uint32_t(k.is_depth), uint32_t(k.resource_format));
  }
  return out;
}

void MetalRenderTargetCache::PerformTransfers(
    RenderTarget* const* render_targets,
    const std::vector<Transfer>* transfer_lists) {
  // Stage B: the explicit JOIN between the shared two-source plan and the
  // canonical operation stream. Ranges are recorded per two-source transfer
  // (RecordTransition) and consulted by the canonical depth materialize
  // encoders; the scope is THIS transfer batch only — 8551 proved that
  // reconciling ordinary materializes (out-of-plan lane state) corrupts
  // 1.5M EDRAM words while in-plan reconciliation is exact.
  two_source_records_.clear();
  bool any = false;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets && !any; ++i) {
    any = render_targets[i] && !transfer_lists[i].empty();
  }
  if (!any) {
    return;
  }
  // Transfers are encoded as standalone blits at a pass boundary before the
  // draw's encoder opens.
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)
      command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kOwnershipTransfer));
  if (!cb) {
    return;
  }
  id<MTLBlitCommandEncoder> blit = nil;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    auto* dest = static_cast<MetalRenderTarget*>(render_targets[i]);
    if (!dest) {
      continue;
    }
    RenderTargetKey dest_key = dest->GetKey();
    for (const Transfer& transfer : transfer_lists[i]) {
      auto* source = static_cast<MetalRenderTarget*>(transfer.source);
      if (!source) {
        continue;
      }
      RenderTargetKey src_key = source->GetKey();
      uint32_t sxs, sys, dxs, dys;
      GetMsaaShifts(uint32_t(src_key.msaa_samples), sxs, sys);
      GetMsaaShifts(uint32_t(dest_key.msaa_samples), dxs, dys);
      // Classify the ownership transition with the shared, unit-tested decision
      // function (render_target_transfer_class.h) — the SAME predicates that
      // drive the branch cascade below. This labels the transition manifest and
      // gives the L2/L3b conformance rung an independent expected kind. Purely
      // observational: it does not change which branch runs.
      TransferClassInput tci;
      tci.src_is_depth = src_key.is_depth != 0;
      tci.dst_is_depth = dest_key.is_depth != 0;
      tci.src_is_64bpp = src_key.Is64bpp();
      tci.dst_is_64bpp = dest_key.Is64bpp();
      tci.src_msaa = uint32_t(src_key.msaa_samples);
      tci.dst_msaa = uint32_t(dest_key.msaa_samples);
      tci.host_format_equal = source->pixel_format() == dest->pixel_format();
      tci.src_is_color8888 = source->pixel_format() == kColorFormat;
      tci.dst_is_color8888 = dest->pixel_format() == kColorFormat;
      tci.src_is_r32f =
          source->pixel_format() == uint64_t(MTLPixelFormatR32Float);
      tci.dst_is_r32f =
          dest->pixel_format() == uint64_t(MTLPixelFormatR32Float);
      TransferKind kind = ClassifyTransfer(tci);
      RecordTransition(kind, src_key, dest_key, transfer);
      auto canonical_format_supported = [](const RenderTargetKey& key) {
        if (key.is_depth) {
          return true;
        }
        return IsCanonicalColorFormatSupported(key.GetColorFormat());
      };
      if (ShouldUseCanonicalTransferPath(sample_edram_raster_enabled_,
                                         canonical_format_supported(src_key),
                                         canonical_format_supported(dest_key))) {
        // Tasks 4.1/4.2: canonical raw words make exact-format copies,
        // cross-MSAA ownership, color-format reinterpretation, and
        // color<->depth/stencil aliasing the same operation. The scheduler
        // packs through the producer's interpretation, then materializes
        // through the consumer's, preserving the physical words in between.
        // Two-word 64bpp views use the same physical scheduler with wide
        // materialize/pack kernels.
        if (blit) {
          [blit endEncoding];
          blit = nil;
        }
        continue;
      }
      if (source->pixel_format() == dest->pixel_format() && sxs == dxs &&
          sys == dys) {
        // Same host format + same MSAA (equal pixel block sizes): copy
        // tile-by-tile. An EDRAM tile is 80x16 SAMPLES = an
        // (80>>xs)x(16>>ys)-pixel block at a computable position per layout.
        if (!blit) {
          blit = [cb blitCommandEncoder];
          blit.label = @"rtcache:transfer-blit";
        }
        static uint32_t s_blit_logs = 0;
        if (s_blit_logs++ < 256) {
          REXLOG_WARN("MetalXferBlit @draw {}: {} -> {} tiles [{},{})",
                 command_processor_.trace_draw_index(),
                 src_key.GetDebugName(), dest_key.GetDebugName(),
                 transfer.start_tiles, transfer.end_tiles);
        }
        id<MTLTexture> src_tex = (__bridge id<MTLTexture>)source->texture();
        id<MTLTexture> dst_tex = (__bridge id<MTLTexture>)dest->texture();
        uint32_t tile_w_samples = src_key.Is64bpp() ? 40 : 80;
        uint32_t bw = tile_w_samples >> sxs, bh = 16 >> sys;
        uint32_t src_pitch = src_key.GetPitchTiles();
        uint32_t dst_pitch = dest_key.GetPitchTiles();
        for (uint32_t tile = transfer.start_tiles; tile < transfer.end_tiles;
             ++tile) {
          uint32_t sl = tile - src_key.base_tiles;
          uint32_t dl = tile - dest_key.base_tiles;
          uint32_t sx = ((sl % src_pitch) * tile_w_samples) >> sxs,
                   sy = ((sl / src_pitch) * 16) >> sys;
          uint32_t dx = ((dl % dst_pitch) * tile_w_samples) >> dxs,
                   dy = ((dl / dst_pitch) * 16) >> dys;
          if (sx + bw > src_tex.width || sy + bh > src_tex.height ||
              dx + bw > dst_tex.width || dy + bh > dst_tex.height) {
            continue;
          }
          [blit copyFromTexture:src_tex
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(sx, sy, 0)
                     sourceSize:MTLSizeMake(bw, bh, 1)
                      toTexture:dst_tex
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(dx, dy, 0)];
        }
      } else if (source->pixel_format() == dest->pixel_format() &&
                 src_key.is_depth == dest_key.is_depth &&
                 !src_key.Is64bpp() && !dest_key.Is64bpp()) {
        // Same host format, different MSAA sample density: per-tile pixel
        // blocks have different sizes, so remap through a sampling pass.
        if (blit) {
          [blit endEncoding];
          blit = nil;
        }
        if (canonical_depth_enabled_ && src_key.is_depth && dest_key.is_depth &&
            canonical_edram_) {
          // Option 1: route the depth ownership flip through the durable
          // canonical EDRAM authority (pack source -> canonical, materialize
          // dest <- canonical) instead of the lossy legacy spatial resample.
          CanonicalDepthTransfer((__bridge const void*)cb, source, dest,
                                 transfer);
        } else {
          PerformCrossMsaaTransfer((__bridge const void*)cb, source, dest,
                                   transfer);
        }
      } else if (dest_key.is_depth && !src_key.is_depth &&
                 source->pixel_format() == kColorFormat) {
        // Color (raw EDRAM bits in a BGRA8 texture, e.g. the restored
        // snapshot) reinterpreted as depth: unpack the 24-bit guest depth and
        // write host D32F via a depth-only pass.
        if (blit) {
          [blit endEncoding];
          blit = nil;
        }
        PerformColorToDepthTransfer((__bridge const void*)cb, source, dest,
                                    transfer);
        // Lineage (who lent these tiles) is recorded centrally in
        // RecordTransition, keyed by RenderTargetKey values.
      } else if (!dest_key.is_depth && src_key.is_depth &&
                 dest->pixel_format() == kColorFormat) {
        // Depth reinterpreted as raw-bits color (a color pass reuses tiles a
        // depth RT last owned). DEFAULT: SKIP — the color key keeps its own
        // previous content ("key-local EDRAM persistence"). Both consumer
        // patterns want that: JC2's tonemap draws BLEND AGAINST DESTINATION
        // over these tiles (writing zeros here rendered the casino's dark
        // rectangles; real depth bits render as garbage the multiply
        // amplifies), and the composite's depth-owned resolve spans read
        // whatever the chain last produced, exactly like the mainline truth
        // leg. XE_METAL_D2C_REAL=1 encodes the real depth bits instead
        // (hardware-accurate raw reinterpret, for titles that genuinely
        // read them).
        static const bool s_d2c_real = std::getenv("XE_METAL_D2C_REAL");
        // Same-frame ping-pong rule, now through the key-identity classifier
        // (Sonic's DoF pass writes a packed motion-vector/CoC map as 4x
        // depth bits over the blur buffer's tiles and the composite decodes
        // the raw bits — skipping left stale scene pixels decoded as gather
        // vectors: the melted mip / white screen). Verdicts SPLIT: only the
        // proven intersections transport real bits; the remainder keeps
        // key-local persistence. XE_METAL_D2C_REAL=1 forces the whole range
        // real (unchanged legacy A/B override).
        std::vector<MetalD2cSegment> segments;
        if (s_d2c_real) {
          segments.push_back({transfer.start_tiles, transfer.end_tiles,
                              MetalD2cVerdict::kRealTransfer, "forced"});
        } else {
          segments = d2c_lineage_.Classify(
              dest_key.key, src_key.key, transfer.start_tiles,
              transfer.end_tiles, command_processor_.swap_count(),
              /*allow_cross_frame=*/false);
          // Review-mandated measurement BEFORE any cross-frame enablement.
          uint32_t cross_frame_tiles =
              d2c_lineage_.CountCrossFrameCandidateTiles(
                  dest_key.key, src_key.key, transfer.start_tiles,
                  transfer.end_tiles, command_processor_.swap_count());
          if (cross_frame_tiles) {
            static uint32_t s_cf_logs = 0;
            if (s_cf_logs++ < 32) {
              REXLOG_INFO("MetalD2C cross-frame candidate: {} tiles {} -> {} "
                     "(measured, not acted on)",
                     cross_frame_tiles, src_key.GetDebugName(),
                     dest_key.GetDebugName());
            }
          }
        }
        for (const MetalD2cSegment& segment : segments) {
          if (segment.verdict != MetalD2cVerdict::kRealTransfer) {
            continue;
          }
          if (blit) {
            [blit endEncoding];
            blit = nil;
          }
          static uint32_t s_pp_logs = 0;
          if (s_pp_logs++ < 16) {
            REXLOG_WARN("MetalXferD2C @draw {}: {} {} -> {} tiles [{},{}) — real "
                   "bit transport",
                   command_processor_.trace_draw_index(), segment.reason,
                   src_key.GetDebugName(), dest_key.GetDebugName(),
                   segment.start_tiles, segment.end_tiles);
          }
          Transfer segment_transfer = transfer;
          segment_transfer.start_tiles = segment.start_tiles;
          segment_transfer.end_tiles = segment.end_tiles;
          PerformDepthToColorTransfer((__bridge const void*)cb, source, dest,
                                      segment_transfer, !s_d2c_real);
        }
        // Consume-once: this return (executed or skipped) is the tiles' next
        // ownership movement — an earlier lend can no longer prove a later
        // return without a fresh c2d.
        d2c_lineage_.SupersedeRange(transfer.start_tiles, transfer.end_tiles);
      } else if (!src_key.is_depth && !dest_key.is_depth &&
                 !src_key.Is64bpp() && !dest_key.Is64bpp() &&
                 ((source->pixel_format() == kColorFormat &&
                   dest->pixel_format() == uint64_t(MTLPixelFormatR32Float)) ||
                  (source->pixel_format() == uint64_t(MTLPixelFormatR32Float) &&
                   dest->pixel_format() == kColorFormat))) {
        // Color<->color 32bpp REINTERPRET: the EDRAM word is identical, only
        // the host container differs (BGRA8 bytes vs f32 bits). JC2's
        // auto-exposure pyramid renders into k_32_FLOAT-keyed tiles that then
        // alias k_8_8_8_8 keys (and vice versa); this was the last log-skipped
        // transfer class — the luminance content stayed stranded in the other
        // key's texture and the exposure loop read zeros (the black flicker).
        if (blit) {
          [blit endEncoding];
          blit = nil;
        }
        PerformColorReinterpretTransfer((__bridge const void*)cb, source, dest,
                                        transfer);
      } else if (!src_key.is_depth && !dest_key.is_depth &&
                 !src_key.Is64bpp() && !dest_key.Is64bpp()) {
        // General 32bpp color->color transfer: encode the source texel to the
        // raw EDRAM word (per source host format), decode per dest format,
        // with the full tile remap (handles reinterpret AND cross-MSAA in one
        // pass). Covers the combos the specific handlers don't — Sonic's HDR
        // chain runs on k_2_10_10_10_FLOAT (host RGBA16F, 7e3-packed words)
        // and every one of its ownership handoffs to/from 8888 was skipped,
        // so the bloom pyramid read stale content (the scrambled top band).
        if (blit) {
          [blit endEncoding];
          blit = nil;
        }
        PerformColorConvertTransfer((__bridge const void*)cb, source, dest,
                                    transfer);
      } else {
        // Stage D 5.2: BOUNDED, reason-coded residual report. This branch is
        // unreachable across the current corpus (all engaged classes route
        // canonically or through the legacy branches above) — if it EVER
        // fires, the report must carry everything triage needs without
        // replaying: source/destination formats + sample counts (the debug
        // names), exact tile range, and transition order. Deduplicated by
        // key pair, capped, unconditional (a residual gated behind a
        // diagnostic env is a silent skip with extra steps). A 64bpp key
        // pair appearing here is the reopening tripwire for the unclaimed
        // 64bpp transfer classes.
        static std::set<uint64_t> s_residual_pairs;
        static uint32_t s_residual_reports = 0;
        uint64_t pair_key =
            (uint64_t(src_key.key) << 32) | uint64_t(dest_key.key);
        if (s_residual_pairs.insert(pair_key).second &&
            s_residual_reports < 32) {
          ++s_residual_reports;
          REXLOG_ERROR(
              "MetalXferResidual[{}]: {} -> {} tiles [{},{}) transition #{} "
              "@draw {}{}",
              s_residual_reports, src_key.GetDebugName(),
              dest_key.GetDebugName(), transfer.start_tiles,
              transfer.end_tiles, transition_counter_,
              command_processor_.trace_draw_index(),
              (src_key.Is64bpp() || dest_key.Is64bpp())
                  ? " [64BPP CLASS — unclaimed-coverage tripwire]"
                  : "");
          if (s_residual_reports == 32) {
            REXLOG_ERROR("MetalXferResidual: report cap reached; further distinct "
                   "pairs suppressed");
          }
        }
      }
    }
  }
  if (blit) {
    [blit endEncoding];
  }
  if (canonical_full_sync_enabled_) {
    ++canonical_full_sync_count_;
    if (canonical_full_sync_count_ <= 64 ||
        (canonical_full_sync_count_ & 255u) == 0) {
      REXLOG_WARN(
          "MetalCanonicalFullSync #{} @draw {}: ownership-transfer batch "
          "commit+wait",
          canonical_full_sync_count_, command_processor_.trace_draw_index());
    }
    command_processor_.EndFrameEncoderAndWait(
        metal::SeamBoundaryReason::kDebugProbe);
  }
}

float MetalRenderTargetCache::CanonicalDepthTransferResult(
    float host_depth, xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24S8) {
    // CPU mirrors of xe_f32_to_unorm24 / xe_unorm24_to_f32 (metal_edram_msl.h):
    // rint() rounds half to even under the default rounding mode, as MSL rint.
    float clamped = std::min(std::max(host_depth, 0.0f), 1.0f);
    uint32_t t = std::min(uint32_t(std::nearbyint(clamped * 16777216.0f)),
                          uint32_t(16777216u));
    uint32_t n24 = (t > 8388608u ? t - 1u : t) & 0xFFFFFFu;
    return float(n24 + (n24 >> 23)) * (1.0f / 16777216.0f);
  }
  // float24: pack = xe_f32_to_f20e4(d * 2) (truncating), materialize =
  // xe_f20e4_to_f32(d24) * 0.5, unless the lane reconcile restores the host
  // value (xe_host_depth_reconcile: the arm's rounding of d * 2 reproduces the
  // packed bits).
  uint32_t d24 = xenos::Float32To20e4(host_depth * 2.0f, false);
  uint32_t lane_mode = HostLaneMode(/*materialize=*/true);
  bool reconcile =
      lane_mode != 0 &&
      MetalDepthToDepthCarriesHostPrecision(
          host_depth_d2d_enabled_ && host_depth_gate_enabled_ &&
              !f24_ps_engaged_,
          /*has_planner_host_source=*/false, true, true,
          xenos::DepthRenderTargetFormat::kD24FS8,
          xenos::DepthRenderTargetFormat::kD24FS8);
  if (reconcile &&
      xenos::Float32To20e4(host_depth * 2.0f, (lane_mode & 4u) != 0) == d24) {
    return host_depth;
  }
  return xenos::Float20e4To32(d24) * 0.5f;
}

uint32_t MetalRenderTargetCache::HostLaneMode(bool materialize) const {
  if (!host_depth_gate_enabled_ || f24_ps_engaged_) {
    return 0;
  }
  uint32_t mode = materialize ? 2u : 1u;
  if (::rex::graphics::metal::GetMetalSettings().depth_float24_round.Get()) {
    mode |= 4u;
  }
  return mode;
}

void* MetalRenderTargetCache::EnsureHostLaneBuffer(void* device_handle) {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_handle;
  if (host_depth_gate_enabled_ && !f24_ps_engaged_) {
    if (!host_depth_lane_) {
      // Same index space as the canonical words: one float per word.
      id<MTLBuffer> lane =
          [device newBufferWithLength:canonical_edram_size()
                              options:MTLResourceStorageModePrivate];
      lane.label = @"rtcache:host-depth-lane";
      host_depth_lane_ = (__bridge_retained void*)lane;
    }
    return host_depth_lane_;
  }
  if (!host_lane_dummy_) {
    id<MTLBuffer> dummy =
        [device newBufferWithLength:16
                            options:MTLResourceStorageModePrivate];
    dummy.label = @"rtcache:host-depth-lane-dummy";
    host_lane_dummy_ = (__bridge_retained void*)dummy;
  }
  return host_lane_dummy_;
}

// Stage B lane self-check + restore measurement (SAMPLE_EDRAM_SELF_CHECK):
// blit the refreshed lane and the canonical words over the exact two-source
// intersections into a shared buffer, and analyze on the command buffer's
// completion handler with the PRODUCTION CPU oracle — counting host-wins,
// guest-wins, and PRECISION RESTORES (host won AND its float differs from the
// guest-decoded float). Zero EDRAM-word movement is only a regression result;
// these counts are the positive evidence promotion needs.
static void EncodeTwoSourceLaneDiagnostic(
    id<MTLCommandBuffer> command, id<MTLBuffer> canonical, id<MTLBuffer> lane,
    const std::vector<MetalTwoSourceRecord>& records, bool rne,
    uint32_t tile_bytes) {
  // 1280 words per tile at 1x, times the draw resolution scale.
  const uint32_t kTileBytes = tile_bytes;
  uint32_t total_bytes = 0;
  for (const MetalTwoSourceRecord& record : records) {
    total_bytes += (record.end_tiles - record.start_tiles) * kTileBytes;
  }
  if (!total_bytes) {
    return;
  }
  id<MTLBuffer> readback =
      [command.device newBufferWithLength:size_t(total_bytes) * 2
                                  options:MTLResourceStorageModeShared];
  if (!readback) {
    return;
  }
  id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
  blit.label = @"rtcache:host-lane-diagnostic";
  uint32_t cursor = 0;
  for (const MetalTwoSourceRecord& record : records) {
    uint32_t bytes = (record.end_tiles - record.start_tiles) * kTileBytes;
    [blit copyFromBuffer:canonical
             sourceOffset:size_t(record.start_tiles) * kTileBytes
                 toBuffer:readback
        destinationOffset:cursor
                     size:bytes];
    [blit copyFromBuffer:lane
             sourceOffset:size_t(record.start_tiles) * kTileBytes
                 toBuffer:readback
        destinationOffset:size_t(total_bytes) + cursor
                     size:bytes];
    cursor += bytes;
  }
  [blit endEncoding];
  std::vector<MetalTwoSourceRecord> records_copy = records;
  [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    if (completed.status != MTLCommandBufferStatusCompleted) {
      return;
    }
    const uint32_t* words = static_cast<const uint32_t*>(readback.contents);
    const float* lane_values = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(readback.contents) + total_bytes);
    uint32_t offset_words = 0;
    for (const MetalTwoSourceRecord& record : records_copy) {
      uint32_t count = (record.end_tiles - record.start_tiles) *
                       (kTileBytes / uint32_t(sizeof(uint32_t)));
      uint64_t host_wins = 0, guest_wins = 0, precision_restores = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t word = words[offset_words + i];
        float host = lane_values[offset_words + i];
        float reconciled = MetalHostDepthReconcile(word, host, rne);
        uint32_t reconciled_bits, host_bits;
        std::memcpy(&reconciled_bits, &reconciled, 4);
        std::memcpy(&host_bits, &host, 4);
        if (reconciled_bits == host_bits) {
          ++host_wins;
          float guest_decoded =
              xenos::Float20e4To32(word >> 8) * 0.5f;
          uint32_t guest_bits;
          std::memcpy(&guest_bits, &guest_decoded, 4);
          if (guest_bits != host_bits) {
            ++precision_restores;
          }
        } else {
          ++guest_wins;
        }
      }
      offset_words += count;
      REXLOG_INFO(
          "MetalHostDepthDiag tiles [{},{}): host-wins {} (precision "
          "restores {}), guest-wins {} of {}",
          record.start_tiles, record.end_tiles, host_wins, precision_restores,
          guest_wins, count);
    }
  }];
}

void MetalRenderTargetCache::NoteLaneOwner(const void* owner,
                                           uint32_t start_tiles,
                                           uint32_t end_tiles) {
  if (lane_owner_.size() != ::rex::xenos::kEDRAMTileCount) {
    lane_owner_.assign(::rex::xenos::kEDRAMTileCount, nullptr);
  }
  end_tiles = std::min(end_tiles, ::rex::xenos::kEDRAMTileCount);
  for (uint32_t t = start_tiles; t < end_tiles; ++t) {
    lane_owner_[t] = owner;
  }
}

bool MetalRenderTargetCache::LaneOwnedBy(const void* owner,
                                         uint32_t start_tiles,
                                         uint32_t end_tiles) const {
  static const bool s_always =
      std::getenv("XE_METAL_LANE_REFRESH_ALWAYS") != nullptr;
  if (s_always || !owner || lane_owner_.size() != ::rex::xenos::kEDRAMTileCount ||
      start_tiles >= end_tiles) {
    return false;
  }
  end_tiles = std::min(end_tiles, ::rex::xenos::kEDRAMTileCount);
  for (uint32_t t = start_tiles; t < end_tiles; ++t) {
    if (lane_owner_[t] != owner) {
      return false;
    }
  }
  return true;
}

bool MetalRenderTargetCache::EncodeLaneRefresh(
    const void* command_buffer, const MetalTwoSourceRecord& record) {
  if (!command_buffer || !record.host_source) {
    return false;
  }
  if (LaneOwnedBy(record.host_source, record.start_tiles,
                  record.end_tiles)) {
    // The lane still holds this source's floats over the whole range.
    return true;
  }
  auto* source = static_cast<MetalRenderTarget*>(
      static_cast<RenderTarget*>(const_cast<void*>(record.host_source)));
  const RenderTargetKey& key = source->GetKey();
  // Only a float24 depth source has host precision worth carrying.
  if (!key.is_depth ||
      key.GetDepthFormat() != xenos::DepthRenderTargetFormat::kD24FS8) {
    return false;
  }
  bool ms = key.msaa_samples != xenos::MSAASamples::k1X;
  id<MTLTexture> texture =
      (__bridge id<MTLTexture>)(ms ? source->msaa_texture()
                                   : source->texture());
  if (!texture) {
    return false;
  }
  const ShadowDepthPipelines* pipelines =
      EnsureShadowDepthPipelines(ms ? source->sample_count() : 1u);
  if (!pipelines || !pipelines->lane_store) {
    return false;
  }
  id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)command_buffer;
  EdramSurfaceParams params{key.base_tiles, key.GetPitchTiles(),
                            uint32_t(key.msaa_samples), 2u, 0u,
                            canonical_edram_size() /
                                uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{record.start_tiles, record.end_tiles};
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)pipelines->lane_store;
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  encoder.label = @"rtcache:host-lane-refresh";
  [encoder setComputePipelineState:pipeline];
  [encoder setTexture:texture atIndex:0];
  [encoder setBuffer:(__bridge id<MTLBuffer>)EnsureHostLaneBuffer(
                         (__bridge void*)command.device)
              offset:0
             atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  [encoder setBytes:&range length:sizeof(range) atIndex:2];
  // The kernel guards every thread by tile range; dispatch only the rows the
  // range can reach, as the pack and materialize kernels do.
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      key.base_tiles, key.GetPitchTiles(), uint32_t(key.msaa_samples),
      record.start_tiles, record.end_tiles, texture.height,
      GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const uint32_t row_start = uint32_t(GetCanonicalRangeTextureRowStart(
      key.base_tiles, key.GetPitchTiles(), uint32_t(key.msaa_samples),
      record.start_tiles, work_height, texture.height,
      GetDrawResolutionScaleY()));
  [encoder setBytes:&row_start length:sizeof(row_start) atIndex:7];
  [encoder dispatchThreads:MTLSizeMake(texture.width, work_height - row_start,
                                       ms ? texture.sampleCount : 1)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  [encoder endEncoding];
  NoteLaneOwner(record.host_source, record.start_tiles, record.end_tiles);
  return true;
}

void MetalRenderTargetCache::RecordTransition(TransferKind kind,
                                              const RenderTargetKey& src_key,
                                              const RenderTargetKey& dst_key,
                                              const Transfer& transfer) {
  TransitionRecord rec;
  rec.transition_id = transition_counter_++;
  rec.draw_index = command_processor_.trace_draw_index();
  rec.src_key = src_key.key;
  rec.dst_key = dst_key.key;
  rec.kind = kind;
  rec.start_tiles = transfer.start_tiles;
  rec.end_tiles = transfer.end_tiles;
  // Stage C lineage maintenance (key identities, sequence = transition_id):
  // a c2d lend records; every OTHER kind supersedes its overlap — EXCEPT
  // kDepthToColor, whose branch consumes-then-supersedes itself so this
  // (earlier) call cannot destroy the lineage it is about to classify.
  if (kind == TransferKind::kColorToDepth) {
    d2c_lineage_.RecordColorToDepth(src_key.key, dst_key.key,
                                    transfer.start_tiles, transfer.end_tiles,
                                    command_processor_.swap_count(),
                                    rec.transition_id);
  } else if (kind != TransferKind::kDepthToColor) {
    d2c_lineage_.SupersedeRange(transfer.start_tiles, transfer.end_tiles);
  }
  // Stage B 3.4: DIRECT measurement of the two host-depth transfer classes.
  // Generic color_to_depth counts do not prove kColorAndHostDepthToDepth —
  // only a transfer carrying host_depth_source does. Class is named by the
  // guest-word SOURCE type (the host-depth side is a depth RT by
  // construction). One-shot per class unconditionally (engagement evidence
  // must not depend on a diagnostic env), every occurrence in the manifest.
  const char* two_source_class = nullptr;
  if (transfer.host_depth_source) {
    two_source_class = src_key.is_depth ? "depth_and_host_depth_to_depth"
                                        : "color_and_host_depth_to_depth";
    two_source_records_.push_back({dst_key.key, transfer.host_depth_source,
                                   transfer.start_tiles, transfer.end_tiles});
  } else if (transfer.source &&
             MetalDepthToDepthCarriesHostPrecision(
                 host_depth_d2d_enabled_ && host_depth_gate_enabled_ &&
                     !f24_ps_engaged_,
                 /*has_planner_host_source=*/false, src_key.is_depth != 0,
                 dst_key.is_depth != 0, src_key.GetDepthFormat(),
                 dst_key.GetDepthFormat())) {
    // The source depth attachment still holds the host-precision floats the
    // canonical words were packed from (see the constructor note). A lane
    // refresh from it before the destination materialize lets the pointwise
    // predicate restore that precision exactly where the words are its
    // rounded shadow — and nowhere else.
    two_source_class = "depth_to_depth_host_precision";
    two_source_records_.push_back({dst_key.key, transfer.source,
                                   transfer.start_tiles, transfer.end_tiles});
  }
  static const bool s_manifest = std::getenv("XE_METAL_TRANSITION_MANIFEST");
  if (s_manifest && two_source_class) {
    REXLOG_WARN("MetalXferTwoSource[{}] @draw {}: {} -> {} tiles [{},{})",
           two_source_class, rec.draw_index, src_key.GetDebugName(),
           dst_key.GetDebugName(), rec.start_tiles, rec.end_tiles);
  }
  if (s_manifest) {
    // Warning level (not GPU/Debug): the sibling transfer logs use REXLOG_WARN and a
    // Release build filters Debug, so a Debug line here logs nothing on-device
    // (why the first M4 run showed zero MetalXfer lines). Env-gated, so this is
    // only noisy in an explicit diagnostic session.
    REXLOG_WARN("MetalXfer[{}] #{} @draw {}: {} -> {} tiles [{},{})",
           TransferKindName(kind), rec.transition_id, rec.draw_index,
           src_key.GetDebugName(), dst_key.GetDebugName(), rec.start_tiles,
           rec.end_tiles);
  }
}

// Writers of depth targets other than draws and materialization (transfers,
// resolve clears): every cached 2x/4x stencil may be stale after them.
void MetalRenderTargetCache::InvalidateMsaaStencilCache(
    void* msaa_texture, MsaaStencilInvalidation why) {
  if (!msaa_texture) {
    return;
  }
  auto it = msaa_stencil_caches_.find(msaa_texture);
  if (it != msaa_stencil_caches_.end() && it->second.valid) {
    it->second.valid = false;
    it->second.why = why;
  }
}

void MetalRenderTargetCache::InvalidateMsaaStencilCaches(
    MsaaStencilInvalidation why) {
  for (auto& pair : msaa_stencil_caches_) {
    if (pair.second.valid) {
      pair.second.valid = false;
      pair.second.why = why;
    }
  }
}

static bool MsaaStencilExtract(id<MTLDevice> device);

const MetalRenderTargetCache::ShadowDepthPipelines*
MetalRenderTargetCache::EnsureShadowDepthPipelines(uint32_t sample_count) {
  auto it = shadow_pipelines_.find(sample_count);
  if (it != shadow_pipelines_.end()) {
    return it->second.pack ? &it->second : nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!shadow_depth_library_) {
    NSError* err = nil;
    // Diagnostic legacy adapter for the LIVE half-res sc=1 store of 4x/2x depth:
    // it is NOT part of the canonical authority (kEdramMsl stays pristine),
    // but it shares the exact same addresser by being compiled in the SAME unit
    // (no duplication, no drift). pack_depth_cov is pack_depth plus a coverage
    // byte per written sample slot, so the round-trip can TRUTHFULLY separate
    // slots the collapsed store actually holds (known) from the samples
    // decimation discarded (missing) -- instead of conflating "missing" with a
    // zero depth value. For a 1x source every in-region slot is covered; for a
    // 4x collapsed source only 1 of every 4 (the sample-0 slot) is.
    static const char* kShadowAdapterMsl = R"MSL(
kernel void pack_depth_cov(depth2d<float, access::read> tex [[texture(0)]],
                           texture2d<uint, access::read> sten [[texture(1)]],
                           device uint* canonical [[buffer(0)]],
                           constant SurfaceParams& sp [[buffer(1)]],
                           device uchar* coverage [[buffer(2)]],
                           uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, 0u);
  uint idx = off >> 2;
  if (idx >= sp.words) return;
  float d = tex.read(gid);
  uint s = sten.read(gid).r & 0xFFu;
  canonical[idx] =
      xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);
  coverage[idx] = 1u;
}
// --- Canonical DEPTH authority kernels (Option 1). Same addresser/helpers as
// the pristine canonical string (compiled in this unit), plus a tile-range guard
// so a whole-texture pass touches ONLY the transfer's tiles -- the source pack
// refreshes just its owned tiles in canonical, and the dest materialize replaces
// just those tiles, leaving every other tile of the real RT intact.
struct RangeParams { uint start_tiles; uint end_tiles; };
// Tile index for a host pixel (mirrors the toff math in xe_edram_offset_bytes;
// fmt_log2 = 0 for 32bpp depth, so the tile grid is the 32bpp tile, 80x16
// samples times the draw resolution scale).
inline uint xe_edram_tile_of(uint px, uint py, uint base, uint pitch, uint msaa,
                             uint sample) {
  uint2 s = uint2(px, py) << xe_edram_msaa_shifts(msaa);
  s += (uint2(sample, sample) >> uint2(1u, 0u)) & 1u;
  return base + (s.y / XE_EDRAM_TILE_HEIGHT_SAMPLES) * pitch +
         (s.x / XE_EDRAM_TILE_WIDTH_SAMPLES);
}
kernel void pack_depth_range(depth2d<float, access::read> tex [[texture(0)]],
                             texture2d<uint, access::read> sten [[texture(1)]],
                             device uint* canonical [[buffer(0)]],
                             constant SurfaceParams& sp [[buffer(1)]],
                             constant RangeParams& rp [[buffer(2)]],
                             constant HostLaneParams& hl [[buffer(3)]],
                             device float* lane [[buffer(4)]],
                             constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint tile = xe_edram_tile_of(gid.x, gid.y, sp.base, sp.pitch, sp.msaa, 0u);
  if (tile < rp.start_tiles || tile >= rp.end_tiles) return;
  float d = tex.read(gid);
  uint s = sten.read(gid).r & 0xFFu;
  uint word =
      xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);
  // Missing-sample reconstruction: the live host store is DECIMATED to one
  // sample per pixel, so a host pixel of a 2x/4x view stands for ALL samples of
  // its group. Replicate the word across the group's canonical slots (nearest
  // upscale, matching the legacy resample) so a later 1x view reading full-res
  // canonical gets a COHERENT depth instead of 3/4 stale slots -> no decimation
  // streaks, correct depth-based shading. 1x (msaa 0) writes exactly one slot.
  uint nsamp = 1u << sp.msaa;
  for (uint sid = 0u; sid < nsamp; ++sid) {
    uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                     sp.is_depth, 0u, sid);
    uint idx = off >> 2;
    if (idx < sp.words) {
      canonical[idx] = word;
      // Stage B: host-precision lane beside every replicated slot, so a
      // later reconcile at ANY sample of the group can restore it.
      if ((hl.mode & 1u) && sp.is_depth == 2u) lane[idx] = d;
    }
  }
}
// Stage B provenance refresh (1x source): the SELECTED host source's depth
// into the lane over the exact intersection; canonical words untouched.
// Replicates across the sample group like pack_depth_range so a reconcile at
// any slot of the group reads the source's value.
kernel void lane_store_range(depth2d<float, access::read> tex [[texture(0)]],
                             device float* lane [[buffer(0)]],
                             constant SurfaceParams& sp [[buffer(1)]],
                             constant RangeParams& rp [[buffer(2)]],
                             constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (sp.is_depth != 2u) return;
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint tile = xe_edram_tile_of(gid.x, gid.y, sp.base, sp.pitch, sp.msaa, 0u);
  if (tile < rp.start_tiles || tile >= rp.end_tiles) return;
  float d = tex.read(gid);
  uint nsamp = 1u << sp.msaa;
  for (uint sid = 0u; sid < nsamp; ++sid) {
    uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                     sp.is_depth, 0u, sid);
    uint idx = off >> 2;
    if (idx < sp.words) lane[idx] = d;
  }
}
fragment DepthFragOut materialize_depth_range_fs(
    DepthVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant RangeParams& rp [[buffer(2)]],
    constant HostLaneParams& hl [[buffer(3)]],
    device const float* lane [[buffer(4)]]) {
  uint2 gid = uint2(in.pos.xy);
  uint tile = xe_edram_tile_of(gid.x, gid.y, sp.base, sp.pitch, sp.msaa, 0u);
  if (tile < rp.start_tiles || tile >= rp.end_tiles) discard_fragment();
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, 0u);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  DepthFragOut o;
  if ((hl.mode & 2u) && sp.is_depth == 2u && idx < sp.words) {
    o.depth = xe_host_depth_reconcile(w, lane[idx], (hl.mode >> 2u) & 1u);
  } else {
    o.depth = xe_depth_word_to_host(w, sp.is_depth);
  }
  return o;
}
fragment void materialize_sten_range_fs(DepthVOut in [[stage_in]],
                                        device const uint* canonical [[buffer(0)]],
                                        constant SurfaceParams& sp [[buffer(1)]],
                                        constant RangeParams& rp [[buffer(2)]],
                                        constant uint& bit [[buffer(3)]]) {
  uint2 gid = uint2(in.pos.xy);
  uint tile = xe_edram_tile_of(gid.x, gid.y, sp.base, sp.pitch, sp.msaa, 0u);
  if (tile < rp.start_tiles || tile >= rp.end_tiles) discard_fragment();
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, 0u);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  if ((xe_stencil(w) & bit) == 0u) discard_fragment();
}
// Single-pass 1x sibling of materialize_depth_sten_ms_range_fs.
fragment DepthStencilFragOut materialize_depth_sten_range_fs(
    DepthVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant RangeParams& rp [[buffer(2)]],
    constant HostLaneParams& hl [[buffer(3)]],
    device const float* lane [[buffer(4)]]) {
  uint2 gid = uint2(in.pos.xy);
  uint tile = xe_edram_tile_of(gid.x, gid.y, sp.base, sp.pitch, sp.msaa, 0u);
  if (tile < rp.start_tiles || tile >= rp.end_tiles) discard_fragment();
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, 0u);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  DepthStencilFragOut o;
  if ((hl.mode & 2u) && sp.is_depth == 2u && idx < sp.words) {
    o.depth = xe_host_depth_reconcile(w, lane[idx], (hl.mode >> 2u) & 1u);
  } else {
    o.depth = xe_depth_word_to_host(w, sp.is_depth);
  }
  o.stencil = xe_stencil(w);
  return o;
}
)MSL";
    // Only GPUs on the bit-plane pack compile this part; Apple GPUs never
    // build its pipelines.
    static const char* kShadowStencilExtractMsl = R"MSL(
// 2x/4x depth pack without the X32_Stencil8 view (MsaaStencilExtract): the
// target's stencil is cached per pixel (byte n = sample n) by one draw per
// stencil bit -- stencil EQUAL on that bit, early tests, at sample rate,
// ORing the bit into the pixel's word -- and the packs read it from there.
[[early_fragment_tests]] fragment void cache_stencil_bit_fs(
    DepthVOut in [[stage_in]],
    device atomic_uint* cache [[buffer(0)]],
    constant uint& width [[buffer(1)]],
    constant uint& bit [[buffer(3)]],
    uint sid [[sample_id]]) {
  uint2 px = uint2(in.pos.xy);
  atomic_fetch_or_explicit(&cache[px.y * width + px.x], bit << (8u * sid),
                           memory_order_relaxed);
}
// XE_METAL_MSAA_STENCIL_EXTRACT_VERIFY builds _verify variants that also
// compare each sample's cached byte with the X32_Stencil8 view (only where
// that read is safe): counts[0] compared, counts[1] differing.
#define XE_PACK_CACHED_STENCIL_KERNELS(SUFFIX, VERIFY_ARGS, VERIFY)           \
  kernel void pack_depth_ms_range_cached##SUFFIX(                             \
      depth2d_ms<float, access::read> tex [[texture(0)]],                     \
      device uint* canonical [[buffer(0)]],                                   \
      constant SurfaceParams& sp [[buffer(1)]],                               \
      constant AuthorityRangeParams& range [[buffer(2)]],                     \
      constant HostLaneParams& hl [[buffer(3)]],                              \
      device float* lane [[buffer(4)]],                                       \
      constant uint& row_start [[buffer(5)]],                                 \
      device const uint* cache [[buffer(7)]] VERIFY_ARGS,                     \
      uint3 tid [[thread_position_in_grid]]) {                                \
    uint3 gid = uint3(tid.x, tid.y + row_start, tid.z);                       \
    uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);             \
    if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||             \
        gid.z >= tex.get_num_samples() ||                                     \
        !xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range))        \
      return;                                                                 \
    uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch,         \
                                     sp.msaa, sp.is_depth, 0u,                \
                                     guest_sample) >> 2;                      \
    if (idx >= sp.words) return;                                              \
    uint2 px = uint2(gid.x, gid.y);                                           \
    uint ms = gid.z;                                                          \
    float d = tex.read(px, ms);                                               \
    uint s = (cache[px.y * tex.get_width() + px.x] >> (8u * ms)) & 0xFFu;     \
    VERIFY                                                                    \
    canonical[idx] =                                                          \
        xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);       \
    if ((hl.mode & 1u) && sp.is_depth == 2u) lane[idx] = d;                   \
  }                                                                           \
  kernel void pack_depth_ms_range_px_cached##SUFFIX(                          \
      depth2d_ms<float, access::read> tex [[texture(0)]],                     \
      device uint* canonical [[buffer(0)]],                                   \
      constant SurfaceParams& sp [[buffer(1)]],                               \
      constant AuthorityRangeParams& range [[buffer(2)]],                     \
      constant HostLaneParams& hl [[buffer(3)]],                              \
      device float* lane [[buffer(4)]],                                       \
      constant uint& row_start [[buffer(5)]],                                 \
      device const uint* cache [[buffer(7)]] VERIFY_ARGS,                     \
      uint2 tid [[thread_position_in_grid]]) {                                \
    uint2 px = uint2(tid.x, tid.y + row_start);                               \
    if (px.x >= tex.get_width() || px.y >= tex.get_height() ||               \
        !xe_authority_in_range(px.x, px.y, sp, 0u, range))                    \
      return;                                                                 \
    uint idx0 = xe_edram_offset_bytes(px.x, px.y, sp.base, sp.pitch,          \
                                      sp.msaa, sp.is_depth, 0u, 0u) >> 2;     \
    uint samples = tex.get_num_samples();                                     \
    uint cached = cache[px.y * tex.get_width() + px.x];                       \
    for (uint ms = 0u; ms < samples; ++ms) {                                  \
      uint idx = xe_pixel_sample_word(                                        \
          idx0, xe_metal_sample_to_guest(sp.msaa, ms), 0u);                   \
      if (idx >= sp.words) continue;                                          \
      float d = tex.read(px, ms);                                             \
      uint s = (cached >> (8u * ms)) & 0xFFu;                                 \
      VERIFY                                                                  \
      canonical[idx] =                                                        \
          xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);     \
      if ((hl.mode & 1u) && sp.is_depth == 2u) lane[idx] = d;                 \
    }                                                                         \
  }
#define XE_NO_VERIFY_ARGS
#define XE_NO_VERIFY
#define XE_VERIFY_ARGS                                                        \
  , texture2d_ms<uint, access::read> view [[texture(1)]],                     \
      device atomic_uint* counts [[buffer(6)]]
#define XE_VERIFY                                                             \
  atomic_fetch_add_explicit(&counts[0], 1u, memory_order_relaxed);            \
  if (s != (view.read(px, ms).r & 0xFFu))                                     \
    atomic_fetch_add_explicit(&counts[1], 1u, memory_order_relaxed);
XE_PACK_CACHED_STENCIL_KERNELS(, XE_NO_VERIFY_ARGS, XE_NO_VERIFY)
XE_PACK_CACHED_STENCIL_KERNELS(_verify, XE_VERIFY_ARGS, XE_VERIFY)
)MSL";
    std::string src = std::string(kEdramMsl) + kShadowAdapterMsl;
    if (MsaaStencilExtract(device)) {
      src += kShadowStencilExtractMsl;
    }
    id<MTLLibrary> lib = [device
        newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                     options:EdramMslCompileOptions(
                                 edram_tile_width_samples(),
                                 edram_tile_height_samples())
                       error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalShadow: kEdramMsl failed to compile: {}",
             err ? err.localizedDescription.UTF8String : "?");
      shadow_pipelines_[sample_count] = ShadowDepthPipelines{};  // poison
      return nullptr;
    }
    shadow_depth_library_ = (__bridge_retained void*)lib;
  }
  id<MTLLibrary> lib = (__bridge id<MTLLibrary>)shadow_depth_library_;
  bool ms = sample_count > 1;
  NSError* err = nil;
  MTLRenderPipelineDescriptor* rpd = [[MTLRenderPipelineDescriptor alloc] init];
  rpd.vertexFunction = [lib newFunctionWithName:@"xe_depth_vs"];
  rpd.fragmentFunction = [lib
      newFunctionWithName:(ms ? @"materialize_depth_ms_fs" : @"materialize_depth_fs")];
  rpd.depthAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
  rpd.stencilAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
  rpd.rasterSampleCount = sample_count;
  id<MTLRenderPipelineState> matd =
      [device newRenderPipelineStateWithDescriptor:rpd error:&err];
  rpd.fragmentFunction = [lib
      newFunctionWithName:(ms ? @"materialize_sten_ms_fs" : @"materialize_sten_fs")];
  id<MTLRenderPipelineState> mats =
      [device newRenderPipelineStateWithDescriptor:rpd error:&err];
  id<MTLComputePipelineState> pack = [device
      newComputePipelineStateWithFunction:[lib
          newFunctionWithName:(ms ? @"pack_depth_ms" : @"pack_depth")]
                                    error:&err];
  // Coverage-aware pack (sc=1 collapsed store only; the diagnostic legacy path).
  id<MTLComputePipelineState> pack_cov =
      ms ? nil
         : [device newComputePipelineStateWithFunction:
                       [lib newFunctionWithName:@"pack_depth_cov"]
                                                 error:&err];
  // Range-guarded canonical-authority pipelines (Option 1). sc=1 only: this path
  // renders single-sample and uses canonical EDRAM as the durable authority.
  id<MTLComputePipelineState> pack_range =
      ms ? nil
         : [device newComputePipelineStateWithFunction:
                       [lib newFunctionWithName:@"pack_depth_range"]
                                                 error:&err];
  id<MTLRenderPipelineState> mat_depth_range = nil;
  id<MTLRenderPipelineState> mat_sten_range = nil;
  id<MTLComputePipelineState> pack_ms_range = nil;
  id<MTLComputePipelineState> pack_ms_range_px = nil;
  id<MTLRenderPipelineState> mat_depth_ms_range = nil;
  id<MTLRenderPipelineState> mat_sten_ms_range = nil;
  if (!ms) {
    rpd.fragmentFunction = [lib newFunctionWithName:@"materialize_depth_range_fs"];
    mat_depth_range = [device newRenderPipelineStateWithDescriptor:rpd
                                                             error:&err];
    rpd.fragmentFunction = [lib newFunctionWithName:@"materialize_sten_range_fs"];
    mat_sten_range = [device newRenderPipelineStateWithDescriptor:rpd
                                                            error:&err];
  } else {
    pack_ms_range = [device
        newComputePipelineStateWithFunction:
            [lib newFunctionWithName:@"pack_depth_ms_range"]
                                      error:&err];
    pack_ms_range_px = [device
        newComputePipelineStateWithFunction:
            [lib newFunctionWithName:@"pack_depth_ms_range_px"]
                                      error:&err];
    rpd.fragmentFunction =
        [lib newFunctionWithName:@"materialize_depth_ms_range_fs"];
    mat_depth_ms_range =
        [device newRenderPipelineStateWithDescriptor:rpd error:&err];
    rpd.fragmentFunction =
        [lib newFunctionWithName:@"materialize_sten_ms_range_fs"];
    mat_sten_ms_range =
        [device newRenderPipelineStateWithDescriptor:rpd error:&err];
  }
  // Single-pass depth+stencil export materialize (warn-only: the encoders fall
  // back to the depth + bit-plane passes when absent).
  id<MTLRenderPipelineState> mat_depth_sten_range = nil;
  {
    NSError* export_err = nil;
    rpd.fragmentFunction = [lib
        newFunctionWithName:(ms ? @"materialize_depth_sten_ms_range_fs"
                                : @"materialize_depth_sten_range_fs")];
    mat_depth_sten_range =
        rpd.fragmentFunction
            ? [device newRenderPipelineStateWithDescriptor:rpd
                                                     error:&export_err]
            : nil;
    if (!mat_depth_sten_range) {
      REXLOG_WARN("MetalShadow: stencil-export materialize pipeline failed "
             "sc={}: {}",
             sample_count,
             export_err ? export_err.localizedDescription.UTF8String : "?");
    }
  }
  if (!matd || !mats || !pack ||
      (!ms &&
       (!pack_cov || !pack_range || !mat_depth_range || !mat_sten_range)) ||
      (ms && (!pack_ms_range || !pack_ms_range_px || !mat_depth_ms_range ||
              !mat_sten_ms_range))) {
    REXLOG_ERROR("MetalShadow: pipeline build failed sc={}: {}", sample_count,
           err ? err.localizedDescription.UTF8String : "?");
    shadow_pipelines_[sample_count] = ShadowDepthPipelines{};  // poison
    return nullptr;
  }
  // Stage B lane refresh (warn-only: reconciliation fails closed to the pure
  // materialize when the pipeline is absent).
  id<MTLComputePipelineState> lane_store = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:(ms ? @"lane_store_ms_range"
                                       : @"lane_store_range")]
                                    error:&err];
  if (!lane_store) {
    REXLOG_WARN("MetalShadow: lane_store pipeline build failed sc={}: {}",
           sample_count,
           err ? err.localizedDescription.UTF8String : "?");
  }
  // Stencil-view-free depth pack for MsaaStencilExtract (warn-only: the pack
  // falls back to the X32_Stencil8 view when absent).
  id<MTLRenderPipelineState> or_stencil = nil;
  id<MTLComputePipelineState> pack_cached = nil;
  id<MTLComputePipelineState> pack_cached_px = nil;
  if (ms && MsaaStencilExtract(device)) {
    NSError* extract_err = nil;
    MTLRenderPipelineDescriptor* epd =
        [[MTLRenderPipelineDescriptor alloc] init];
    epd.vertexFunction = [lib newFunctionWithName:@"xe_depth_vs"];
    epd.fragmentFunction = [lib newFunctionWithName:@"cache_stencil_bit_fs"];
    epd.depthAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
    epd.stencilAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
    epd.rasterSampleCount = sample_count;
    or_stencil = [device newRenderPipelineStateWithDescriptor:epd
                                                        error:&extract_err];
    const bool verify =
        std::getenv("XE_METAL_MSAA_STENCIL_EXTRACT_VERIFY") != nullptr;
    auto compute = [&](NSString* name) {
      return [device
          newComputePipelineStateWithFunction:[lib newFunctionWithName:name]
                                        error:&extract_err];
    };
    pack_cached = compute(verify ? @"pack_depth_ms_range_cached_verify"
                                 : @"pack_depth_ms_range_cached");
    pack_cached_px = compute(verify ? @"pack_depth_ms_range_px_cached_verify"
                                    : @"pack_depth_ms_range_px_cached");
    if (!or_stencil || !pack_cached || !pack_cached_px) {
      REXLOG_WARN("MetalShadow: MSAA stencil extraction pipelines failed sc={}: {}",
             sample_count,
             extract_err ? extract_err.localizedDescription.UTF8String : "?");
      or_stencil = nil;
      pack_cached = nil;
      pack_cached_px = nil;
    } else if (verify && !msaa_stencil_verify_buffer_) {
      id<MTLBuffer> counts =
          [device newBufferWithLength:2 * sizeof(uint32_t)
                              options:MTLResourceStorageModeShared];
      std::memset(counts.contents, 0, 2 * sizeof(uint32_t));
      msaa_stencil_verify_buffer_ = (__bridge_retained void*)counts;
    }
  }
  ShadowDepthPipelines p;
  p.sample_count = sample_count;

  if (or_stencil) {
    p.extract_sten = (__bridge_retained void*)or_stencil;
    p.pack_ms_range_cached = (__bridge_retained void*)pack_cached;
    p.pack_ms_range_px_cached = (__bridge_retained void*)pack_cached_px;
    for (uint32_t b = 0; b < 8; ++b) {
      MTLDepthStencilDescriptor* eds = [[MTLDepthStencilDescriptor alloc] init];
      eds.depthCompareFunction = MTLCompareFunctionAlways;
      eds.depthWriteEnabled = NO;
      MTLStencilDescriptor* es = [[MTLStencilDescriptor alloc] init];
      es.stencilCompareFunction = MTLCompareFunctionEqual;
      es.readMask = 1u << b;
      es.writeMask = 0;
      eds.frontFaceStencil = es;
      eds.backFaceStencil = es;
      p.extract_states[b] = (__bridge_retained void*)[device
          newDepthStencilStateWithDescriptor:eds];
    }
  }
  p.lane_store = (__bridge_retained void*)lane_store;
  p.materialize_depth = (__bridge_retained void*)matd;
  p.materialize_sten = (__bridge_retained void*)mats;
  p.pack = (__bridge_retained void*)pack;
  p.pack_cov = pack_cov ? (__bridge_retained void*)pack_cov : nullptr;
  p.pack_range = pack_range ? (__bridge_retained void*)pack_range : nullptr;
  p.mat_depth_range =
      mat_depth_range ? (__bridge_retained void*)mat_depth_range : nullptr;
  p.mat_depth_sten_range = mat_depth_sten_range
                               ? (__bridge_retained void*)mat_depth_sten_range
                               : nullptr;
  p.mat_sten_range =
      mat_sten_range ? (__bridge_retained void*)mat_sten_range : nullptr;
  p.pack_ms_range =
      pack_ms_range ? (__bridge_retained void*)pack_ms_range : nullptr;
  p.pack_ms_range_px =
      pack_ms_range_px ? (__bridge_retained void*)pack_ms_range_px : nullptr;
  p.mat_depth_ms_range = mat_depth_ms_range
                             ? (__bridge_retained void*)mat_depth_ms_range
                             : nullptr;
  p.mat_sten_ms_range = mat_sten_ms_range
                            ? (__bridge_retained void*)mat_sten_ms_range
                            : nullptr;
  // Depth pass writes depth AND REPLACEs stencil to 0 (stencilRef 0) so the
  // bit-plane passes fully replace stencil -- the partial-write lesson from the
  // conformance mixed-sequence test, kept here so the substrate behaves the same.
  MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
  dsd.depthCompareFunction = MTLCompareFunctionAlways;
  dsd.depthWriteEnabled = YES;
  MTLStencilDescriptor* zs = [[MTLStencilDescriptor alloc] init];
  zs.stencilCompareFunction = MTLCompareFunctionAlways;
  zs.depthStencilPassOperation = MTLStencilOperationReplace;
  zs.writeMask = 0xFF;
  dsd.frontFaceStencil = zs;
  dsd.backFaceStencil = zs;
  p.depth_state =
      (__bridge_retained void*)[device newDepthStencilStateWithDescriptor:dsd];
  for (uint32_t b = 0; b < 8; ++b) {
    MTLDepthStencilDescriptor* bds = [[MTLDepthStencilDescriptor alloc] init];
    bds.depthCompareFunction = MTLCompareFunctionAlways;
    bds.depthWriteEnabled = NO;
    MTLStencilDescriptor* bs = [[MTLStencilDescriptor alloc] init];
    bs.stencilCompareFunction = MTLCompareFunctionAlways;
    bs.depthStencilPassOperation = MTLStencilOperationReplace;
    bs.writeMask = 1u << b;
    bds.frontFaceStencil = bs;
    bds.backFaceStencil = bs;
    p.bit_states[b] =
        (__bridge_retained void*)[device newDepthStencilStateWithDescriptor:bds];
  }
  auto ins = shadow_pipelines_.emplace(sample_count, p);
  return &ins.first->second;
}

// 2x/4x packs to canonical EDRAM run one thread per pixel, packing all of its
// samples (2026-10-29; GTA IV at 2x2 on the M4 mini 19.0 -> 21.3 fps).
// Default on; XE_METAL_MSAA_PACK_PER_PIXEL=0 packs one thread per sample.
// 2x/4x depth packs take stencil from stencil-test bit planes instead of an
// X32_Stencil8 view on pre-Metal 3 GPUs: on the Radeon Pro 580X (Polaris)
// reading a multisampled stencil view (from compute or fragment shaders)
// intermittently wedges the GPU (~26 s command buffer ->
// kIOAccelCommandBufferCallbackErrorTimeout, then every frame; RDR ~90% of
// five-minute runs with texture heaps off). The bit planes need a render pass
// on the depth target, which costs like a decompression on AMD, so they fill
// a per-target stencil cache reused until the target is drawn to (USF4 packs
// ~220 times a frame, 96% without a draw in between): USF4 60 fps (60
// before), Gears 3 19.5 (20.4). The cache's rewrites wait on a fence the
// reading packs update: a reused intermediate was rewritten before an
// earlier pack read it on the RX 6900 XT.
// XE_METAL_MSAA_STENCIL_EXTRACT_VERIFY compares the packed stencil with the
// view where reading it is safe. XE_METAL_MSAA_STENCIL_EXTRACT=0/1 overrides.
// XE_METAL_MSAA_DEPTH_PACK=compute|bits picks the 2x/4x depth pack: the
// compute pack reading the X32_Stencil8 view, or the bit-plane stencil cache.
// (The view read in an attachment-less fragment pass wedged the 580X too.)
enum class MsaaDepthPackMode { kCompute, kBits };
static MsaaDepthPackMode GetMsaaDepthPackMode(id<MTLDevice> device) {
  static const MsaaDepthPackMode mode = [device] {
    if (const char* v = std::getenv("XE_METAL_MSAA_DEPTH_PACK")) {
      return !std::strcmp(v, "bits") ? MsaaDepthPackMode::kBits
                                     : MsaaDepthPackMode::kCompute;
    }
    if (const char* v = std::getenv("XE_METAL_MSAA_STENCIL_EXTRACT")) {
      return v[0] == '1' ? MsaaDepthPackMode::kBits
                         : MsaaDepthPackMode::kCompute;
    }
    bool metal3 = false;
    if (@available(macOS 13.0, *)) {
      metal3 = [device supportsFamily:MTLGPUFamilyMetal3];
    }
    return !metal3 && ![device supportsFamily:MTLGPUFamilyApple7]
               ? MsaaDepthPackMode::kBits
               : MsaaDepthPackMode::kCompute;
  }();
  return mode;
}
static bool MsaaStencilExtract(id<MTLDevice> device) {
  return GetMsaaDepthPackMode(device) == MsaaDepthPackMode::kBits;
}
static bool MsaaPackPerPixel() {
  static const bool enabled = ParseMetalEnvironmentFlag(
      std::getenv("XE_METAL_MSAA_PACK_PER_PIXEL"), true);
  return enabled;
}

// Depth materialize writes the stencil byte from the fragment shader
// ([[stencil]] export) in the depth pass instead of eight bit-plane passes.
// XE_METAL_STENCIL_EXPORT=0 is the exact rollback to the bit-plane passes.
static bool CanonicalStencilExportEnabled() {
  static const bool enabled = MetalSwitchEnabled(
      ::rex::graphics::metal::GetMetalSettings().metal_stencil_export.Get(), std::getenv("XE_METAL_STENCIL_EXPORT"));
  return enabled;
}

bool MetalRenderTargetCache::EncodeCanonicalDepthMaterialize(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || !render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples != xenos::MSAASamples::k1X) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  id<MTLTexture> texture = (__bridge id<MTLTexture>)target->texture();
  const ShadowDepthPipelines* pipelines = EnsureShadowDepthPipelines(1u);
  if (!texture || !pipelines || !pipelines->mat_depth_range ||
      !pipelines->mat_sten_range) {
    return false;
  }
  uint32_t depth_format =
      target->GetKey().GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8
          ? 2u
          : 1u;
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), depth_format, 0u,
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  // Stage B provenance join: dest-keyed, intersection-clipped selection, and
  // a lane refresh from each SELECTED source encoded BEFORE the render pass
  // (compute cannot interleave with it). A failed refresh drops its record —
  // reconciliation fails closed to the pure materialize.
  std::vector<MetalTwoSourceRecord> two_source =
      MetalSelectTwoSourceIntersections(two_source_records_,
                                        target->GetKey().key, start_tiles,
                                        end_tiles);
  if (HostLaneMode(/*materialize=*/true) == 0u) {
    two_source.clear();
  }
  if (MetalDropOverlappingTwoSourceRecords(&two_source)) {
    // The shared planner walks disjoint ownership ranges per destination, so
    // this is an invariant violation, not expected traffic. The overlapping
    // later records were dropped (their tiles materialize pure guest words).
    REXLOG_ERROR(
        "MetalRenderTargetCache: overlapping same-destination two-source "
        "records dropped (planner disjointness invariant violated)");
    assert_always();
  }
  for (size_t i = 0; i < two_source.size();) {
    if (EncodeLaneRefresh(command_buffer, two_source[i])) {
      ++i;
    } else {
      two_source.erase(two_source.begin() + i);
    }
  }
  if (!two_source.empty() && canonical_restore_self_check_enabled_ &&
      host_depth_lane_) {
    EncodeTwoSourceLaneDiagnostic(
        command, canonical, (__bridge id<MTLBuffer>)host_depth_lane_,
        two_source, ::rex::graphics::metal::GetMetalSettings().depth_float24_round.Get(),
        5120u * GetDrawResolutionScaleX() * GetDrawResolutionScaleY());
  }
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.depthAttachment.texture = texture;
  pass.depthAttachment.loadAction = MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.texture = texture;
  pass.stencilAttachment.loadAction = MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  encoder.label = @"rtcache:canonical-depth-materialize-range";
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const NSUInteger row_start = GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY());
  [encoder setScissorRect:MTLScissorRect{0, row_start, texture.width,
                                         work_height - row_start}];
  const bool stencil_export =
      CanonicalStencilExportEnabled() && pipelines->mat_depth_sten_range;
  [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)(
                                      stencil_export
                                          ? pipelines->mat_depth_sten_range
                                          : pipelines->mat_depth_range)];
  [encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                    pipelines->depth_state];
  [encoder setStencilReferenceValue:0];
  [encoder setFragmentBuffer:canonical offset:0 atIndex:0];
  [encoder setFragmentBytes:&params length:sizeof(params) atIndex:1];
  [encoder setFragmentBytes:&range length:sizeof(range) atIndex:2];
  {
    // Base materialize is ALWAYS the pure guest-word path; reconciliation is
    // applied only by the per-intersection draws below.
    uint32_t hl_mode = 0u;
    [encoder setFragmentBytes:&hl_mode length:sizeof(hl_mode) atIndex:3];
    [encoder setFragmentBuffer:(__bridge id<MTLBuffer>)EnsureHostLaneBuffer(
                                   (__bridge void*)texture.device)
                        offset:0
                       atIndex:4];
  }
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  // Per-intersection reconcile draws: rp narrowed to the EXACT two-source
  // intersection (the fs discards outside rp), lane freshly holding the
  // selected source's provenance. Overwrites the base draw's depth there.
  for (const MetalTwoSourceRecord& ts : two_source) {
    RangeParams ts_range{ts.start_tiles, ts.end_tiles};
    uint32_t ts_mode = HostLaneMode(/*materialize=*/true);
    [encoder setFragmentBytes:&ts_range length:sizeof(ts_range) atIndex:2];
    [encoder setFragmentBytes:&ts_mode length:sizeof(ts_mode) atIndex:3];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
  }
  if (stencil_export) {
    [encoder endEncoding];
    return true;
  }
  if (!two_source.empty()) {
    // Restore the full-range rp for the stencil bit-plane passes.
    [encoder setFragmentBytes:&range length:sizeof(range) atIndex:2];
  }
  [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                      pipelines->mat_sten_range];
  [encoder setStencilReferenceValue:0xFF];
  for (uint32_t bit_index = 0; bit_index < 8u; ++bit_index) {
    uint32_t bit = 1u << bit_index;
    [encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                      pipelines->bit_states[bit_index]];
    [encoder setFragmentBytes:&bit length:sizeof(bit) atIndex:3];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:0
                 vertexCount:3];
  }
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::EncodeCanonicalDepthPack(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || !render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples != xenos::MSAASamples::k1X) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  id<MTLTexture> texture = (__bridge id<MTLTexture>)target->texture();
  const ShadowDepthPipelines* pipelines = EnsureShadowDepthPipelines(1u);
  if (!texture || !pipelines || !pipelines->pack_range) {
    return false;
  }
  id<MTLTexture> stencil =
      [texture newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
  if (!stencil) {
    return false;
  }
  uint32_t depth_format =
      target->GetKey().GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8
          ? 2u
          : 1u;
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), depth_format, 0u,
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)pipelines->pack_range;
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  encoder.label = @"rtcache:canonical-depth-pack-range";
  [encoder setComputePipelineState:pipeline];
  [encoder setTexture:texture atIndex:0];
  [encoder setTexture:stencil atIndex:1];
  [encoder setBuffer:canonical offset:0 atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  [encoder setBytes:&range length:sizeof(range) atIndex:2];
  {
    uint32_t hl_mode = HostLaneMode(/*materialize=*/false);
    [encoder setBytes:&hl_mode length:sizeof(hl_mode) atIndex:3];
    [encoder setBuffer:(__bridge id<MTLBuffer>)EnsureHostLaneBuffer(
                           (__bridge void*)command.device)
                offset:0
               atIndex:4];
  }
  NSUInteger thread_width = pipeline.threadExecutionWidth;
  NSUInteger thread_height =
      pipeline.maxTotalThreadsPerThreadgroup / thread_width;
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const uint32_t row_start = uint32_t(GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY()));
  [encoder setBytes:&row_start length:sizeof(row_start) atIndex:7];
  [encoder dispatchThreads:MTLSizeMake(texture.width, work_height - row_start, 1)
      threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
  [encoder endEncoding];
  if ((HostLaneMode(/*materialize=*/false) & 1u) && depth_format == 2u) {
    NoteLaneOwner(target, start_tiles, end_tiles);
  }
  return true;
}

bool MetalRenderTargetCache::EncodeCanonicalDepthMsaaMaterialize(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  ++msaa_stencil_census_[4];
  if (render_target) {
    auto* materialized = static_cast<MetalRenderTarget*>(render_target);
    InvalidateMsaaStencilCache(materialized->msaa_texture(),
                               kMsaaStencilMaterialize);
  }
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || !render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples == xenos::MSAASamples::k1X) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  id<MTLTexture> texture =
      (__bridge id<MTLTexture>)target->msaa_texture();
  if (!texture || (target->sample_count() != 2u &&
                   target->sample_count() != 4u)) {
    return false;
  }
  const ShadowDepthPipelines* pipelines =
      EnsureShadowDepthPipelines(target->sample_count());
  if (!pipelines) {
    return false;
  }
  uint32_t depth_format =
      target->GetKey().GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8
          ? 2u
          : 1u;
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), depth_format, 0u,
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  // Stage B provenance join (see the 1x sibling for the contract).
  std::vector<MetalTwoSourceRecord> two_source =
      MetalSelectTwoSourceIntersections(two_source_records_,
                                        target->GetKey().key, start_tiles,
                                        end_tiles);
  if (HostLaneMode(/*materialize=*/true) == 0u) {
    two_source.clear();
  }
  if (MetalDropOverlappingTwoSourceRecords(&two_source)) {
    // The shared planner walks disjoint ownership ranges per destination, so
    // this is an invariant violation, not expected traffic. The overlapping
    // later records were dropped (their tiles materialize pure guest words).
    REXLOG_ERROR(
        "MetalRenderTargetCache: overlapping same-destination two-source "
        "records dropped (planner disjointness invariant violated)");
    assert_always();
  }
  for (size_t i = 0; i < two_source.size();) {
    if (EncodeLaneRefresh(command_buffer, two_source[i])) {
      ++i;
    } else {
      two_source.erase(two_source.begin() + i);
    }
  }
  if (!two_source.empty() && canonical_restore_self_check_enabled_ &&
      host_depth_lane_) {
    EncodeTwoSourceLaneDiagnostic(
        command, canonical, (__bridge id<MTLBuffer>)host_depth_lane_,
        two_source, ::rex::graphics::metal::GetMetalSettings().depth_float24_round.Get(),
        5120u * GetDrawResolutionScaleX() * GetDrawResolutionScaleY());
  }
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.depthAttachment.texture = texture;
  pass.depthAttachment.loadAction = MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.texture = texture;
  pass.stencilAttachment.loadAction = MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  encoder.label = @"rtcache:canonical-depth-materialize-msaa-range";
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const NSUInteger row_start = GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY());
  [encoder setScissorRect:MTLScissorRect{0, row_start, texture.width,
                                         work_height - row_start}];

  // With stencil export the first pass writes every depth sample and its
  // complete stencil byte. Otherwise it replaces stencil with zero and eight
  // masked REPLACE passes reconstruct the byte per sample.
  const bool stencil_export =
      CanonicalStencilExportEnabled() && pipelines->mat_depth_sten_range;
  [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)(
                                      stencil_export
                                          ? pipelines->mat_depth_sten_range
                                          : pipelines->mat_depth_ms_range)];
  [encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                    pipelines->depth_state];
  [encoder setStencilReferenceValue:0];
  [encoder setFragmentBuffer:canonical offset:0 atIndex:0];
  [encoder setFragmentBytes:&params length:sizeof(params) atIndex:1];
  [encoder setFragmentBytes:&range length:sizeof(range) atIndex:2];
  {
    uint32_t hl_mode = 0u;
    [encoder setFragmentBytes:&hl_mode length:sizeof(hl_mode) atIndex:3];
    [encoder setFragmentBuffer:(__bridge id<MTLBuffer>)EnsureHostLaneBuffer(
                                   (__bridge void*)texture.device)
                        offset:0
                       atIndex:4];
  }
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  for (const MetalTwoSourceRecord& ts : two_source) {
    RangeParams ts_range{ts.start_tiles, ts.end_tiles};
    uint32_t ts_mode = HostLaneMode(/*materialize=*/true);
    [encoder setFragmentBytes:&ts_range length:sizeof(ts_range) atIndex:2];
    [encoder setFragmentBytes:&ts_mode length:sizeof(ts_mode) atIndex:3];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
  }
  if (stencil_export) {
    [encoder endEncoding];
    return true;
  }
  if (!two_source.empty()) {
    [encoder setFragmentBytes:&range length:sizeof(range) atIndex:2];
  }
  [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                      pipelines->mat_sten_ms_range];
  [encoder setStencilReferenceValue:0xFF];
  for (uint32_t bit_index = 0; bit_index < 8u; ++bit_index) {
    uint32_t bit = 1u << bit_index;
    [encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                      pipelines->bit_states[bit_index]];
    [encoder setFragmentBytes:&bit length:sizeof(bit) atIndex:3];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:0
                 vertexCount:3];
  }
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::EncodeCanonicalDepthMsaaPack(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  ++msaa_stencil_census_[3];
  static const bool s_census =
      std::getenv("XE_METAL_MSAA_STENCIL_CENSUS") != nullptr;
  if (s_census && render_target) {
    const uint32_t key = render_target->GetKey().key;
    const uint64_t last_draw = msaa_census_last_draw_[key];
    auto& last = msaa_census_last_pack_[key];
    if (last.serial && last.serial >= last_draw) {
      ++msaa_stencil_census_[6];
      if (start_tiles >= last.start && end_tiles <= last.end) {
        ++msaa_stencil_census_[5];
      }
    }
    if (last.serial >= last_draw && last.serial) {
      last.start = std::min(last.start, start_tiles);
      last.end = std::max(last.end, end_tiles);
    } else {
      last.start = start_tiles;
      last.end = end_tiles;
    }
    last.serial = msaa_stencil_census_[0] ? msaa_stencil_census_[0] : 1;
  }
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || !render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples == xenos::MSAASamples::k1X) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  id<MTLTexture> texture =
      (__bridge id<MTLTexture>)target->msaa_texture();
  if (!texture || (target->sample_count() != 2u &&
                   target->sample_count() != 4u)) {
    return false;
  }
  const ShadowDepthPipelines* pipelines =
      EnsureShadowDepthPipelines(target->sample_count());
  if (!pipelines) {
    return false;
  }
  uint32_t depth_format =
      target->GetKey().GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8
          ? 2u
          : 1u;
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), depth_format, 0u,
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    return false;
  }
  const uint32_t row_start = uint32_t(GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY()));
  if (row_start >= work_height) {
    return true;
  }
  // Devices where reading a 2x/4x stencil view can wedge the GPU
  // (MsaaStencilExtract) read stencil from the target's bit-plane cache.
  const bool extract =
      MsaaStencilExtract(texture.device) && pipelines->extract_sten;
  id<MTLTexture> stencil = nil;
  id<MTLBuffer> stencil_cache = nil;
  id<MTLFence> stencil_cache_fence = nil;
  if (extract) {
    // The per-pixel stencil cache: rebuilt by the bit planes after the target
    // was drawn to or materialized, then read by every pack until then (USF4
    // packs a target ~220 times a frame, 96% with no draw in between, and the
    // bit-plane pass costs a render pass on the 2x/4x depth target).
    MsaaStencilCache& cache = msaa_stencil_caches_[(__bridge void*)texture];
    const uint64_t cache_bytes =
        uint64_t(texture.width) * texture.height * sizeof(uint32_t);
    if (!cache.buffer || cache.bytes < cache_bytes) {
      if (cache.buffer) {
        deferred_texture_releases_.emplace_back(
            command_processor_.CurrentEncodingSubmission(), cache.buffer);
      }
      id<MTLBuffer> created =
          [texture.device newBufferWithLength:cache_bytes
                                      options:MTLResourceStorageModePrivate];
      if (!created) {
        return false;
      }
      created.label = @"rtcache:msaa-stencil-cache";
      cache.buffer = (__bridge_retained void*)created;
      cache.bytes = cache_bytes;
      cache.valid = false;
      cache.why = kMsaaStencilNew;
      if (!cache.fence) {
        cache.fence = (__bridge_retained void*)[texture.device newFence];
      }
    }
    stencil_cache = (__bridge id<MTLBuffer>)cache.buffer;
    stencil_cache_fence = (__bridge id<MTLFence>)cache.fence;
    // A stale cache rebuilds the pack's rows; a valid one missing them (more
    // packs with no draw in between) rebuilds the whole target for the rest.
    // XE_METAL_MSAA_STENCIL_CACHE=0: rebuild for every pack.
    static const bool s_cache_off = [] {
      const char* v = std::getenv("XE_METAL_MSAA_STENCIL_CACHE");
      return v && v[0] == '0';
    }();
    if (s_cache_off) {
      cache.valid = false;
      cache.why = kMsaaStencilOff;
    }
    uint32_t rows_start = row_start;
    uint32_t rows_end = uint32_t(work_height);
    if (cache.valid &&
        (rows_start < cache.valid_start || rows_end > cache.valid_end)) {
      rows_start = 0;
      rows_end = uint32_t(texture.height);
    }
    if (!cache.valid || rows_start < cache.valid_start ||
        rows_end > cache.valid_end) {
      ++msaa_stencil_cache_rebuilds_;
      ++msaa_stencil_rebuild_why_[cache.valid ? kMsaaStencilRows : cache.why];
      // Earlier packs read the cache; Metal did not order a later write
      // after such reads on the RX 6900 XT, so the clear waits on the fence
      // they update.
      id<MTLBlitCommandEncoder> clear = [command blitCommandEncoder];
      clear.label = @"rtcache:msaa-stencil-cache-clear";
      [clear waitForFence:stencil_cache_fence];
      const NSUInteger row_bytes = texture.width * sizeof(uint32_t);
      [clear fillBuffer:stencil_cache
                  range:NSMakeRange(rows_start * row_bytes,
                                    (rows_end - rows_start) * row_bytes)
                  value:0];
      [clear endEncoding];
      MTLRenderPassDescriptor* pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.depthAttachment.texture = texture;
      pass.depthAttachment.loadAction = MTLLoadActionLoad;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.stencilAttachment.texture = texture;
      pass.stencilAttachment.loadAction = MTLLoadActionLoad;
      pass.stencilAttachment.storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> bits = [command
          renderCommandEncoderWithDescriptor:
              metal::MetalApplyXenosSamplePositions(pass)];
      bits.label = @"rtcache:msaa-stencil-cache-bits";
      [bits setScissorRect:MTLScissorRect{0, rows_start, texture.width,
                                          rows_end - rows_start}];
      [bits setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                       pipelines->extract_sten];
      [bits setStencilReferenceValue:0xFF];
      [bits setFragmentBuffer:stencil_cache offset:0 atIndex:0];
      const uint32_t width = uint32_t(texture.width);
      [bits setFragmentBytes:&width length:sizeof(width) atIndex:1];
      for (uint32_t b = 0; b < 8; ++b) {
        const uint32_t bit = 1u << b;
        [bits setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                       pipelines->extract_states[b]];
        [bits setFragmentBytes:&bit length:sizeof(bit) atIndex:3];
        [bits drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:0
                 vertexCount:3];
      }
      [bits endEncoding];
      // Rows rebuilt now; still-valid rows they touch stay valid with them.
      if (cache.valid && rows_end >= cache.valid_start &&
          rows_start <= cache.valid_end) {
        cache.valid_start = std::min(cache.valid_start, rows_start);
        cache.valid_end = std::max(cache.valid_end, rows_end);
      } else {
        cache.valid_start = rows_start;
        cache.valid_end = rows_end;
      }
      cache.valid = true;
    } else {
      ++msaa_stencil_cache_hits_;
    }
    if (msaa_stencil_verify_buffer_) {
      stencil =
          [texture newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    }
  } else {
    stencil =
        [texture newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    if (!stencil) {
      return false;
    }
  }
  const bool per_pixel = MsaaPackPerPixel();
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(
          extract ? (per_pixel ? pipelines->pack_ms_range_px_cached
                               : pipelines->pack_ms_range_cached)
                  : (per_pixel ? pipelines->pack_ms_range_px
                               : pipelines->pack_ms_range));
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  encoder.label = @"rtcache:canonical-depth-pack-msaa-range";
  [encoder setComputePipelineState:pipeline];
  [encoder setTexture:texture atIndex:0];
  if (stencil) {
    [encoder setTexture:stencil atIndex:1];
  }
  [encoder setBuffer:canonical offset:0 atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  [encoder setBytes:&range length:sizeof(range) atIndex:2];
  {
    uint32_t hl_mode = HostLaneMode(/*materialize=*/false);
    [encoder setBytes:&hl_mode length:sizeof(hl_mode) atIndex:3];
    [encoder setBuffer:(__bridge id<MTLBuffer>)EnsureHostLaneBuffer(
                           (__bridge void*)command.device)
                offset:0
               atIndex:4];
  }
  if (extract) {
    [encoder setBuffer:stencil_cache offset:0 atIndex:7];
    if (msaa_stencil_verify_buffer_) {
      [encoder setBuffer:(__bridge id<MTLBuffer>)msaa_stencil_verify_buffer_
                  offset:0
                 atIndex:6];
    }
  }
  NSUInteger thread_width = pipeline.threadExecutionWidth;
  NSUInteger thread_height =
      pipeline.maxTotalThreadsPerThreadgroup / thread_width;
  [encoder setBytes:&row_start length:sizeof(row_start) atIndex:5];
  [encoder dispatchThreads:MTLSizeMake(texture.width, work_height - row_start,
                                       per_pixel ? 1 : texture.sampleCount)
      threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
  if (stencil_cache_fence) {
    [encoder updateFence:stencil_cache_fence];
  }
  [encoder endEncoding];
  if (extract && (++msaa_stencil_cache_packs_ & 0xFFFFu) == 0) {
    static const char* const kWhy[kMsaaStencilInvalidationCount] = {
        "new", "draw", "rows", "materialize", "replace",
        "clear-load", "shadow-cross", "depth-transfer", "cross-msaa",
        "color-to-depth", "resolve-clear", "off"};
    std::string why;
    for (uint32_t i = 0; i < kMsaaStencilInvalidationCount; ++i) {
      if (msaa_stencil_rebuild_why_[i]) {
        why += fmt::format(" {}={}", kWhy[i], msaa_stencil_rebuild_why_[i]);
      }
    }
    REXLOG_INFO("MetalMsaaStencilCache: {} packs, {} rebuilds, {} hits; rebuilt "
           "after:{}",
           msaa_stencil_cache_packs_, msaa_stencil_cache_rebuilds_,
           msaa_stencil_cache_hits_, why);
  }
  if (extract && msaa_stencil_verify_buffer_ &&
      (++msaa_stencil_verify_packs_ & 1023u) == 0) {
    const uint32_t* counts = static_cast<const uint32_t*>(
        [(__bridge id<MTLBuffer>)msaa_stencil_verify_buffer_ contents]);
    REXLOG_INFO("MetalMsaaStencilVerify: {} packs, {} samples compared, {} packed "
           "stencil bytes differ from the view",
           msaa_stencil_verify_packs_, counts[0], counts[1]);
  }
  if ((HostLaneMode(/*materialize=*/false) & 1u) && depth_format == 2u) {
    NoteLaneOwner(target, start_tiles, end_tiles);
  }
  return true;
}

void MetalRenderTargetCache::ShadowCrossMsaaDepth(const void* command_buffer,
                                                  RenderTarget* source_rt,
                                                  const Transfer& transfer) {
  InvalidateMsaaStencilCaches(kMsaaStencilShadowCross);
  // Cap the sampled transitions: the shadow adds real GPU work per transition,
  // and a sample is enough to prove the substrate on live data.
  if (shadow_count_ >= 128u) {
    return;
  }
  auto* source = static_cast<MetalRenderTarget*>(source_rt);
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> src_tex = (__bridge id<MTLTexture>)source->texture();
  if (!cb || !src_tex) {
    return;
  }
  uint32_t sc = uint32_t(src_tex.sampleCount);
  const ShadowDepthPipelines* p = EnsureShadowDepthPipelines(sc);
  if (!p) {
    return;
  }
  ++shadow_count_;
  // Fresh per-transition buffers (Shared so the completion handler can read
  // them; retained by the handler block, freed after it runs) -- no reuse, so
  // no race even if several transitions are shadowed in one frame.
  const NSUInteger kBytes = 10u * 1024u * 1024u;
  const NSUInteger kWords = kBytes / 4u;
  id<MTLBuffer> canon_a = [device newBufferWithLength:kBytes
                                             options:MTLResourceStorageModeShared];
  id<MTLBuffer> canon_b = [device newBufferWithLength:kBytes
                                             options:MTLResourceStorageModeShared];
  // Coverage: one byte per canonical word slot, marked by the diagnostic legacy
  // adapter pack for each slot the collapsed sc=1 store actually holds. Slots
  // left 0 in the transfer's tile range are samples decimation discarded
  // (MISSING) -- distinct from a slot that holds a genuine zero-depth word.
  id<MTLBuffer> cov_a = [device newBufferWithLength:kWords
                                           options:MTLResourceStorageModeShared];
  std::memset(canon_a.contents, 0, kBytes);
  std::memset(canon_b.contents, 0, kBytes);
  std::memset(cov_a.contents, 0, kWords);
  id<MTLTexture> src_sten =
      [src_tex newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
  MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
  td.textureType = sc > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
  td.pixelFormat = (MTLPixelFormat)kDepthFormat;
  td.width = src_tex.width;
  td.height = src_tex.height;
  td.sampleCount = sc;
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
             MTLTextureUsagePixelFormatView;
  td.storageMode = MTLStorageModePrivate;
  id<MTLTexture> scratch = [device newTextureWithDescriptor:td];
  id<MTLTexture> scratch_sten =
      [scratch newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
  bool f24 = source->GetKey().GetDepthFormat() ==
             xenos::DepthRenderTargetFormat::kD24FS8;
  EdramSurfaceParams sp{source->GetKey().base_tiles,
                        source->GetKey().GetPitchTiles(),
                        uint32_t(source->GetKey().msaa_samples),
                        f24 ? 2u : 1u, 0u, uint32_t(kBytes / 4)};
  id<MTLBuffer> spb = [device newBufferWithBytes:&sp
                                          length:sizeof(sp)
                                         options:MTLResourceStorageModeShared];
  uint32_t w = uint32_t(src_tex.width), h = uint32_t(src_tex.height);
  id<MTLComputePipelineState> pack_pso =
      (__bridge id<MTLComputePipelineState>)p->pack;
  id<MTLComputePipelineState> pack_cov_pso =
      (__bridge id<MTLComputePipelineState>)p->pack_cov;
  // All shadow work is encoded on the FRAME cb, AFTER the source's draws and the
  // legacy resample above -- so it reads correct, hazard-tracked source content
  // and never modifies a real render target (canon/scratch are private to this).
  // `cov` non-null -> use the coverage-aware adapter (source pack); null -> the
  // canonical pack (scratch re-pack, no coverage needed).
  auto pack = [&](id<MTLTexture> att, id<MTLTexture> sten, id<MTLBuffer> out,
                  id<MTLBuffer> cov, NSString* label) {
    bool use_cov = cov && pack_cov_pso;
    id<MTLComputePipelineState> pso = use_cov ? pack_cov_pso : pack_pso;
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    e.label = label;
    [e setComputePipelineState:pso];
    [e setTexture:att atIndex:0];
    [e setTexture:sten atIndex:1];
    [e setBuffer:out offset:0 atIndex:0];
    [e setBuffer:spb offset:0 atIndex:1];
    if (use_cov) {
      [e setBuffer:cov offset:0 atIndex:2];
    }
    NSUInteger tw = pso.threadExecutionWidth;
    NSUInteger th = pso.maxTotalThreadsPerThreadgroup / tw;
    [e dispatchThreads:MTLSizeMake(w, h, sc)
        threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [e endEncoding];
  };
  auto materialize = [&](id<MTLTexture> att, id<MTLBuffer> src,
                         NSString* label) {
    MTLRenderPassDescriptor* rp =
        [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = att;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 0.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.texture = att;
    rp.stencilAttachment.loadAction = MTLLoadActionClear;
    rp.stencilAttachment.clearStencil = 0;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e =
        [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(rp)];
    e.label = label;
    [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                  p->materialize_depth];
    [e setDepthStencilState:(__bridge id<MTLDepthStencilState>)p->depth_state];
    [e setStencilReferenceValue:0];
    [e setFragmentBuffer:src offset:0 atIndex:0];
    [e setFragmentBuffer:spb offset:0 atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                  p->materialize_sten];
    [e setStencilReferenceValue:0xFF];
    [e setFragmentBuffer:src offset:0 atIndex:0];
    [e setFragmentBuffer:spb offset:0 atIndex:1];
    for (uint32_t b = 0; b < 8; ++b) {
      uint32_t bit = 1u << b;
      [e setDepthStencilState:(__bridge id<MTLDepthStencilState>)p->bit_states[b]];
      [e setFragmentBytes:&bit length:sizeof(bit) atIndex:2];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [e endEncoding];
  };
  pack(src_tex, src_sten, canon_a, cov_a,
       @"rtcache:shadow-depth-pack-source");  // 1) source -> A (+ coverage)
  materialize(scratch, canon_a,
              @"rtcache:shadow-depth-materialize");  // 2) A -> scratch
  pack(scratch, scratch_sten, canon_b, nil,
       @"rtcache:shadow-depth-pack-roundtrip");  // 3) scratch -> B
  // Read A/B once the frame cb completes (NO commit/wait). The round-trip is now
  // scored ONLY over slots the collapsed store actually holds (coverage==1):
  // known slots that materialize+re-pack reproduce prove the substrate on the
  // LIVE representation; the missing 3/4 (coverage==0) are honestly reported, not
  // silently counted as matching zeros. Capture the metadata by value.
  const uint32_t words_per_tile = 80u * 16u;  // depth is 32bpp.
  uint64_t w0 = uint64_t(transfer.start_tiles) * words_per_tile;
  uint64_t w1 = uint64_t(transfer.end_tiles) * words_per_tile;
  uint64_t cap = kWords;
  if (w1 > cap) w1 = cap;
  uint32_t idx = shadow_count_, draw = command_processor_.trace_draw_index();
  uint32_t st = transfer.start_tiles, et = transfer.end_tiles;
  uint32_t src_msaa = 1u << uint32_t(source->GetKey().msaa_samples);
  const void* src_ptr = source->texture();
  std::string name = source->GetKey().GetDebugName();
  [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
    const uint32_t* a = static_cast<const uint32_t*>(canon_a.contents);
    const uint32_t* b = static_cast<const uint32_t*>(canon_b.contents);
    const uint8_t* cov = static_cast<const uint8_t*>(cov_a.contents);
    uint32_t known = 0, missing = 0, known_nz = 0, mism = 0;
    uint32_t ha = 2166136261u, hb = 2166136261u;
    for (uint64_t i = w0; i < w1; ++i) {
      if (cov[i]) {
        ++known;
        if (a[i]) ++known_nz;
        if (a[i] != b[i]) ++mism;
        ha = (ha ^ a[i]) * 16777619u;
        hb = (hb ^ b[i]) * 16777619u;
      } else {
        ++missing;
      }
    }
    REXLOG_WARN("MetalShadowX2X #{} @draw {}: {} sc={} msaa={}x {} tiles [{},{}) "
           "known={} missing={} known_nz={} mism={} hashA={:08x} hashB={:08x} "
           "srctex={} {}",
           idx, draw, name, sc, src_msaa, f24 ? "f24" : "un24", st, et, known,
           missing, known_nz, mism, ha, hb, src_ptr,
           mism == 0 ? "ROUNDTRIP-OK" : "MISMATCH");
    (void)canon_b;
    (void)cov_a;  // keep buffers alive until the handler runs.
  }];
}

void MetalRenderTargetCache::CanonicalDepthTransfer(const void* command_buffer,
                                                    RenderTarget* source_rt,
                                                    RenderTarget* dest_rt,
                                                    const Transfer& transfer) {
  InvalidateMsaaStencilCaches(kMsaaStencilDepthTransfer);
  // Option 1 depth authority: pack(source depth+stencil) -> canonical EDRAM,
  // then materialize(dest) <- canonical EDRAM, both guarded to the transfer's
  // tiles. Content lives in the durable canonical buffer, so it survives the RT
  // instance churn that lost it before -- NO transient-companion persistence.
  // Single-sample rendering: both host textures are sc=1, so the sc=1 range
  // pipelines and per-view SurfaceParams (base/pitch/msaa from each key) map each
  // view's pixels into the shared per-sample canonical space.
  auto* source = static_cast<MetalRenderTarget*>(source_rt);
  auto* dest = static_cast<MetalRenderTarget*>(dest_rt);
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> src_tex = (__bridge id<MTLTexture>)source->texture();
  id<MTLTexture> dst_tex = (__bridge id<MTLTexture>)dest->texture();
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  if (!cb || !src_tex || !dst_tex || !canonical) {
    return;
  }
  const ShadowDepthPipelines* p = EnsureShadowDepthPipelines(1u);
  if (!p || !p->pack_range || !p->mat_depth_range || !p->mat_sten_range) {
    // Fall back to the legacy resample if the canonical pipelines are missing.
    PerformCrossMsaaTransfer(command_buffer, source_rt, dest_rt, transfer);
    return;
  }
  auto fmt_is_depth = [](MetalRenderTarget* rt) -> uint32_t {
    return rt->GetKey().GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8
               ? 2u
               : 1u;
  };
  const uint32_t kWords = canonical_edram_size() / 4u;
  EdramSurfaceParams sp_src{
      source->GetKey().base_tiles, source->GetKey().GetPitchTiles(),
      uint32_t(source->GetKey().msaa_samples), fmt_is_depth(source), 0u, kWords};
  EdramSurfaceParams sp_dst{
      dest->GetKey().base_tiles, dest->GetKey().GetPitchTiles(),
      uint32_t(dest->GetKey().msaa_samples), fmt_is_depth(dest), 0u, kWords};
  struct RP {
    uint32_t start_tiles, end_tiles;
  } rp{transfer.start_tiles, transfer.end_tiles};

  id<MTLTexture> src_sten =
      [src_tex newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];

  // 1) pack source depth+stencil into canonical EDRAM (source's owned tiles).
  id<MTLComputePipelineState> pack_pso =
      (__bridge id<MTLComputePipelineState>)p->pack_range;
  {
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    e.label = @"rtcache:canonical-depth-pack";
    [e setComputePipelineState:pack_pso];
    [e setTexture:src_tex atIndex:0];
    [e setTexture:src_sten atIndex:1];
    [e setBuffer:canonical offset:0 atIndex:0];
    [e setBytes:&sp_src length:sizeof(sp_src) atIndex:1];
    [e setBytes:&rp length:sizeof(rp) atIndex:2];
    NSUInteger tw = pack_pso.threadExecutionWidth;
    NSUInteger th = pack_pso.maxTotalThreadsPerThreadgroup / tw;
    [e dispatchThreads:MTLSizeMake(src_tex.width, src_tex.height, 1)
        threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [e endEncoding];
  }

  // 2) materialize dest depth+stencil FROM canonical EDRAM (dest's tiles only;
  //    loadAction Load + the range guard leave every other tile untouched).
  MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
  rpd.depthAttachment.texture = dst_tex;
  rpd.depthAttachment.loadAction = MTLLoadActionLoad;
  rpd.depthAttachment.storeAction = MTLStoreActionStore;
  rpd.stencilAttachment.texture = dst_tex;
  rpd.stencilAttachment.loadAction = MTLLoadActionLoad;
  rpd.stencilAttachment.storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(rpd)];
  e.label = @"rtcache:canonical-depth-materialize";
  // Depth pass: write depth AND REPLACE stencil to 0 (so the bit-plane passes
  // fully replace instead of OR-ing over stale stencil -- the conformance lesson).
  [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)p->mat_depth_range];
  [e setDepthStencilState:(__bridge id<MTLDepthStencilState>)p->depth_state];
  [e setStencilReferenceValue:0];
  [e setFragmentBuffer:canonical offset:0 atIndex:0];
  [e setFragmentBytes:&sp_dst length:sizeof(sp_dst) atIndex:1];
  [e setFragmentBytes:&rp length:sizeof(rp) atIndex:2];
  {
    // Same-key cross-MSAA transfer: never a shared-plan two-source op.
    uint32_t hl_mode = 0u;
    [e setFragmentBytes:&hl_mode length:sizeof(hl_mode) atIndex:3];
    [e setFragmentBuffer:(__bridge id<MTLBuffer>)EnsureHostLaneBuffer(
                             (__bridge void*)cb.device)
                  offset:0
                 atIndex:4];
  }
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  // Stencil bit-plane REPLACE passes.
  [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)p->mat_sten_range];
  [e setStencilReferenceValue:0xFF];
  [e setFragmentBuffer:canonical offset:0 atIndex:0];
  [e setFragmentBytes:&sp_dst length:sizeof(sp_dst) atIndex:1];
  [e setFragmentBytes:&rp length:sizeof(rp) atIndex:2];
  for (uint32_t b = 0; b < 8; ++b) {
    uint32_t bit = 1u << b;
    [e setDepthStencilState:(__bridge id<MTLDepthStencilState>)p->bit_states[b]];
    [e setFragmentBytes:&bit length:sizeof(bit) atIndex:3];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [e endEncoding];

  static uint32_t s_logs = 0;
  if (s_logs++ < 128) {
    REXLOG_WARN("MetalCanonicalDepth @draw {}: {} ({}x)->{} ({}x) tiles [{},{})",
           command_processor_.trace_draw_index(), source->GetKey().GetDebugName(),
           1u << uint32_t(source->GetKey().msaa_samples), dest->GetKey().GetDebugName(),
           1u << uint32_t(dest->GetKey().msaa_samples), transfer.start_tiles,
           transfer.end_tiles);
  }
}

void MetalRenderTargetCache::PerformCrossMsaaTransfer(
    const void* command_buffer, RenderTarget* source_rt, RenderTarget* dest_rt,
    const Transfer& transfer) {
  InvalidateMsaaStencilCaches(kMsaaStencilCrossMsaa);
  auto* source = static_cast<MetalRenderTarget*>(source_rt);
  auto* dest = static_cast<MetalRenderTarget*>(dest_rt);
  // The half-height double-stack forensics: which draw triggers a cross-MSAA
  // transfer over which tiles (throttled; replay has dozens per frame).
  static uint32_t s_x2x_logs = 0;
  if (s_x2x_logs++ < 256) {
    id<MTLTexture> st = (__bridge id<MTLTexture>)source->texture();
    id<MTLTexture> dt = (__bridge id<MTLTexture>)dest->texture();
    REXLOG_WARN("MetalX2X @draw {}: {} ({}x{} tex={}) -> {} ({}x{} tex={}) "
           "tiles [{},{})",
           command_processor_.trace_draw_index(),
           source->GetKey().GetDebugName(), st.width, st.height,
           (void*)source->texture(), dest->GetKey().GetDebugName(), dt.width,
           dt.height, (void*)dest->texture(), transfer.start_tiles,
           transfer.end_tiles);
  }
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  bool is_depth = dest->GetKey().is_depth != 0;
  // Stage 2 shadow (XE_METAL_CROSSMSAA_SHADOW): additive on-device proof that the
  // shared depth substrate round-trips THIS real cross-MSAA source. Depth only;
  // modifies no render target, so the legacy resample below still runs and the
  // frame is byte-identical.
  static const bool s_shadow =
      IsMetalEnvironmentFlagEnabled("XE_METAL_CROSSMSAA_SHADOW");
  if (s_shadow && is_depth) {
    ShadowCrossMsaaDepth(command_buffer, source, transfer);
  }
  // XE_METAL_X2X_PROBE=1: after the first depth transfer, flush and read
  // back dest vs source pixels (verifies the copy lands). =2: at the SECOND
  // depth transfer, read the SOURCE (the msaa RT after the guest draw on it)
  // BEFORE encoding anything — locates where transferred content gets wiped.
  if (!transfer_x2x_library_) {
    // Same tile arithmetic as the reinterpret transfers; host->host, so no
    // raw-word decode and no depth 40-column swap.
    NSString* src = @R"METAL(
      #include <metal_stdlib>
      using namespace metal;
      struct XferParams {
        uint src_base; uint src_pitch; uint dst_base; uint dst_pitch;
        uint start_tiles; uint end_tiles; uint stencil_bit; uint shifts;
      };
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_x2x_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      // shifts: dst_x | dst_y<<4 | src_x<<8 | src_y<<12 (sample->pixel).
      static uint2 xe_x2x_src(uint2 dp, constant XferParams& p,
                              thread bool& in_range) {
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         dp.y << ((p.shifts >> 4) & 15u));
        uint dl = (ds.y / 16u) * p.dst_pitch + (ds.x / 80u);
        uint tile = p.dst_base + dl;
        in_range = tile >= p.start_tiles && tile < p.end_tiles;
        uint sl = tile - p.src_base;
        uint2 ss = uint2((sl % p.src_pitch) * 80u + (ds.x % 80u),
                         (sl / p.src_pitch) * 16u + (ds.y % 16u));
        return uint2(ss.x >> ((p.shifts >> 8) & 15u),
                     ss.y >> ((p.shifts >> 12) & 15u));
      }
      fragment float4 xe_x2x_col_fs(VOut in [[stage_in]],
                                    texture2d<float> src [[texture(0)]],
                                    constant XferParams& p [[buffer(0)]]) {
        bool in_range;
        uint2 sp = xe_x2x_src(uint2(in.pos.xy), p, in_range);
        if (!in_range) {
          discard_fragment();
        }
        return src.read(sp, 0);
      }
      struct DepthOut { float depth [[depth(any)]]; };
      fragment DepthOut xe_x2x_dep_fs(VOut in [[stage_in]],
                                      depth2d<float> src [[texture(0)]],
                                      constant XferParams& p [[buffer(0)]]) {
        bool in_range;
        uint2 sp = xe_x2x_src(uint2(in.pos.xy), p, in_range);
        if (!in_range) {
          discard_fragment();
        }
        DepthOut o;
        o.depth = src.read(sp);
        return o;
      }
      // Stencil bit-plane pass (source stencil sampled via an X32_Stencil8
      // view bound as texture2d<uint>); same masked-REPLACE scheme as c2d.
      fragment void xe_x2x_sten_fs(VOut in [[stage_in]],
                                   texture2d<uint> src [[texture(0)]],
                                   constant XferParams& p [[buffer(0)]]) {
        bool in_range;
        uint2 sp = xe_x2x_src(uint2(in.pos.xy), p, in_range);
        if (!in_range) {
          discard_fragment();
        }
        if ((src.read(sp, 0).r & p.stencil_bit) == 0u) {
          discard_fragment();
        }
      }
    )METAL";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: x2x transfer library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    transfer_x2x_library_ = (__bridge_retained void*)lib;
  }
  id<MTLLibrary> lib = (__bridge id<MTLLibrary>)transfer_x2x_library_;
  id<MTLRenderPipelineState> pipeline = nil;
  if (is_depth) {
    if (!transfer_x2x_depth_pipeline_) {
      NSError* err = nil;
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_x2x_vs"];
      d.fragmentFunction = [lib newFunctionWithName:@"xe_x2x_dep_fs"];
      d.depthAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
      d.stencilAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&err];
      if (!ps) {
        REXLOG_ERROR("MetalRenderTargetCache: x2x depth pipeline failed: {}",
               err ? err.localizedDescription.UTF8String : "?");
        return;
      }
      transfer_x2x_depth_pipeline_ = (__bridge_retained void*)ps;
      // The depth pass also zeroes the rect's stencil (REPLACE ref=0) as the
      // base the bit passes below OR the source stencil into.
      MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
      ds.depthCompareFunction = MTLCompareFunctionAlways;
      ds.depthWriteEnabled = YES;
      MTLStencilDescriptor* zero_sten = [[MTLStencilDescriptor alloc] init];
      zero_sten.stencilCompareFunction = MTLCompareFunctionAlways;
      zero_sten.depthStencilPassOperation = MTLStencilOperationReplace;
      zero_sten.stencilFailureOperation = MTLStencilOperationReplace;
      zero_sten.depthFailureOperation = MTLStencilOperationReplace;
      zero_sten.writeMask = 0xFF;
      ds.frontFaceStencil = zero_sten;
      ds.backFaceStencil = zero_sten;
      transfer_x2x_depth_state_ =
          (__bridge_retained void*)[device newDepthStencilStateWithDescriptor:ds];
      MTLRenderPipelineDescriptor* sd =
          [[MTLRenderPipelineDescriptor alloc] init];
      sd.vertexFunction = [lib newFunctionWithName:@"xe_x2x_vs"];
      sd.fragmentFunction = [lib newFunctionWithName:@"xe_x2x_sten_fs"];
      sd.depthAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
      sd.stencilAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
      id<MTLRenderPipelineState> sps =
          [device newRenderPipelineStateWithDescriptor:sd error:&err];
      if (sps) {
        transfer_x2x_sten_pipeline_ = (__bridge_retained void*)sps;
      } else {
        REXLOG_ERROR("MetalRenderTargetCache: x2x stencil pipeline failed: {}",
               err ? err.localizedDescription.UTF8String : "?");
      }
      EnsureStencilBitStates();
    }
    pipeline =
        (__bridge id<MTLRenderPipelineState>)transfer_x2x_depth_pipeline_;
  } else {
    uint64_t fmt = dest->pixel_format();
    auto it = transfer_x2x_color_pipelines_.find(fmt);
    if (it == transfer_x2x_color_pipelines_.end()) {
      NSError* err = nil;
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_x2x_vs"];
      d.fragmentFunction = [lib newFunctionWithName:@"xe_x2x_col_fs"];
      d.colorAttachments[0].pixelFormat = (MTLPixelFormat)fmt;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&err];
      if (!ps) {
        REXLOG_ERROR("MetalRenderTargetCache: x2x color pipeline {} failed: {}", fmt,
               err ? err.localizedDescription.UTF8String : "?");
      }
      it = transfer_x2x_color_pipelines_
               .emplace(fmt, ps ? (__bridge_retained void*)ps : nullptr)
               .first;
    }
    pipeline = (__bridge id<MTLRenderPipelineState>)it->second;
  }
  if (!pipeline) {
    return;
  }
  uint32_t sxs, sys, dxs, dys;
  GetMsaaShifts(uint32_t(source->GetKey().msaa_samples), sxs, sys);
  GetMsaaShifts(uint32_t(dest->GetKey().msaa_samples), dxs, dys);
  struct XferParams {
    uint32_t src_base, src_pitch, dst_base, dst_pitch;
    uint32_t start_tiles, end_tiles, stencil_bit, shifts;
  } params{source->GetKey().base_tiles, source->GetKey().GetPitchTiles(),
           dest->GetKey().base_tiles,   dest->GetKey().GetPitchTiles(),
           transfer.start_tiles,     transfer.end_tiles,
           0,                        dxs | (dys << 4) | (sxs << 8) | (sys << 12)};
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  if (is_depth) {
    pass.depthAttachment.texture = (__bridge id<MTLTexture>)dest->texture();
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.texture = (__bridge id<MTLTexture>)dest->texture();
    pass.stencilAttachment.loadAction = MTLLoadActionLoad;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
  } else {
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)dest->texture();
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  }
  id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  enc.label = @"rtcache:transfer-crossmsaa";
  [enc setRenderPipelineState:pipeline];
  if (is_depth) {
    [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                  transfer_x2x_depth_state_];
    [enc setStencilReferenceValue:0];  // depth pass zeroes the rect's stencil.
  }
  [enc setFragmentTexture:(__bridge id<MTLTexture>)source->texture() atIndex:0];
  [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  // Default ON: the earlier "x2x stencil makes it worse" verdict was an
  // artifact of the dump's msaa-shift bug (enum-vs-count). With the dump
  // fixed, stencil transfer takes 7282 draw-25 567K -> 327K diff dwords and
  // draw 900 reaches FULL EDRAM parity (0 tiles). XE_METAL_X2X_STEN_OFF=1
  // disables for A/B.
  static const bool s_x2x_sten_off = std::getenv("XE_METAL_X2X_STEN_OFF");
  if (is_depth && !s_x2x_sten_off && transfer_x2x_sten_pipeline_ &&
      stencil_bit_states_[0]) {
    // Transfer the source stencil plane: 8 masked REPLACE passes reading an
    // X32_Stencil8 view of the source (texture2d<uint>, .r = stencil).
    id<MTLTexture> src_tex = (__bridge id<MTLTexture>)source->texture();
    id<MTLTexture> sten_view = [src_tex
        newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    static uint32_t s_view_logs = 0;
    if (!sten_view && s_view_logs++ < 4) {
      REXLOG_ERROR("MetalX2X: X32_Stencil8 view creation FAILED (usage flags?)");
    }
    if (sten_view) {
      [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                      transfer_x2x_sten_pipeline_];
      [enc setStencilReferenceValue:0xFF];
      [enc setFragmentTexture:sten_view atIndex:0];
      for (uint32_t bit = 0; bit < 8; ++bit) {
        params.stencil_bit = 1u << bit;
        [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                      stencil_bit_states_[bit]];
        [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
      }
    }
  }
  [enc endEncoding];
}


// General color->color transfer with format conversion through the raw EDRAM
// word. Source texel -> 32-bit word (per source host format) -> dest texel
// (per dest format), with the same dest-pixel -> tile -> source-pixel remap
// as the cross-MSAA pass, so one pipeline handles reinterpret, cross-MSAA,
// and both combined. Format values are the Xenos storage-format enum and the
// conversion itself comes from the shared canonical MSL: 0=RGBA8, 2=RGB10A2,
// 3=RGBA16F holding 7e3-packed k_2_10_10_10_FLOAT, 14=R32Float.
void MetalRenderTargetCache::PerformColorConvertTransfer(
    const void* command_buffer, RenderTarget* source_rt, RenderTarget* dest_rt,
    const Transfer& transfer) {
  auto* source = static_cast<MetalRenderTarget*>(source_rt);
  auto* dest = static_cast<MetalRenderTarget*>(dest_rt);
  auto fmt_class = [](uint64_t pf) -> int32_t {
    switch ((MTLPixelFormat)pf) {
      case MTLPixelFormatRGBA8Unorm:
        return uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
      case MTLPixelFormatRGBA16Unorm:
        return uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA);
      case MTLPixelFormatR32Float:
        return uint32_t(xenos::ColorRenderTargetFormat::k_32_FLOAT);
      case MTLPixelFormatRGBA16Float:
        return uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
      case MTLPixelFormatRGB10A2Unorm:
        return uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10);
      default:
        return -1;
    }
  };
  int32_t src_fmt = fmt_class(source->pixel_format());
  int32_t dst_fmt = fmt_class(dest->pixel_format());
  if (src_fmt < 0 || dst_fmt < 0) {
    static uint32_t s_conv_skips = 0;
    if (s_conv_skips++ < 8) {
      REXLOG_WARN("MetalRenderTargetCache: conv transfer unsupported format {} -> {}",
             source->pixel_format(), dest->pixel_format());
    }
    return;
  }
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!transfer_conv_library_) {
    NSString* shared = [NSString stringWithUTF8String:kEdramMsl];
    NSString* src = [shared stringByAppendingString:@R"METAL(
      struct ConvParams {
        uint src_base; uint src_pitch; uint dst_base; uint dst_pitch;
        uint start_tiles; uint end_tiles; uint src_fmt; uint shifts;
      };
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_conv_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      fragment float4 xe_conv_fs(VOut in [[stage_in]],
                                 texture2d<float> src [[texture(0)]],
                                 constant ConvParams& p [[buffer(0)]],
                                 constant uint& dst_fmt [[buffer(1)]]) {
        uint2 dp = uint2(in.pos.xy);
        // shifts: dst_x | dst_y<<4 | src_x<<8 | src_y<<12 (sample->pixel).
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         dp.y << ((p.shifts >> 4) & 15u));
        uint dl = (ds.y / 16u) * p.dst_pitch + (ds.x / 80u);
        uint tile = p.dst_base + dl;
        if (tile < p.start_tiles || tile >= p.end_tiles) {
          discard_fragment();
        }
        uint sl = tile - p.src_base;
        uint2 ss = uint2((sl % p.src_pitch) * 80u + (ds.x % 80u),
                         (sl / p.src_pitch) * 16u + (ds.y % 16u));
        uint2 sp = uint2(ss.x >> ((p.shifts >> 8) & 15u),
                         ss.y >> ((p.shifts >> 12) & 15u));
        float4 c = src.read(sp, 0);
        return xe_unpack_color32_working(
            xe_pack_color32_working(c, p.src_fmt), dst_fmt);
      }
    )METAL"];
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: conv transfer library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    transfer_conv_library_ = (__bridge_retained void*)lib;
  }
  uint64_t pipe_key = dest->pixel_format();
  auto it = transfer_conv_pipelines_.find(pipe_key);
  if (it == transfer_conv_pipelines_.end()) {
    id<MTLLibrary> lib = (__bridge id<MTLLibrary>)transfer_conv_library_;
    MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"xe_conv_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"xe_conv_fs"];
    d.colorAttachments[0].pixelFormat = (MTLPixelFormat)dest->pixel_format();
    NSError* err = nil;
    id<MTLRenderPipelineState> ps =
        [device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) {
      REXLOG_ERROR("MetalRenderTargetCache: conv transfer pipeline {} failed: {}",
             dest->pixel_format(),
             err ? err.localizedDescription.UTF8String : "?");
    }
    it = transfer_conv_pipelines_
             .emplace(pipe_key, ps ? (__bridge_retained void*)ps : nullptr)
             .first;
  }
  if (!it->second) {
    return;
  }
  static uint32_t s_conv_logs = 0;
  if (s_conv_logs++ < 64) {
    REXLOG_WARN("MetalXferConv @draw {}: {} -> {} tiles [{},{}) fmt {}->{}",
           command_processor_.trace_draw_index(),
           source->GetKey().GetDebugName(), dest->GetKey().GetDebugName(),
           transfer.start_tiles, transfer.end_tiles, src_fmt, dst_fmt);
  }
  uint32_t sxs, sys, dxs, dys;
  GetMsaaShifts(uint32_t(source->GetKey().msaa_samples), sxs, sys);
  GetMsaaShifts(uint32_t(dest->GetKey().msaa_samples), dxs, dys);
  struct ConvParams {
    uint32_t src_base, src_pitch, dst_base, dst_pitch;
    uint32_t start_tiles, end_tiles, src_fmt, shifts;
  } params{source->GetKey().base_tiles, source->GetKey().GetPitchTiles(),
           dest->GetKey().base_tiles,   dest->GetKey().GetPitchTiles(),
           transfer.start_tiles,     transfer.end_tiles,
           uint32_t(src_fmt),
           dxs | (dys << 4) | (sxs << 8) | (sys << 12)};
  uint32_t dst_fmt_u = uint32_t(dst_fmt);
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)dest->texture();
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  enc.label = @"rtcache:transfer-convert";
  [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)it->second];
  [enc setFragmentTexture:(__bridge id<MTLTexture>)source->texture() atIndex:0];
  [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [enc setFragmentBytes:&dst_fmt_u length:sizeof(dst_fmt_u) atIndex:1];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];
}

// Lazily creates the 8 per-bit stencil-write states shared by the c2d and x2x
// stencil rebuild passes: func ALWAYS, REPLACE on pass (the fragment shader
// discards where the source bit is clear), write mask = that bit.
void MetalRenderTargetCache::EnsureStencilBitStates() {
  if (stencil_bit_states_[0]) {
    return;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  for (uint32_t bit = 0; bit < 8; ++bit) {
    MTLDepthStencilDescriptor* bds = [[MTLDepthStencilDescriptor alloc] init];
    bds.depthCompareFunction = MTLCompareFunctionAlways;
    bds.depthWriteEnabled = NO;
    MTLStencilDescriptor* bsten = [[MTLStencilDescriptor alloc] init];
    bsten.stencilCompareFunction = MTLCompareFunctionAlways;
    bsten.depthStencilPassOperation = MTLStencilOperationReplace;
    bsten.stencilFailureOperation = MTLStencilOperationKeep;
    bsten.depthFailureOperation = MTLStencilOperationReplace;
    bsten.writeMask = 1u << bit;
    bds.frontFaceStencil = bsten;
    bds.backFaceStencil = bsten;
    stencil_bit_states_[bit] =
        (__bridge_retained void*)[device newDepthStencilStateWithDescriptor:bds];
  }
}

void MetalRenderTargetCache::PerformColorToDepthTransfer(
    const void* command_buffer, RenderTarget* source_rt, RenderTarget* dest_rt,
    const Transfer& transfer) {
  InvalidateMsaaStencilCaches(kMsaaStencilColorToDepth);
  auto* source = static_cast<MetalRenderTarget*>(source_rt);
  auto* dest = static_cast<MetalRenderTarget*>(dest_rt);
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!transfer_c2d_pipeline_) {
    NSString* src = @R"METAL(
      #include <metal_stdlib>
      using namespace metal;
      struct XferParams {
        uint src_base; uint src_pitch; uint dst_base; uint dst_pitch;
        uint start_tiles; uint end_tiles; uint is_float24; uint shifts;
        uint stencil_bit; uint pad0; uint pad1; uint pad2;
      };
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_xfer_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      struct DepthOut { float depth [[depth(any)]]; };
      // 20e4 (4-bit exponent, 20-bit mantissa, [0,2)) guest float24 -> float32.
      static float xe_f20e4_to_f32(uint f24) {
        f24 &= 0xFFFFFFu;
        if (f24 == 0u) return 0.0;
        uint mantissa = f24 & 0xFFFFFu;
        uint exponent = f24 >> 20;
        if (exponent == 0u) {
          uint lzcnt = clz(mantissa) - (32u - 21u);
          exponent = uint(1 - int(lzcnt));
          mantissa = (mantissa << lzcnt) & 0xFFFFFu;
        }
        return as_type<float>(((exponent + 112u) << 23) | (mantissa << 3));
      }
      fragment DepthOut xe_xfer_c2d_fs(VOut in [[stage_in]],
                                       texture2d<float> src [[texture(0)]],
                                       constant XferParams& p [[buffer(0)]]) {
        uint2 dp = uint2(in.pos.xy);
        // shifts: dst_x | dst_y<<4 | src_x<<8 | src_y<<12 (sample->pixel).
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         dp.y << ((p.shifts >> 4) & 15u));
        uint dl = (ds.y / 16u) * p.dst_pitch + (ds.x / 80u);
        uint tile = p.dst_base + dl;
        if (tile < p.start_tiles || tile >= p.end_tiles) {
          discard_fragment();
        }
        uint sl = tile - p.src_base;
        // Depth EDRAM addressing swaps the 40-sample column halves of each
        // tile (see the reference dump shader): the raw word for host sample
        // x lives at x^40-ish (self-inverse +-40).
        uint dtx = ds.x % 80u;
        uint stx = dtx < 40u ? dtx + 40u : dtx - 40u;
        uint2 ss = uint2((sl % p.src_pitch) * 80u + stx,
                         (sl / p.src_pitch) * 16u + (ds.y % 16u));
        uint2 sp = uint2(ss.x >> ((p.shifts >> 8) & 15u),
                         ss.y >> ((p.shifts >> 12) & 15u));
        float4 c = src.read(sp, 0);
        // Canonical channel order: logical .r holds guest byte0 (RGBA8
        // storage == guest EDRAM bytes); depth in bits [8,32).
        uint bits = uint(round(c.r * 255.0)) | (uint(round(c.g * 255.0)) << 8) |
                    (uint(round(c.b * 255.0)) << 16) |
                    (uint(round(c.a * 255.0)) << 24);
        uint d24 = bits >> 8;
        DepthOut o;
        // Canonical unorm24 decode (pixel_formats.xesli XeUNorm24To32):
        // exact exponent-shift division by 2^24 with the >>23 bias trick;
        // /16777215.0 is off by an ULP for common values like 0xC00000.
        o.depth = p.is_float24 != 0u
                      ? xe_f20e4_to_f32(d24) * 0.5f
                      : float(d24 + (d24 >> 23)) * (1.0 / 16777216.0);
        return o;
      }
      // Stencil bit-plane pass: survives (and lets the stencil REPLACE op set
      // writemask's bit) only where the source word's stencil byte has
      // p.stencil_bit set. No depth/color output; Metal has no shader stencil
      // export, so the byte transfers as 8 masked REPLACE passes.
      fragment void xe_xfer_c2d_sten_fs(VOut in [[stage_in]],
                                        texture2d<float> src [[texture(0)]],
                                        constant XferParams& p [[buffer(0)]]) {
        uint2 dp = uint2(in.pos.xy);
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         dp.y << ((p.shifts >> 4) & 15u));
        uint dl = (ds.y / 16u) * p.dst_pitch + (ds.x / 80u);
        uint tile = p.dst_base + dl;
        if (tile < p.start_tiles || tile >= p.end_tiles) {
          discard_fragment();
        }
        uint sl = tile - p.src_base;
        uint dtx = ds.x % 80u;
        uint stx = dtx < 40u ? dtx + 40u : dtx - 40u;
        uint2 ss = uint2((sl % p.src_pitch) * 80u + stx,
                         (sl / p.src_pitch) * 16u + (ds.y % 16u));
        uint2 sp = uint2(ss.x >> ((p.shifts >> 8) & 15u),
                         ss.y >> ((p.shifts >> 12) & 15u));
        float4 c = src.read(sp, 0);
        uint stencil = uint(round(c.r * 255.0));  // guest byte0 = stencil.
        if ((stencil & p.stencil_bit) == 0u) {
          discard_fragment();
        }
      }
    )METAL";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: c2d transfer library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"xe_xfer_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"xe_xfer_c2d_fs"];
    d.depthAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
    id<MTLRenderPipelineState> ps =
        [device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) {
      REXLOG_ERROR("MetalRenderTargetCache: c2d transfer pipeline failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    transfer_c2d_pipeline_ = (__bridge_retained void*)ps;
    // Depth pass state: also zeroes the transferred rect's stencil (REPLACE
    // ref=0, full write mask) as the base the bit passes OR into.
    MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
    ds.depthCompareFunction = MTLCompareFunctionAlways;
    ds.depthWriteEnabled = YES;
    MTLStencilDescriptor* zero_sten = [[MTLStencilDescriptor alloc] init];
    zero_sten.stencilCompareFunction = MTLCompareFunctionAlways;
    zero_sten.depthStencilPassOperation = MTLStencilOperationReplace;
    zero_sten.stencilFailureOperation = MTLStencilOperationReplace;
    zero_sten.depthFailureOperation = MTLStencilOperationReplace;
    zero_sten.writeMask = 0xFF;
    ds.frontFaceStencil = zero_sten;
    ds.backFaceStencil = zero_sten;
    transfer_c2d_depth_state_ =
        (__bridge_retained void*)[device newDepthStencilStateWithDescriptor:ds];
    // Stencil bit-plane pipeline + one state per bit.
    MTLRenderPipelineDescriptor* sd =
        [[MTLRenderPipelineDescriptor alloc] init];
    sd.vertexFunction = [lib newFunctionWithName:@"xe_xfer_vs"];
    sd.fragmentFunction = [lib newFunctionWithName:@"xe_xfer_c2d_sten_fs"];
    sd.depthAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
    sd.stencilAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
    id<MTLRenderPipelineState> sps =
        [device newRenderPipelineStateWithDescriptor:sd error:&err];
    if (!sps) {
      REXLOG_ERROR("MetalRenderTargetCache: c2d stencil pipeline failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    transfer_c2d_sten_pipeline_ = (__bridge_retained void*)sps;
    EnsureStencilBitStates();
  }
  if (!transfer_c2d_pipeline_ || !transfer_c2d_depth_state_ ||
      !transfer_c2d_sten_pipeline_) {
    return;
  }
  uint32_t sxs, sys, dxs, dys;
  GetMsaaShifts(uint32_t(source->GetKey().msaa_samples), sxs, sys);
  GetMsaaShifts(uint32_t(dest->GetKey().msaa_samples), dxs, dys);
  struct XferParams {
    uint32_t src_base, src_pitch, dst_base, dst_pitch;
    uint32_t start_tiles, end_tiles, is_float24, shifts;
    uint32_t stencil_bit, pad0, pad1, pad2;
  } params{source->GetKey().base_tiles,     source->GetKey().GetPitchTiles(),
           dest->GetKey().base_tiles,       dest->GetKey().GetPitchTiles(),
           transfer.start_tiles,         transfer.end_tiles,
           uint32_t(dest->GetKey().GetDepthFormat() ==
                    xenos::DepthRenderTargetFormat::kD24FS8),
           dxs | (dys << 4) | (sxs << 8) | (sys << 12),
           0, 0, 0, 0};
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.depthAttachment.texture = (__bridge id<MTLTexture>)dest->texture();
  pass.depthAttachment.loadAction = MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.texture = (__bridge id<MTLTexture>)dest->texture();
  pass.stencilAttachment.loadAction = MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                  transfer_c2d_pipeline_];
  enc.label = @"rtcache:transfer-c2d";
  [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                transfer_c2d_depth_state_];
  [enc setStencilReferenceValue:0];  // depth pass zeroes the rect's stencil.
  [enc setFragmentTexture:(__bridge id<MTLTexture>)source->texture() atIndex:0];
  [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  // Rebuild the guest stencil byte from the source color's byte0, one masked
  // REPLACE pass per bit (no shader stencil export in Metal).
  [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                  transfer_c2d_sten_pipeline_];
  [enc setStencilReferenceValue:0xFF];
  for (uint32_t bit = 0; bit < 8; ++bit) {
    if (!stencil_bit_states_[bit]) {
      break;
    }
    params.stencil_bit = 1u << bit;
    [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                  stencil_bit_states_[bit]];
    [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [enc endEncoding];
}

void MetalRenderTargetCache::PerformDepthToColorTransfer(
    const void* command_buffer, RenderTarget* source_rt, RenderTarget* dest_rt,
    const Transfer& transfer, bool real_bits) {
  auto* source = static_cast<MetalRenderTarget*>(source_rt);
  auto* dest = static_cast<MetalRenderTarget*>(dest_rt);
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!transfer_d2c_pipeline_) {
    NSString* src = @R"METAL(
      #include <metal_stdlib>
      using namespace metal;
      struct XferParams {
        uint src_base; uint src_pitch; uint dst_base; uint dst_pitch;
        uint start_tiles; uint end_tiles; uint is_float24; uint shifts;
      };
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_xfer_d2c_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      // float32 -> guest 20e4 float24 (truncating).
      static uint xe_f32_to_f20e4(float f) {
        if (!(f > 0.0)) return 0u;
        uint u = as_type<uint>(f);
        if (u >= 0x40000000u) return 0xFFFFFFu;  // >= 2.0 saturates.
        if (u < 0x38800000u) {
          // Denormal in 20e4: shift the implicit-1 mantissa down.
          uint shift = min(113u - (u >> 23), 24u);
          u = (0x800000u | (u & 0x7FFFFFu)) >> shift;
        } else {
          u -= 0x38000000u;
        }
        return (u >> 3) & 0xFFFFFFu;
      }
      fragment float4 xe_xfer_d2c_fs(VOut in [[stage_in]],
                                     depth2d<float> src [[texture(0)]],
                                     texture2d<uint> sten [[texture(1)]],
                                     constant XferParams& p [[buffer(0)]]) {
        uint2 dp = uint2(in.pos.xy);
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         dp.y << ((p.shifts >> 4) & 15u));
        uint dl = (ds.y / 16u) * p.dst_pitch + (ds.x / 80u);
        uint tile = p.dst_base + dl;
        if (tile < p.start_tiles || tile >= p.end_tiles) {
          discard_fragment();
        }
        uint sl = tile - p.src_base;
        // Inverse of the depth EDRAM 40-column swap: this raw-word position
        // holds the host depth sample from the other 40-column half.
        uint dtx = ds.x % 80u;
        uint stx = dtx < 40u ? dtx + 40u : dtx - 40u;
        uint2 ss = uint2((sl % p.src_pitch) * 80u + stx,
                         (sl / p.src_pitch) * 16u + (ds.y % 16u));
        uint2 sp = uint2(ss.x >> ((p.shifts >> 8) & 15u),
                         ss.y >> ((p.shifts >> 12) & 15u));
        float d = src.read(sp);
        // rint = roundEven: the canonical unorm24 encode (the Vulkan dump
        // shader uses GLSL RoundEven; round-half-away differs at exact .5).
        uint d24 = p.is_float24 != 0u
                       ? xe_f32_to_f20e4(d * 2.0f)
                       : uint(rint(saturate(d) * 16777215.0));
        // DEFAULT: zero words. Rationale: a color pass claiming depth-owned
        // tiles only ever CONSUMES this content when the real producer draws
        // were lost (trace replay's stale-VB degenerate quads) — live, the
        // guest overwrites the tiles before reading. The stable Vulkan
        // reference zero-fills this case, and real f20e4 depth bytes here
        // read as saturated blue fog in JC2's composite (81.9 vs 49.9 MAE).
        // p.shifts bit 17 = probe: encode the REAL depth bits (d24<<8, with
        // bit 16 setting stencil FF instead of 0).
        // Bit 18 (0x40000): transport the REAL stencil plane — in Sonic's
        // packed vector map the stencil byte is part of each vector's value
        // (packed formats center at 128; a zero byte decodes as a large
        // negative component = the horizontal echo drag).
        uint stencil = (p.shifts & 0x40000u) != 0u
                           ? (sten.read(sp).r & 0xFFu)
                           : ((p.shifts & 0x10000u) != 0u ? 0xFFu : 0u);
        uint bits = (p.shifts & 0x20000u) != 0u ? ((d24 << 8) | stencil) : 0u;
        return float4(float(bits & 0xFFu), float((bits >> 8) & 0xFFu),
                      float((bits >> 16) & 0xFFu), float((bits >> 24) & 0xFFu)) /
               255.0;
      }
    )METAL";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: d2c transfer library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"xe_xfer_d2c_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"xe_xfer_d2c_fs"];
    d.colorAttachments[0].pixelFormat = (MTLPixelFormat)kColorFormat;
    id<MTLRenderPipelineState> ps =
        [device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) {
      REXLOG_ERROR("MetalRenderTargetCache: d2c transfer pipeline failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    transfer_d2c_pipeline_ = (__bridge_retained void*)ps;
  }
  if (!transfer_d2c_pipeline_) {
    return;
  }
  uint32_t sxs, sys, dxs, dys;
  GetMsaaShifts(uint32_t(source->GetKey().msaa_samples), sxs, sys);
  GetMsaaShifts(uint32_t(dest->GetKey().msaa_samples), dxs, dys);
  // A/B probes: XE_METAL_D2C_REAL=1 encodes the real depth bits instead of
  // the default zero words (see the shader comment); XE_METAL_D2C_STENCILFF=1
  // additionally writes 0xFF in the stencil byte (hardware-capture-like;
  // measured WORSE vs Vulkan truth).
  static const uint32_t s_stencil0_bit =
      (std::getenv("XE_METAL_D2C_STENCILFF") ? 0x10000u : 0u) |
      (std::getenv("XE_METAL_D2C_REAL") ? 0x20000u : 0u);
  // Ping-pong bit transport: the caller proved this transfer returns tiles
  // the dest color key lent THIS frame — the guest wrote real payload bits
  // through the depth plane (Sonic's vector map). Zero words here decode as
  // maximum-magnitude vectors (packed formats center at 0x8000): the
  // full-screen echo/streak haze.
  const uint32_t encode_bits =
      s_stencil0_bit | (real_bits ? (0x20000u | 0x40000u) : 0u);
  struct XferParams {
    uint32_t src_base, src_pitch, dst_base, dst_pitch;
    uint32_t start_tiles, end_tiles, is_float24, shifts;
  } params{source->GetKey().base_tiles,     source->GetKey().GetPitchTiles(),
           dest->GetKey().base_tiles,       dest->GetKey().GetPitchTiles(),
           transfer.start_tiles,         transfer.end_tiles,
           uint32_t(source->GetKey().GetDepthFormat() ==
                    xenos::DepthRenderTargetFormat::kD24FS8),
           dxs | (dys << 4) | (sxs << 8) | (sys << 12) | encode_bits};
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)dest->texture();
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                  transfer_d2c_pipeline_];
  enc.label = @"rtcache:transfer-d2c";
  id<MTLTexture> src_tex = (__bridge id<MTLTexture>)source->texture();
  [enc setFragmentTexture:src_tex atIndex:0];
  // Stencil plane view (bit 18 real-stencil transport). The shader statically
  // references the texture, so bind it even when the bit is off.
  id<MTLTexture> sten_view =
      [src_tex newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
  [enc setFragmentTexture:sten_view atIndex:1];
  [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];
}

bool MetalRenderTargetCache::ResolveSpanTouchesCurrentAttachments(
    uint32_t edram_base, uint32_t row_length_used, uint32_t rows,
    uint32_t pitch_tiles) const {
  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(edram_base, row_length_used, rows, pitch_tiles,
                                 rectangles);
  RenderTarget* const* accumulated = GetLastUpdateAccumulatedRenderTargets();
  for (const ResolveCopyDumpRectangle& rect : rectangles) {
    if (!rect.render_target) {
      continue;
    }
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      if (accumulated[i] == rect.render_target) {
        return true;
      }
    }
  }
  return false;
}

void MetalRenderTargetCache::ResolvePresentableMsaaCopy(void* command_buffer) {
  MetalRenderTarget* rt = presentable_color_rt_;
  if (!command_buffer || !rt || rt->sample_count() <= 1 ||
      !rt->msaa_texture() || rt->texture() != presentable_color_target_) {
    return;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)rt->msaa_texture();
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
  pass.colorAttachments[0].resolveTexture = (__bridge id<MTLTexture>)rt->texture();
  id<MTLRenderCommandEncoder> encoder =
      [(__bridge id<MTLCommandBuffer>)command_buffer
          renderCommandEncoderWithDescriptor:
              metal::MetalApplyXenosSamplePositions(pass)];
  encoder.label = @"rtcache:present-msaa-resolve";
  [encoder endEncoding];
}

void* MetalRenderTargetCache::GetResolveSourceTexture(
    uint32_t edram_base, uint32_t row_length_used, uint32_t rows,
    uint32_t pitch_tiles, bool is_depth, uint64_t* pixel_format_out) {
  resolve_dump_rectangles_.clear();
  GetResolveCopyRectanglesToDump(edram_base, row_length_used, rows, pitch_tiles,
                                 resolve_dump_rectangles_);
  // Ownership forensics (rides XE_METAL_PROBE_RESOLVE, any value): the span
  // and EVERY owner rect, not just the dominant — the transferless 2x->1x
  // ownership flip hides in this list.
  static const bool s_probe_owners =
      std::getenv("XE_METAL_PROBE_RESOLVE") != nullptr;
  if (s_probe_owners) {
    REXLOG_WARN(
        "MetalOwners @draw {}: span base={} rowlen={} rows={} pitch={} depth={}",
        command_processor_.trace_draw_index(), edram_base, row_length_used,
        rows, pitch_tiles, is_depth);
    for (const ResolveCopyDumpRectangle& rect : resolve_dump_rectangles_) {
      auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
      REXLOG_WARN("MetalOwners   rows[{}+{}] owner={}", rect.row_first, rect.rows,
             rt ? rt->GetKey().GetDebugName().c_str() : "(null)");
    }
  }
  // The span can be owned piecewise; pick the DOMINANT owner (most tiles) —
  // e.g. a 64-tile quarter-scene resolve whose first 4 tiles belong to a tiny
  // R32Float exposure RT must not take its snapshot format from the sliver.
  MetalRenderTarget* best = nullptr;
  uint32_t best_tiles = 0;
  for (const ResolveCopyDumpRectangle& rect : resolve_dump_rectangles_) {
    auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!rt || rt->GetKey().is_depth != uint32_t(is_depth)) {
      continue;
    }
    uint32_t tiles = rect.rows * row_length_used;
    if (tiles > best_tiles) {
      best_tiles = tiles;
      best = rt;
    }
  }
  if (!best) {
    return nullptr;
  }
  if (pixel_format_out) {
    *pixel_format_out = best->pixel_format();
  }
  return best->texture();
}

void MetalRenderTargetCache::PerformColorReinterpretTransfer(
    const void* command_buffer, RenderTarget* source_rt, RenderTarget* dest_rt,
    const Transfer& transfer) {
  auto* source = static_cast<MetalRenderTarget*>(source_rt);
  auto* dest = static_cast<MetalRenderTarget*>(dest_rt);
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  bool to_float = dest->pixel_format() == uint64_t(MTLPixelFormatR32Float);
  if (!transfer_reint_c2f_pipeline_ || !transfer_reint_f2c_pipeline_) {
    // Same tile arithmetic as the c2d/d2c reinterprets, but color tiles have
    // no 40-sample column swap; the EDRAM 32-bit word itself is preserved.
    // Byte order matches the c2d reconstruction: word = b|g<<8|r<<16|a<<24.
    NSString* src = @R"METAL(
      #include <metal_stdlib>
      using namespace metal;
      struct XferParams {
        uint src_base; uint src_pitch; uint dst_base; uint dst_pitch;
        uint start_tiles; uint end_tiles; uint unused; uint shifts;
      };
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_xfer_reint_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      static uint2 xe_reint_src_pixel(uint2 dp, constant XferParams& p,
                                      thread bool& in_range) {
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         dp.y << ((p.shifts >> 4) & 15u));
        uint dl = (ds.y / 16u) * p.dst_pitch + (ds.x / 80u);
        uint tile = p.dst_base + dl;
        in_range = tile >= p.start_tiles && tile < p.end_tiles;
        uint sl = tile - p.src_base;
        uint2 ss = uint2((sl % p.src_pitch) * 80u + (ds.x % 80u),
                         (sl / p.src_pitch) * 16u + (ds.y % 16u));
        return uint2(ss.x >> ((p.shifts >> 8) & 15u),
                     ss.y >> ((p.shifts >> 12) & 15u));
      }
      // BGRA8 raw bytes -> the same EDRAM word as f32 bits.
      fragment float4 xe_xfer_c2f_fs(VOut in [[stage_in]],
                                     texture2d<float> src [[texture(0)]],
                                     constant XferParams& p [[buffer(0)]]) {
        bool in_range;
        uint2 sp = xe_reint_src_pixel(uint2(in.pos.xy), p, in_range);
        if (!in_range || sp.x >= src.get_width() || sp.y >= src.get_height()) {
          discard_fragment();
        }
        float4 c = src.read(sp, 0);
        // Canonical: logical .r = guest byte0 (RGBA8 storage).
        uint bits = uint(round(c.r * 255.0)) | (uint(round(c.g * 255.0)) << 8) |
                    (uint(round(c.b * 255.0)) << 16) |
                    (uint(round(c.a * 255.0)) << 24);
        return float4(as_type<float>(bits), 0.0, 0.0, 0.0);
      }
      // f32 bits -> the same EDRAM word as BGRA8 raw bytes.
      fragment float4 xe_xfer_f2c_fs(VOut in [[stage_in]],
                                     texture2d<float> src [[texture(0)]],
                                     constant XferParams& p [[buffer(0)]]) {
        bool in_range;
        uint2 sp = xe_reint_src_pixel(uint2(in.pos.xy), p, in_range);
        if (!in_range || sp.x >= src.get_width() || sp.y >= src.get_height()) {
          discard_fragment();
        }
        uint bits = as_type<uint>(src.read(sp, 0).x);
        // Canonical: guest byte0 -> logical .r (matches d2c).
        return float4(float(bits & 0xFFu), float((bits >> 8) & 0xFFu),
                      float((bits >> 16) & 0xFFu), float(bits >> 24)) / 255.0;
      }
    )METAL";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: reinterpret transfer library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    auto make_pipeline = [&](NSString* fs, MTLPixelFormat rt_format) -> void* {
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_xfer_reint_vs"];
      d.fragmentFunction = [lib newFunctionWithName:fs];
      d.colorAttachments[0].pixelFormat = rt_format;
      NSError* perr = nil;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&perr];
      if (!ps) {
        REXLOG_ERROR("MetalRenderTargetCache: reinterpret pipeline failed: {}",
               perr ? perr.localizedDescription.UTF8String : "?");
        return nullptr;
      }
      return (__bridge_retained void*)ps;
    };
    transfer_reint_c2f_pipeline_ =
        make_pipeline(@"xe_xfer_c2f_fs", MTLPixelFormatR32Float);
    transfer_reint_f2c_pipeline_ =
        make_pipeline(@"xe_xfer_f2c_fs", (MTLPixelFormat)kColorFormat);
  }
  void* pipeline =
      to_float ? transfer_reint_c2f_pipeline_ : transfer_reint_f2c_pipeline_;
  if (!pipeline) {
    return;
  }
  uint32_t sxs, sys, dxs, dys;
  GetMsaaShifts(uint32_t(source->GetKey().msaa_samples), sxs, sys);
  GetMsaaShifts(uint32_t(dest->GetKey().msaa_samples), dxs, dys);
  struct XferParams {
    uint32_t src_base, src_pitch, dst_base, dst_pitch;
    uint32_t start_tiles, end_tiles, unused, shifts;
  } params{source->GetKey().base_tiles, source->GetKey().GetPitchTiles(),
           dest->GetKey().base_tiles,   dest->GetKey().GetPitchTiles(),
           transfer.start_tiles,     transfer.end_tiles,
           0,                        dxs | (dys << 4) | (sxs << 8) | (sys << 12)};
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)dest->texture();
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  enc.label = @"rtcache:transfer-reinterpret";
  [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)pipeline];
  [enc setFragmentTexture:(__bridge id<MTLTexture>)source->texture() atIndex:0];
  [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];
}

bool MetalRenderTargetCache::ResolveDepthToSnapshot(
    const void* command_buffer, uint32_t span_base, uint32_t row_length_used,
    uint32_t rows, uint32_t span_pitch, uint32_t span_msaa_samples,
    bool is_float24, void* dest_snapshot, uint32_t width, uint32_t height,
    uint32_t dest_y_offset) {
  if (!command_buffer || !dest_snapshot) {
    return false;
  }
  // The span can be owned PIECEWISE by several render targets; convert each
  // owner's rectangle from its texture.
  resolve_dump_rectangles_.clear();
  GetResolveCopyRectanglesToDump(span_base, row_length_used, rows, span_pitch,
                                 resolve_dump_rectangles_);
  if (resolve_dump_rectangles_.empty()) {
    return false;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!resolve_c2f_pipeline_) {
    NSString* src = @R"METAL(
      #include <metal_stdlib>
      using namespace metal;
      struct Params {
        uint src_base; uint src_pitch; uint span_base; uint span_pitch;
        uint y_offset; uint is_float24; uint shifts; uint pad1;
      };
      // shifts: src_x | src_y<<4 | span_x<<8 | span_y<<12 (sample->pixel).
      static uint2 xe_span_sample(uint2 dp, uint y_offset, uint shifts) {
        return uint2(dp.x << ((shifts >> 8) & 15u),
                     (dp.y - y_offset) << ((shifts >> 12) & 15u));
      }
      static uint2 xe_src_pixel(uint sl, uint src_pitch, uint2 ds,
                                uint shifts) {
        uint2 ss = uint2((sl % src_pitch) * 80u + (ds.x % 80u),
                         (sl / src_pitch) * 16u + (ds.y % 16u));
        return uint2(ss.x >> (shifts & 15u), ss.y >> ((shifts >> 4) & 15u));
      }
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_c2f_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      static float xe_f20e4_to_f32(uint f24) {
        f24 &= 0xFFFFFFu;
        if (f24 == 0u) return 0.0;
        uint mantissa = f24 & 0xFFFFFu;
        uint exponent = f24 >> 20;
        if (exponent == 0u) {
          uint lzcnt = clz(mantissa) - (32u - 21u);
          exponent = uint(1 - int(lzcnt));
          mantissa = (mantissa << lzcnt) & 0xFFFFFu;
        }
        return as_type<float>(((exponent + 112u) << 23) | (mantissa << 3));
      }
      fragment float xe_c2f_fs(VOut in [[stage_in]],
                               texture2d<float> src [[texture(0)]],
                               constant Params& p [[buffer(0)]]) {
        uint2 dp = uint2(in.pos.xy);
        uint2 ds = xe_span_sample(dp, p.y_offset, p.shifts);
        uint tile = p.span_base + (ds.y / 16u) * p.span_pitch + (ds.x / 80u);
        uint sl = tile - p.src_base;
        // Depth EDRAM 40-column swap: the word for this span sample is in
        // the other 40-column half of the tile.
        uint2 dsw = uint2((ds.x / 80u) * 80u +
                              ((ds.x % 80u) < 40u ? (ds.x % 80u) + 40u
                                                  : (ds.x % 80u) - 40u),
                          ds.y);
        uint2 sp = xe_src_pixel(sl, p.src_pitch, dsw, p.shifts);
        float4 c = src.read(sp, 0);
        uint bits = uint(round(c.b * 255.0)) | (uint(round(c.g * 255.0)) << 8) |
                    (uint(round(c.r * 255.0)) << 16) |
                    (uint(round(c.a * 255.0)) << 24);
        uint d24 = bits >> 8;
        // Canonical unorm24 decode (see XeUNorm24To32).
        return p.is_float24 != 0u
                   ? xe_f20e4_to_f32(d24)
                   : float(d24 + (d24 >> 23)) * (1.0 / 16777216.0);
      }
      fragment float xe_d2f_fs(VOut in [[stage_in]],
                               depth2d<float> src [[texture(0)]],
                               constant Params& p [[buffer(0)]]) {
        uint2 dp = uint2(in.pos.xy);
        uint2 ds = xe_span_sample(dp, p.y_offset, p.shifts);
        uint tile = p.span_base + (ds.y / 16u) * p.span_pitch + (ds.x / 80u);
        uint sl = tile - p.src_base;
        uint2 sp = xe_src_pixel(sl, p.src_pitch, ds, p.shifts);
        float host_depth = src.read(sp);
        // The depth attachment stores the full float24 guest [0,2) domain in
        // host [0,1]. Resolve snapshots are guest-visible numeric textures, so
        // undo that attachment encoding just like the canonical resolve path.
        return p.is_float24 != 0u ? host_depth * 2.0f : host_depth;
      }
    )METAL";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: resolve library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return false;
    }
    auto make_pipeline = [&](NSString* fs_name) -> void* {
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_c2f_vs"];
      d.fragmentFunction = [lib newFunctionWithName:fs_name];
      d.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
      NSError* perr = nil;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&perr];
      if (!ps) {
        REXLOG_ERROR("MetalRenderTargetCache: resolve pipeline {} failed: {}",
               fs_name.UTF8String,
               perr ? perr.localizedDescription.UTF8String : "?");
        return nullptr;
      }
      return (__bridge_retained void*)ps;
    };
    resolve_c2f_pipeline_ = make_pipeline(@"xe_c2f_fs");
    resolve_d2f_pipeline_ = make_pipeline(@"xe_d2f_fs");
  }
  if (!resolve_c2f_pipeline_ || !resolve_d2f_pipeline_) {
    return false;
  }
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)dest_snapshot;
  pass.colorAttachments[0].slice = 0;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> enc =
      [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  enc.label = @"rtcache:resolve-depth-snapshot";
  bool any = false;
  for (const ResolveCopyDumpRectangle& rect : resolve_dump_rectangles_) {
    auto* owner = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!owner) {
      continue;
    }
    bool owner_is_depth = owner->GetKey().is_depth != 0;
    if (!owner_is_depth && owner->pixel_format() != kColorFormat) {
      continue;  // Raw-bits decode only understands 32bpp color for now.
    }
    // The rect's tile rows within the span -> destination pixel rows (span
    // sample rows >> span y-shift).
    uint32_t oxs, oys, pxs, pys;
    GetMsaaShifts(uint32_t(owner->GetKey().msaa_samples), oxs, oys);
    // Owner shifts govern both sides (aliased single-sample textures; see the
    // color resolve).
    pxs = oxs;
    pys = oys;
    (void)span_msaa_samples;
    uint32_t y0 = (rect.row_first * 16) >> pys;
    if (y0 >= height) {
      continue;
    }
    uint32_t h = std::min((rect.rows * 16) >> pys, height - y0);
    struct Params {
      uint32_t src_base, src_pitch, span_base, span_pitch;
      uint32_t y_offset, is_float24, shifts, pad1;
    } params{owner->GetKey().base_tiles, owner->GetKey().GetPitchTiles(),
             span_base,               span_pitch,
             dest_y_offset,           uint32_t(is_float24),
             oxs | (oys << 4) | (pxs << 8) | (pys << 12), 0};
    [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)(
                                    owner_is_depth ? resolve_d2f_pipeline_
                                                   : resolve_c2f_pipeline_)];
    MTLViewport vp{0.0, double(dest_y_offset + y0), double(width), double(h),
                   0.0, 1.0};
    [enc setViewport:vp];
    // Viewports don't clip (the fullscreen triangle overflows 2x); scissor
    // does — without it each rect would stomp the rows below it.
    id<MTLTexture> dest_tex = (__bridge id<MTLTexture>)dest_snapshot;
    uint32_t sy = std::min<uint32_t>(dest_y_offset + y0,
                                     uint32_t(dest_tex.height));
    uint32_t sh =
        std::min<uint32_t>(h, uint32_t(dest_tex.height) - sy);
    if (!sh) {
      continue;
    }
    [enc setScissorRect:MTLScissorRect{0, sy,
                                       std::min<NSUInteger>(width,
                                                            dest_tex.width),
                                       sh}];
    [enc setFragmentTexture:(__bridge id<MTLTexture>)owner->texture()
                    atIndex:0];
    [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    any = true;
    static uint32_t s_logs = 0;
    if (s_logs++ < 16) {
      REXLOG_INFO(
          "MetalRenderTargetCache: depth resolve rect rows[{}+{}] from {}",
          rect.row_first, rect.rows, owner->GetKey().GetDebugName());
    }
  }
  [enc endEncoding];
  return any;
}

bool MetalRenderTargetCache::ResolveColorToSnapshot(
    const void* command_buffer, uint32_t span_base, uint32_t row_length_used,
    uint32_t rows, uint32_t span_pitch, uint32_t span_msaa_samples,
    void* dest_snapshot, uint64_t dest_pixel_format, uint32_t width,
    uint32_t height, uint32_t dest_y_offset, float exp_bias_scale,
    uint32_t dest_int_bits) {
  if (!command_buffer || !dest_snapshot) {
    return false;
  }
  resolve_dump_rectangles_.clear();
  GetResolveCopyRectanglesToDump(span_base, row_length_used, rows, span_pitch,
                                 resolve_dump_rectangles_);
  if (resolve_dump_rectangles_.empty()) {
    return false;
  }
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> dest_tex = (__bridge id<MTLTexture>)dest_snapshot;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  // One sampling pass per owner rect instead of per-tile blits: hundreds of
  // copyFromTexture calls each created a driver texture view — the #2 cost
  // center in gameplay profiles (after the shared-memory memcmp).
  if (!resolve_copy_library_) {
    NSString* src = @R"METAL(
      #include <metal_stdlib>
      using namespace metal;
      struct Params {
        uint src_base; uint src_pitch; uint span_base; uint span_pitch;
        uint start_tiles; uint end_tiles; uint y_offset; uint shifts;
        uint tile_w; float exp_bias_scale; uint dest_int_bits; uint pad3;
      };
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_rcp_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      // shifts: dst_x | dst_y<<4 | src_x<<8 | src_y<<12 (sample->pixel).
      fragment float4 xe_rcp_fs(VOut in [[stage_in]],
                                texture2d<float> src [[texture(0)]],
                                constant Params& p [[buffer(0)]]) {
        uint2 dp = uint2(in.pos.xy);
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         (dp.y - p.y_offset) << ((p.shifts >> 4) & 15u));
        uint col = ds.x / p.tile_w;
        // Past the span's tile row width: would alias into the NEXT tile row.
        if (col >= p.span_pitch) {
          discard_fragment();
        }
        uint tile = p.span_base + (ds.y / 16u) * p.span_pitch + col;
        if (tile < p.start_tiles || tile >= p.end_tiles) {
          discard_fragment();
        }
        uint sl = tile - p.src_base;
        uint2 ss = uint2((sl % p.src_pitch) * p.tile_w + (ds.x % p.tile_w),
                         (sl / p.src_pitch) * 16u + (ds.y % 16u));
        uint2 sp = uint2(ss.x >> ((p.shifts >> 8) & 15u),
                         ss.y >> ((p.shifts >> 12) & 15u));
        // Owner texture bounds (partial edge tiles).
        if (sp.x >= src.get_width() || sp.y >= src.get_height()) {
          discard_fragment();
        }
        float4 v = src.read(sp, 0) * p.exp_bias_scale;
        // Integer-number-format destination in a float snapshot: see
        // CanonicalResolveParams::dest_int_bits.
        if (p.dest_int_bits != 0u) {
          uint4 bits =
              (uint4(p.dest_int_bits) >> uint4(0u, 8u, 16u, 24u)) & 0xFFu;
          v /= max(exp2(float4(bits)) - 1.0, 1.0);
        }
        return v;
      }
    )METAL";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: resolve-copy library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return false;
    }
    resolve_copy_library_ = (__bridge_retained void*)lib;
  }
  id<MTLRenderPipelineState> pipeline = nil;
  {
    auto it = resolve_copy_pipelines_.find(dest_pixel_format);
    if (it == resolve_copy_pipelines_.end()) {
      id<MTLLibrary> lib = (__bridge id<MTLLibrary>)resolve_copy_library_;
      NSError* err = nil;
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_rcp_vs"];
      d.fragmentFunction = [lib newFunctionWithName:@"xe_rcp_fs"];
      d.colorAttachments[0].pixelFormat = (MTLPixelFormat)dest_pixel_format;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&err];
      if (!ps) {
        REXLOG_ERROR("MetalRenderTargetCache: resolve-copy pipeline {} failed: {}",
               dest_pixel_format,
               err ? err.localizedDescription.UTF8String : "?");
      }
      it = resolve_copy_pipelines_
               .emplace(dest_pixel_format,
                        ps ? (__bridge_retained void*)ps : nullptr)
               .first;
    }
    pipeline = (__bridge id<MTLRenderPipelineState>)it->second;
  }
  if (!pipeline) {
    return false;
  }
  bool any = false;
  for (const ResolveCopyDumpRectangle& rect : resolve_dump_rectangles_) {
    auto* owner = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!owner || owner->GetKey().is_depth) {
      continue;
    }
    // Metal performs the numeric color conversion when the sampled float4 is
    // written to the destination attachment. These are the host containers
    // used by Xenos color RTs and resolve destinations; in particular, Red
    // Dead resolves RGBA16F HDR EDRAM into its RGBA8 frontbuffer here.
    auto numeric_color_format = [](uint64_t format) {
      return format == kColorFormat ||
             format == uint64_t(MTLPixelFormatR32Float) ||
             format == uint64_t(MTLPixelFormatRG16Float) ||
             format == uint64_t(MTLPixelFormatRGBA16Float) ||
             format == uint64_t(MTLPixelFormatRGB10A2Unorm);
    };
    if (!numeric_color_format(owner->pixel_format()) ||
        !numeric_color_format(dest_pixel_format)) {
      static uint32_t s_skip_convert_logs = 0;
      if (s_skip_convert_logs++ < 8) {
        REXLOG_WARN(
            "MetalRenderTargetCache: SKIP color resolve owner fmt {} -> dest "
            "fmt {} (unsupported conversion)",
            owner->pixel_format(), dest_pixel_format);
      }
      continue;
    }
    uint32_t owner_base = owner->GetKey().base_tiles;
    uint32_t owner_pitch = owner->GetKey().GetPitchTiles();
    uint32_t tile_w = owner->GetKey().Is64bpp() ? 40 : 80;
    // Owner shifts govern BOTH sides of the mapping: our textures are aliased
    // single-sample, so an MSAA surface already stores one texel per resolved
    // pixel — a guest 2x->1x resolve is a 1:1 pixel copy in our model. (The
    // declared span msaa can differ from the owner's; the physical layout is
    // the owner's.)
    uint32_t oxs, oys;
    GetMsaaShifts(uint32_t(owner->GetKey().msaa_samples), oxs, oys);
    (void)span_msaa_samples;
    static uint32_t s_owner_logs = 0;
    if (s_owner_logs++ < 12) {
      REXLOG_INFO("MetalRenderTargetCache: color resolve owner {} rt={:X} rows[{}+{}]",
             owner->GetKey().GetDebugName(), uintptr_t(owner->texture()),
             rect.row_first, rect.rows);
    }
    struct Params {
      uint32_t src_base, src_pitch, span_base, span_pitch;
      uint32_t start_tiles, end_tiles, y_offset, shifts;
      uint32_t tile_w;
      float exp_bias_scale;
      uint32_t dest_int_bits, pad3;
    } params{owner_base,
             owner_pitch,
             span_base,
             span_pitch,
             span_base + rect.row_first * span_pitch,
             span_base + (rect.row_first + rect.rows) * span_pitch,
             dest_y_offset,
             oxs | (oys << 4) | (oxs << 8) | (oys << 12),
             tile_w,
             exp_bias_scale,
             dest_int_bits,
             0};
    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = dest_tex;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc =
        [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
    enc.label = @"rtcache:resolve-color-snapshot";
    [enc setRenderPipelineState:pipeline];
    // Scissor to this rect's destination rows (the tile-range discard handles
    // exact bounds; the scissor keeps other strips/padding untouched).
    uint32_t sy0 = dest_y_offset + ((rect.row_first * 16) >> oys);
    uint32_t sy1 = dest_y_offset + (((rect.row_first + rect.rows) * 16) >> oys);
    sy0 = std::min<uint32_t>(sy0, uint32_t(dest_tex.height));
    sy1 = std::min<uint32_t>(sy1, uint32_t(dest_tex.height));
    if (sy1 <= sy0) {
      [enc endEncoding];
      continue;
    }
    MTLScissorRect scissor;
    scissor.x = 0;
    scissor.width = dest_tex.width;
    scissor.y = sy0;
    scissor.height = sy1 - sy0;
    [enc setScissorRect:scissor];
    [enc setFragmentTexture:(__bridge id<MTLTexture>)owner->texture()
                    atIndex:0];
    [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    any = true;
  }
  return any;
}

bool MetalRenderTargetCache::ResolveCanonicalToSnapshot(
    const void* command_buffer, bool is_depth, uint32_t base_tiles,
    uint32_t pitch_tiles, uint32_t msaa_samples, uint32_t format,
    bool is_64bpp, uint32_t source_x, uint32_t source_y,
    uint32_t sample_select, bool dest_swap, bool raw_copy,
    void* dest_snapshot,
    uint64_t dest_pixel_format, uint32_t width, uint32_t height,
    uint32_t dest_y_offset, float exp_bias_scale,
    uint32_t dest_x_offset, uint32_t dest_int_bits) {
  xenos::ColorRenderTargetFormat storage_format =
      is_depth
          ? xenos::ColorRenderTargetFormat::k_8_8_8_8
          : xenos::GetStorageColorFormat(
                xenos::ColorRenderTargetFormat(format));
  // Gamma changes render-time transfer behavior, not the raw EDRAM packing.
  // Canonical resolve reads the stored bytes just like plain 8_8_8_8 (matching
  // the existing resolve shader's gamma-storage behavior).
  if (storage_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
    storage_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  }
  if (!sample_edram_raster_enabled_ || !canonical_edram_ || !command_buffer ||
      !dest_snapshot || !width || !height ||
      (!is_depth && !IsCanonicalColorFormatSupported(storage_format)) ||
      !EnsureCanonicalMaterializePipeline()) {
    return false;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  const uint64_t pipeline_key =
      dest_pixel_format | (uint64_t(is_depth) << 63u);
  auto found = canonical_resolve_pipelines_.find(pipeline_key);
  if (found == canonical_resolve_pipelines_.end()) {
    id<MTLLibrary> library =
        (__bridge id<MTLLibrary>)canonical_materialize_library_;
    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"xe_color_vs"];
    descriptor.fragmentFunction = [library
        newFunctionWithName:is_depth ? @"resolve_canonical_depth_fs"
                                     : @"resolve_canonical_color_fs"];
    descriptor.colorAttachments[0].pixelFormat =
        (MTLPixelFormat)dest_pixel_format;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) {
      REXLOG_ERROR(
          "MetalRenderTargetCache: canonical {} resolve pipeline {} failed: "
          "{}",
          is_depth ? "depth" : "color", dest_pixel_format,
          error ? error.localizedDescription.UTF8String : "?");
      return false;
    }
    found = canonical_resolve_pipelines_
                .emplace(pipeline_key, (__bridge_retained void*)pipeline)
                .first;
  }
  id<MTLTexture> destination = (__bridge id<MTLTexture>)dest_snapshot;
  // The snapshot texture is at the draw resolution scale; the resolve's
  // offsets and extents are guest pixels.
  const uint32_t sx = GetDrawResolutionScaleX(), sy = GetDrawResolutionScaleY();
  const uint32_t guest_dest_width = uint32_t(destination.width) / sx;
  const uint32_t guest_dest_height = uint32_t(destination.height) / sy;
  if (dest_y_offset >= guest_dest_height || dest_x_offset >= guest_dest_width) {
    return false;
  }
  uint32_t clipped_width =
      std::min<uint32_t>(width, guest_dest_width - dest_x_offset);
  uint32_t clipped_height =
      std::min<uint32_t>(height, guest_dest_height - dest_y_offset);
  if (!clipped_width || !clipped_height) {
    return false;
  }
  EdramSurfaceParams surface{
      base_tiles, pitch_tiles, msaa_samples,
      is_depth
          ? (format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8) ? 2u
                                                                        : 1u)
          : 0u,
      uint32_t(!is_depth && is_64bpp),
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct ResolveParams {
    EdramSurfaceParams surface;
    uint32_t source_x, source_y, dest_y, sample_select;
    uint32_t color_format;
    uint32_t dest_swap;
    uint32_t dest_red_only;
    float exp_bias_scale;
    uint32_t width, height;
    uint32_t dest_format, dest_bpp_log2, dest_endian, raw_copy;
    // Scatter fields (unused by the snapshot fragment path) and the integer
    // destination widths; the MSL struct is shared with the packed path.
    uint32_t scatter_enable;
    uint32_t dest_base, dest_pitch_aligned;
    uint32_t dest_offset_x, dest_offset_y, dest_extent_bytes;
    uint32_t dest_number;
    uint32_t dest_int_bits;
  } params{surface,
           // A destination x offset (an assembled column) shifts the shader's
           // destination->source mapping and bound; the scissor excludes the
           // columns left of it. The subtraction wraps; the shader's
           // source_x + dest.x wraps back.
           source_x - dest_x_offset, source_y, dest_y_offset, sample_select,
           uint32_t(storage_format), uint32_t(dest_swap),
           uint32_t(!is_depth && raw_copy &&
                    dest_pixel_format == uint64_t(MTLPixelFormatR32Float)),
           exp_bias_scale, dest_x_offset + clipped_width, clipped_height, 0u,
           0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, dest_int_bits};
  static_assert(sizeof(ResolveParams) == 28u * sizeof(uint32_t));
  static const bool s_canonical_resolve_log =
      std::getenv("XE_METAL_RESOLVE_LOG") != nullptr;
  if (s_canonical_resolve_log && width >= 512) {
    REXLOG_WARN("MetalCanonicalResolve {}x{} depth={} format={} storage_format={} "
           "raw_copy={} dest_swap={} dest_pixel_format={} msaa={} base_tiles={}",
           width, height, is_depth, format, uint32_t(storage_format), raw_copy,
           dest_swap, dest_pixel_format, msaa_samples, base_tiles);
  }
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  if (SnapshotComputeEnabled() &&
      (destination.usage & MTLTextureUsageShaderWrite)) {
    void*& cs_slot = is_depth ? canonical_depth_snapshot_cs_pipeline_
                              : canonical_color_snapshot_cs_pipeline_;
    if (!cs_slot) {
      id<MTLLibrary> library =
          (__bridge id<MTLLibrary>)canonical_materialize_library_;
      id<MTLFunction> function = [library
          newFunctionWithName:is_depth ? @"resolve_canonical_depth_cs"
                                       : @"resolve_canonical_color_cs"];
      NSError* error = nil;
      id<MTLComputePipelineState> cs =
          function ? [device newComputePipelineStateWithFunction:function
                                                           error:&error]
                   : nil;
      if (cs) {
        cs_slot = (__bridge_retained void*)cs;
      } else {
        REXLOG_ERROR("MetalRenderTargetCache: snapshot compute pipeline failed: {}",
               error ? error.localizedDescription.UTF8String : "?");
      }
    }
    if (cs_slot) {
      // The MSL parameter block is the full 27-word CanonicalResolveParams;
      // the snapshot path uses its first 20 words.
      struct {
        ResolveParams params;
        uint32_t scatter_unused[7];
      } padded{params, {}};
      const uint32_t origin[2] = {dest_x_offset * sx, dest_y_offset * sy};
      const bool direct = !is_depth && direct_color_source_;
      id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
      compute.label = direct     ? @"rtcache:direct-color-resolve-cs"
                      : is_depth ? @"rtcache:canonical-depth-resolve-cs"
                                 : @"rtcache:canonical-color-resolve-cs";
      id<MTLComputePipelineState> cs =
          (__bridge id<MTLComputePipelineState>)(
              direct ? direct_color_cs_pipelines_[direct_color_ms_ ? 1 : 0]
                     : cs_slot);
      [compute setComputePipelineState:cs];
      if (direct) {
        [compute setTexture:(__bridge id<MTLTexture>)direct_color_texture_
                    atIndex:1];
        [compute setBytes:&direct_color_rt_format_
                   length:sizeof(direct_color_rt_format_)
                  atIndex:3];
      } else {
        [compute setBuffer:(__bridge id<MTLBuffer>)canonical_edram_
                    offset:0
                   atIndex:0];
      }
      [compute setBytes:&padded length:sizeof(padded) atIndex:1];
      [compute setBytes:origin length:sizeof(origin) atIndex:2];
      [compute setTexture:destination atIndex:0];
      MTLSize threads = MTLSizeMake(clipped_width * sx, clipped_height * sy, 1);
      NSUInteger tw = cs.threadExecutionWidth;
      NSUInteger th = std::max<NSUInteger>(
          1, cs.maxTotalThreadsPerThreadgroup / tw);
      [compute dispatchThreads:threads
          threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
      [compute endEncoding];
      return true;
    }
  }
  if (!is_depth && direct_color_source_) {
    // Canonical was not packed for a direct resolve; the render pass would
    // read stale words.
    return false;
  }
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  encoder.label = is_depth ? @"rtcache:canonical-depth-resolve"
                           : @"rtcache:canonical-color-resolve";
  [encoder setRenderPipelineState:
               (__bridge id<MTLRenderPipelineState>)found->second];
  [encoder setScissorRect:MTLScissorRect{dest_x_offset * sx, dest_y_offset * sy,
                                         clipped_width * sx,
                                         clipped_height * sy}];
  [encoder setFragmentBuffer:(__bridge id<MTLBuffer>)canonical_edram_
                       offset:0
                     atIndex:0];
  [encoder setFragmentBytes:&params length:sizeof(params) atIndex:1];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::EnsureResolveForwardPipeline(void*& slot,
                                                          const char* name) {
  if (slot) {
    return true;
  }
  id<MTLLibrary> library =
      (__bridge id<MTLLibrary>)canonical_materialize_library_;
  id<MTLFunction> function =
      [library newFunctionWithName:[NSString stringWithUTF8String:name]];
  NSError* error = nil;
  id<MTLComputePipelineState> pipeline =
      function ? [(__bridge id<MTLDevice>)device_
                     newComputePipelineStateWithFunction:function
                                                   error:&error]
               : nil;
  if (!pipeline) {
    REXLOG_ERROR("MetalRenderTargetCache: resolve forward pipeline {} failed: {}",
           name, error ? error.localizedDescription.UTF8String : "?");
    return false;
  }
  slot = (__bridge_retained void*)pipeline;
  return true;
}

void MetalRenderTargetCache::BindResolveForward(void* encoder,
                                                ResolveForward& forward) {
  id<MTLComputeCommandEncoder> compute =
      (__bridge id<MTLComputeCommandEncoder>)encoder;
  const uint32_t params[5] = {forward.kind, forward.endian, forward.width,
                              forward.height, forward.origin_y};
  [compute setTexture:(__bridge id<MTLTexture>)forward.texture atIndex:2];
  [compute setBytes:params length:sizeof(params) atIndex:5];
  forward.encoded = true;
}

bool MetalRenderTargetCache::ResolveCanonicalDepthToBuffer(
    const void* command_buffer, uint32_t base_tiles, uint32_t pitch_tiles,
    uint32_t msaa_samples, uint32_t format, uint32_t source_x,
    uint32_t source_y, uint32_t sample_select, void* destination_buffer,
    uint32_t width, uint32_t height, uint32_t dest_endian,
    const CanonicalScatterDest* scatter, ResolveForward* forward) {
  if (!sample_edram_raster_enabled_ || !canonical_edram_ || !command_buffer ||
      !destination_buffer || !width || !height ||
      !EnsureCanonicalMaterializePipeline()) {
    return false;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!canonical_depth_raw_resolve_pipeline_) {
    id<MTLLibrary> library =
        (__bridge id<MTLLibrary>)canonical_materialize_library_;
    id<MTLFunction> function =
        [library newFunctionWithName:@"resolve_canonical_depth_raw"];
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
      REXLOG_ERROR(
          "MetalRenderTargetCache: canonical raw depth resolve pipeline "
          "failed: {}",
          error ? error.localizedDescription.UTF8String : "?");
      return false;
    }
    canonical_depth_raw_resolve_pipeline_ =
        (__bridge_retained void*)pipeline;
  }
  EdramSurfaceParams surface{
      base_tiles, pitch_tiles, msaa_samples,
      format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8) ? 2u : 1u,
      0u, canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct ResolveParams {
    EdramSurfaceParams surface;
    uint32_t source_x, source_y, dest_y, sample_select;
    uint32_t color_format;
    uint32_t dest_swap;
    uint32_t dest_red_only;
    float exp_bias_scale;
    uint32_t width, height;
    uint32_t dest_format, dest_bpp_log2, dest_endian, raw_copy;
    uint32_t scatter_enable;
    uint32_t dest_base, dest_pitch_aligned;
    uint32_t dest_offset_x, dest_offset_y, dest_extent_bytes;
    uint32_t dest_number;
    uint32_t dest_int_bits;
  } params{surface,
           source_x,
           source_y,
           0u,
           sample_select,
           0u,
           0u,
           0u,
           1.0f,
           width,
           height,
           0u,
           2u,
           dest_endian,
           1u,
           uint32_t(scatter ? 1u : 0u),
           scatter ? scatter->dest_base : 0u,
           scatter ? scatter->dest_pitch_aligned : 0u,
           scatter ? scatter->dest_offset_x : 0u,
           scatter ? scatter->dest_offset_y : 0u,
           scatter ? scatter->dest_extent_bytes : 0u,
           0u,
           0u};
  static_assert(sizeof(ResolveParams) == 28u * sizeof(uint32_t));
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  encoder.label = scatter ? @"rtcache:canonical-depth-raw-scatter"
                          : @"rtcache:canonical-depth-raw-resolve";
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)
          canonical_depth_raw_resolve_pipeline_;
  if (scatter && forward &&
      EnsureResolveForwardPipeline(canonical_depth_raw_fwd_pipeline_,
                                   "resolve_canonical_depth_raw_fwd")) {
    pipeline =
        (__bridge id<MTLComputePipelineState>)canonical_depth_raw_fwd_pipeline_;
    BindResolveForward((__bridge void*)encoder, *forward);
  }
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:(__bridge id<MTLBuffer>)canonical_edram_
              offset:0
             atIndex:0];
  [encoder setBuffer:(__bridge id<MTLBuffer>)destination_buffer
              offset:0
             atIndex:1];
  [encoder setBytes:&params length:sizeof(params) atIndex:2];
  MTLSize threads = MTLSizeMake(8, 8, 1);
  MTLSize groups = MTLSizeMake((width + 7u) / 8u, (height + 7u) / 8u, 1);
  [encoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::ResolveCanonicalColorToBuffer(
    const void* command_buffer, uint32_t base_tiles, uint32_t pitch_tiles,
    uint32_t msaa_samples, uint32_t format, bool is_64bpp,
    uint32_t source_x, uint32_t source_y, uint32_t sample_select,
    bool dest_swap, float exp_bias_scale, uint32_t dest_format,
    uint32_t dest_bpp_log2, uint32_t dest_endian, bool raw_copy,
    void* destination_buffer, uint32_t width, uint32_t height,
    const CanonicalScatterDest* scatter, uint32_t dest_number,
    ResolveForward* forward) {
  xenos::ColorRenderTargetFormat storage_format =
      xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(format));
  if (storage_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
    storage_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  }
  if (!sample_edram_raster_enabled_ || !canonical_edram_ || !command_buffer ||
      !destination_buffer || !width || !height || dest_bpp_log2 > 4u ||
      !IsCanonicalColorFormatSupported(storage_format) ||
      !EnsureCanonicalMaterializePipeline()) {
    return false;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!canonical_color_packed_resolve_pipeline_) {
    id<MTLLibrary> library =
        (__bridge id<MTLLibrary>)canonical_materialize_library_;
    id<MTLFunction> function =
        [library newFunctionWithName:@"resolve_canonical_color_packed"];
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
      REXLOG_ERROR(
          "MetalRenderTargetCache: canonical packed color resolve pipeline "
          "failed: {}",
          error ? error.localizedDescription.UTF8String : "?");
      return false;
    }
    canonical_color_packed_resolve_pipeline_ =
        (__bridge_retained void*)pipeline;
  }
  EdramSurfaceParams surface{
      base_tiles, pitch_tiles, msaa_samples, 0u, uint32_t(is_64bpp),
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct ResolveParams {
    EdramSurfaceParams surface;
    uint32_t source_x, source_y, dest_y, sample_select;
    uint32_t color_format;
    uint32_t dest_swap;
    uint32_t dest_red_only;
    float exp_bias_scale;
    uint32_t width, height;
    uint32_t dest_format, dest_bpp_log2, dest_endian, raw_copy;
    // Stage B tiled scatter; all zero unless the caller opts in, in which case
    // the dispatch writes guest-tiled into the shared buffer instead of
    // linearly into a staging buffer.
    uint32_t scatter_enable;
    uint32_t dest_base, dest_pitch_aligned;
    uint32_t dest_offset_x, dest_offset_y, dest_extent_bytes;
    uint32_t dest_number;
    uint32_t dest_int_bits;
  } params{surface,
           source_x,
           source_y,
           0u,
           sample_select,
           uint32_t(storage_format),
           uint32_t(dest_swap),
           uint32_t(dest_bpp_log2 == 0u),
           exp_bias_scale,
           width,
           height,
           dest_format,
           dest_bpp_log2,
           dest_endian,
           uint32_t(raw_copy),
           uint32_t(scatter ? 1u : 0u),
           scatter ? scatter->dest_base : 0u,
           scatter ? scatter->dest_pitch_aligned : 0u,
           scatter ? scatter->dest_offset_x : 0u,
           scatter ? scatter->dest_offset_y : 0u,
           scatter ? scatter->dest_extent_bytes : 0u,
           dest_number,
           0u};
  static_assert(sizeof(ResolveParams) == 28u * sizeof(uint32_t));
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  const bool direct = direct_color_source_ != nullptr;
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  encoder.label = direct ? @"rtcache:direct-color-packed-resolve"
                         : @"rtcache:canonical-color-packed-resolve";
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(
          direct ? direct_color_packed_pipelines_[direct_color_ms_ ? 1 : 0]
                 : canonical_color_packed_resolve_pipeline_);
  if (scatter && forward && (dest_bpp_log2 == 2u || dest_bpp_log2 == 3u)) {
    void*& fwd_slot =
        direct ? direct_color_packed_fwd_pipelines_[direct_color_ms_ ? 1 : 0]
               : canonical_color_packed_fwd_pipeline_;
    if (EnsureResolveForwardPipeline(
            fwd_slot,
            direct ? (direct_color_ms_ ? "resolve_direct_color_packed_fwd_ms"
                                       : "resolve_direct_color_packed_fwd_1x")
                   : "resolve_canonical_color_packed_fwd")) {
      pipeline = (__bridge id<MTLComputePipelineState>)fwd_slot;
      BindResolveForward((__bridge void*)encoder, *forward);
    }
  }
  [encoder setComputePipelineState:pipeline];
  if (direct) {
    [encoder setTexture:(__bridge id<MTLTexture>)direct_color_texture_
                atIndex:0];
    [encoder setBytes:&direct_color_rt_format_
               length:sizeof(direct_color_rt_format_)
              atIndex:3];
  } else {
    [encoder setBuffer:(__bridge id<MTLBuffer>)canonical_edram_
                offset:0
               atIndex:0];
  }
  [encoder setBuffer:(__bridge id<MTLBuffer>)destination_buffer
              offset:0
             atIndex:1];
  [encoder setBytes:&params length:sizeof(params) atIndex:2];
  MTLSize threads = MTLSizeMake(8, 8, 1);
  MTLSize groups = MTLSizeMake((width + 7u) / 8u, (height + 7u) / 8u, 1);
  [encoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::PerformCanonicalResolveClear(
    const void* command_buffer, uint32_t span_base,
    uint32_t row_length_used, uint32_t rows, uint32_t span_pitch,
    uint32_t base_tiles, uint32_t surface_pitch_tiles,
    uint32_t msaa_samples, bool is_depth, bool is_64bpp,
    uint32_t source_x, uint32_t source_y, uint32_t width, uint32_t height,
    uint32_t clear_lo, uint32_t clear_hi) {
  if (!sample_edram_raster_enabled_ || !canonical_edram_ || !command_buffer ||
      !width || !height || !row_length_used || !rows ||
      !EnsureCanonicalMaterializePipeline()) {
    return false;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!canonical_resolve_clear_pipeline_) {
    id<MTLLibrary> library =
        (__bridge id<MTLLibrary>)canonical_materialize_library_;
    id<MTLFunction> function =
        [library newFunctionWithName:@"resolve_canonical_clear"];
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
      REXLOG_ERROR("MetalRenderTargetCache: canonical resolve clear pipeline "
             "failed: {}",
             error ? error.localizedDescription.UTF8String : "?");
      return false;
    }
    canonical_resolve_clear_pipeline_ = (__bridge_retained void*)pipeline;
  }
  if (!PrepareCanonicalEdramForResolve(span_base, row_length_used, rows,
                                       span_pitch)) {
    return false;
  }
  EdramSurfaceParams surface{
      base_tiles, surface_pitch_tiles, msaa_samples, uint32_t(is_depth),
      uint32_t(!is_depth && is_64bpp),
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct ClearParams {
    EdramSurfaceParams surface;
    uint32_t source_x, source_y, width, height, clear_lo, clear_hi;
  } params{surface, source_x, source_y, width, height, clear_lo, clear_hi};
  static_assert(sizeof(ClearParams) == 12u * sizeof(uint32_t));
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  encoder.label = is_depth ? @"rtcache:canonical-resolve-clear-depth"
                           : @"rtcache:canonical-resolve-clear-color";
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)canonical_resolve_clear_pipeline_;
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:(__bridge id<MTLBuffer>)canonical_edram_
              offset:0
             atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  // The kernel clears every host pixel of the guest rectangle at the draw
  // resolution scale.
  uint32_t host_width = width * GetDrawResolutionScaleX();
  uint32_t host_height = height * GetDrawResolutionScaleY();
  MTLSize threads = MTLSizeMake(8, 8, 1);
  MTLSize groups = MTLSizeMake((host_width + 7u) / 8u,
                               (host_height + 7u) / 8u, 1u << msaa_samples);
  [encoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
  [encoder endEncoding];
  // SynchronizeCanonical intentionally keeps the attachment it packed as a
  // clean cache. The clear is a new canonical producer, so invalidate every
  // cached attachment over the exact (possibly wrapped) resolve rows. Without
  // this, reusing that attachment may skip materialization and expose its
  // pre-clear contents.
  if (MetalCommandProcessor::PassCensusEnabled()) {
    ++pack_census_.clears;
    pack_census_.clear_tiles += uint64_t(row_length_used) * rows;
    // Whole tiles: the guest rectangle starts and ends on tile edges (80x16
    // samples; 8 pixel rows per tile with MSAA), so no tile keeps pre-clear
    // samples and the pre-clear pack moved bytes the clear overwrote.
    const uint32_t tile_h = msaa_samples ? 8u : 16u;
    const uint32_t tile_w = msaa_samples >= 2 ? 40u : 80u;
    if (!(source_x % tile_w) && !(width % tile_w) && !(source_y % tile_h) &&
        !(height % tile_h)) {
      ++pack_census_.clears_whole_tiles;
    }
    for (uint32_t row = 0; row < rows; ++row) {
      for (uint32_t i = 0; i < row_length_used; ++i) {
        census_clear_tiles_[(span_base + row * span_pitch + i) &
                            (::rex::xenos::kEDRAMTileCount - 1u)] = 1;
      }
    }
  }
  {
    // Whole-tile clears leave every covered tile uniformly (clear_lo,
    // clear_hi) in canonical; a partial clear leaves mixed content.
    const uint32_t tile_h = msaa_samples ? 8u : 16u;
    const uint32_t tile_w = msaa_samples >= 2 ? 40u : 80u;
    // Whole tile rows from x = 0 reaching into the last tile column of the
    // span: the surface's own pixels are then uniform. A surface narrower
    // than its pitch (FEAR: 768 px over 10 tiles = 800) leaves the pitch
    // padding of the last column untouched by the clear; those bytes belong
    // to no pixel of this surface and are published neither by its resolves
    // nor read by its draws, so a clear load that also clears them is still
    // the surface the guest sees. A clear that starts past x = 0, skips tile
    // columns or covers partial rows leaves mixed content.
    const bool whole_tiles =
        source_x == 0 && !(source_y % tile_h) && !(height % tile_h) &&
        width + tile_w > row_length_used * tile_w;
    static const bool s_clear_log = std::getenv("XE_METAL_CLEAR_LOAD_LOG") != nullptr;
    static uint32_t s_clear_logs = 0;
    if (s_clear_log && s_clear_logs++ < 64) {
      REXLOG_WARN("MetalClearLoad clear span_base={} rows={} len={} pitch={} depth={} "
             "msaa={} rect=({},{} {}x{}) whole={} value={:08X}:{:08X}",
             span_base, rows, row_length_used, span_pitch, is_depth ? 1 : 0,
             msaa_samples, source_x, source_y, width, height, whole_tiles ? 1 : 0,
             clear_lo, clear_hi);
    }
    for (uint32_t row = 0; row < rows; ++row) {
      uint32_t start =
          (span_base + row * span_pitch) & (::rex::xenos::kEDRAMTileCount - 1u);
      uint32_t first_length =
          std::min(row_length_used, ::rex::xenos::kEDRAMTileCount - start);
      SetTileClear(start, start + first_length, whole_tiles, clear_lo,
                   clear_hi);
      if (first_length < row_length_used) {
        SetTileClear(0u, row_length_used - first_length, whole_tiles,
                     clear_lo, clear_hi);
      }
    }
  }
  for (uint32_t row = 0; row < rows; ++row) {
    uint32_t start =
        (span_base + row * span_pitch) & (::rex::xenos::kEDRAMTileCount - 1u);
    uint32_t first_length =
        std::min(row_length_used, ::rex::xenos::kEDRAMTileCount - start);
    canonical_authority_.MarkCanonical(start, start + first_length,
                                       ++canonical_authority_order_);
    if (first_length < row_length_used) {
      canonical_authority_.MarkCanonical(0u, row_length_used - first_length,
                                         ++canonical_authority_order_);
    }
  }
  return true;
}

void MetalRenderTargetCache::SetTileClear(uint32_t start_tiles,
                                          uint32_t end_tiles, bool uniform,
                                          uint32_t lo, uint32_t hi) {
  end_tiles = std::min(end_tiles, uint32_t(::rex::xenos::kEDRAMTileCount));
  for (uint32_t t = start_tiles; t < end_tiles; ++t) {
    tile_clear_[t] = TileClear{lo, hi, uint8_t(uniform ? 1u : 0u)};
  }
}

bool MetalRenderTargetCache::ClearLoadEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_CLEAR_LOAD");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

namespace {
// Mirrors xe_gamma_byte_to_linear (metal_edram_msl.h) in float, evaluated
// with the division and with a reciprocal multiply; a byte whose 16-bit
// unorm quantization differs between the two is not reproducible.
bool GammaByteTo16(uint32_t byte, uint32_t* out) {
  const float b = float(byte);
  float num, den;
  if (b < 64.0f) {
    num = b + 0.5f;
    den = 1023.0f;
  } else if (b < 96.0f) {
    num = b - 31.5f;
    den = 511.5f;
  } else if (b < 192.0f) {
    num = b - 63.5f;
    den = 255.75f;
  } else {
    num = b - 127.5f;
    den = 127.875f;
  }
  const float v1 = num / den;
  const float v2 = num * (1.0f / den);
  auto quantize = [](float v) {
    v = std::min(std::max(v, 0.0f), 1.0f);
    return uint32_t(std::lrint(double(v) * 65535.0));
  };
  const uint32_t q1 = quantize(v1), q2 = quantize(v2);
  if (q1 != q2) {
    return false;
  }
  // Also guard a half-way case where the GPU's own rounding could tip.
  const double exact = double(v1) * 65535.0;
  if (std::fabs(exact - std::floor(exact) - 0.5) < 1e-3) {
    return false;
  }
  *out = q1;
  return true;
}
// Mirrors xe_f20e4_to_f32.
float F20E4ToF32(uint32_t f24) {
  f24 &= 0xFFFFFFu;
  if (f24 == 0u) {
    return 0.0f;
  }
  uint32_t mantissa = f24 & 0xFFFFFu;
  uint32_t exponent = f24 >> 20;
  if (exponent == 0u) {
    uint32_t lz = uint32_t(__builtin_clz(mantissa)) - 11u;
    exponent = 1u - lz;
    mantissa = (mantissa << lz) & 0xFFFFFu;
  }
  const uint32_t bits = ((exponent + 112u) << 23) | (mantissa << 3);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}
}  // namespace

bool MetalRenderTargetCache::ComputeClearLoadValue(const RenderTargetKey& key,
                                                   uint32_t lo, uint32_t hi,
                                                   PendingClearLoad* out) {
  if (key.is_depth) {
    const uint32_t d24 = lo >> 8;
    float depth;
    if (key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8) {
      depth = F20E4ToF32(d24) * 0.5f;
    } else {
      // xe_unorm24_to_f32: a power-of-two scale, exact on both sides.
      depth = float(d24 + (d24 >> 23)) * (1.0f / 16777216.0f);
    }
    out->depth = double(depth);
    out->stencil = lo & 0xFFu;
    return true;
  }
  switch (key.GetColorFormat()) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
      // xe_unpack_8888 into RGBA8Unorm: byte / 255, exact.
      for (uint32_t c = 0; c < 4; ++c) {
        out->color[c] = double((lo >> (8u * c)) & 0xFFu) / 255.0;
      }
      return true;
    }
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
      // xe_unpack_color32_working_gamma into RGBA16Unorm.
      for (uint32_t c = 0; c < 3; ++c) {
        uint32_t q;
        if (!GammaByteTo16((lo >> (8u * c)) & 0xFFu, &q)) {
          return false;
        }
        out->color[c] = double(q) / 65535.0;
      }
      out->color[3] = double(lo >> 24) / 255.0;
      return true;
    }
    default:
      return false;
  }
}

bool MetalRenderTargetCache::HasPendingClearLoad(uint32_t color_index) const {
  return color_index < xenos::kMaxColorRenderTargets &&
         pending_clear_load_[color_index + 1u].valid;
}

bool MetalRenderTargetCache::TakePendingClearLoad(uint32_t color_index,
                                                  double clear_color[4]) {
  if (color_index >= xenos::kMaxColorRenderTargets) {
    return false;
  }
  PendingClearLoad& pending = pending_clear_load_[color_index + 1u];
  if (!pending.valid) {
    return false;
  }
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  if (GetCanonicalAttachmentId(used[color_index + 1u]) != pending.attachment) {
    // Not the attachment the deferral was made for: never clear it.
    return false;
  }
  for (uint32_t c = 0; c < 4; ++c) {
    clear_color[c] = pending.color[c];
  }
  for (const auto& operation : pending.operations) {
    pack_census_.clear_load_tiles += operation.end_tiles - operation.start_tiles;
  }
  ++pack_census_.clear_loads;
  pending.valid = false;
  pending.operations.clear();
  return true;
}

bool MetalRenderTargetCache::TakePendingDepthClearLoad(double* clear_depth,
                                                       uint32_t* clear_stencil) {
  PendingClearLoad& pending = pending_clear_load_[0];
  if (!pending.valid) {
    return false;
  }
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  if (GetCanonicalAttachmentId(used[0]) != pending.attachment) {
    return false;
  }
  *clear_depth = pending.depth;
  *clear_stencil = pending.stencil;
  // The pass opens on this clear: a pack since the deferral may have cached
  // the stencil it replaces.
  InvalidateMsaaStencilCache(
      static_cast<MetalRenderTarget*>(used[0])->msaa_texture(),
      kMsaaStencilClearLoad);
  for (const auto& operation : pending.operations) {
    pack_census_.clear_load_tiles += operation.end_tiles - operation.start_tiles;
  }
  ++pack_census_.clear_loads;
  pending.valid = false;
  pending.operations.clear();
  return true;
}

bool MetalRenderTargetCache::FlushPendingClearLoad(uint32_t slot,
                                                   const void* command_buffer) {
  PendingClearLoad& pending = pending_clear_load_[slot];
  if (!pending.valid) {
    return true;
  }
  pending.valid = false;
  ++pack_census_.clear_load_flushes;
  bool ok = command_buffer != nullptr;
  for (const auto& operation : pending.operations) {
    if (!ok || !EncodeCanonicalAuthorityOperation(command_buffer, operation)) {
      ok = false;
      break;
    }
  }
  pending.operations.clear();
  if (!ok) {
    REXLOG_ERROR(
        "MetalRenderTargetCache: deferred materialize for attachment {:016X} "
        "could not be encoded; forgetting authority",
        pending.attachment);
    canonical_authority_.ResetUnknown();
    ResetTileClears();
    ResetTileClears();
  }
  return ok;
}

bool MetalRenderTargetCache::FlushPendingClearLoads(const void* command_buffer,
                                                    uint32_t bound_color_mask,
                                                    bool depth_bound) {
  bool ok = true;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (pending_clear_load_[i + 1u].valid && !(bound_color_mask & (1u << i))) {
      ok = FlushPendingClearLoad(i + 1u, command_buffer) && ok;
    }
  }
  if (pending_clear_load_[0].valid && !depth_bound) {
    ok = FlushPendingClearLoad(0, command_buffer) && ok;
  }
  return ok;
}

bool MetalRenderTargetCache::PerformAttachmentColorResolveClear(
    const void* command_buffer, uint32_t span_base, uint32_t row_length_used,
    uint32_t rows, uint32_t span_pitch, uint32_t base_tiles,
    uint32_t surface_pitch_tiles, uint32_t msaa_samples, bool is_64bpp,
    uint32_t source_x, uint32_t source_y, uint32_t width, uint32_t height,
    uint32_t clear_lo, uint32_t clear_hi) {
  static const bool s_enabled =
      ParseMetalEnvironmentFlag(std::getenv("XE_METAL_ATTACHMENT_CLEAR"), true);
  if (!s_enabled || !sample_edram_raster_enabled_ || !command_buffer ||
      !width || !height || !row_length_used || !rows ||
      span_base >= ::rex::xenos::kEDRAMTileCount ||
      !EnsureCanonicalMaterializePipeline()) {
    return false;
  }
  // Engagement census, logged at powers of two.
  static uint64_t s_attempts = 0, s_taken = 0;
  ++s_attempts;
  // The canonical clear wraps at the end of EDRAM; the attachment does not.
  for (uint32_t row = 0; row < rows; ++row) {
    if (uint64_t(span_base) + uint64_t(row) * span_pitch + row_length_used >
        ::rex::xenos::kEDRAMTileCount) {
      return false;
    }
  }
  // The attachment holding the newest value of the first tile must hold the
  // newest value of every span tile, so that clearing the rectangle in it
  // leaves it the exact authority for the whole span.
  auto newest = [&](uint32_t t) -> MetalEdramAuthorityTracker::AttachmentId {
    const MetalEdramAuthorityTracker::TileState& state =
        canonical_authority_.tile(t);
    if (state.location == MetalEdramAuthorityTracker::Location::kAttachment) {
      return state.authority_attachment;
    }
    if (state.location == MetalEdramAuthorityTracker::Location::kCanonical) {
      return state.clean_attachment;
    }
    return 0;
  };
  const MetalEdramAuthorityTracker::AttachmentId attachment = newest(span_base);
  if (!attachment) {
    return false;
  }
  for (uint32_t row = 0; row < rows; ++row) {
    const uint32_t row_start = span_base + row * span_pitch;
    for (uint32_t t = row_start; t < row_start + row_length_used; ++t) {
      if (newest(t) != attachment) {
        return false;
      }
    }
  }
  RenderTarget* render_target = FindCanonicalAttachment(attachment);
  if (!render_target) {
    return false;
  }
  const RenderTargetKey& key = render_target->GetKey();
  const xenos::ColorRenderTargetFormat color_format = key.GetColorFormat();
  if (key.is_depth || key.base_tiles != base_tiles ||
      key.GetPitchTiles() != surface_pitch_tiles ||
      uint32_t(key.msaa_samples) != msaa_samples ||
      key.Is64bpp() != is_64bpp ||
      IsCanonicalColorRawViewFormat(color_format) ||
      !IsCanonicalColorFormatSupported(color_format)) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  const bool ms = key.msaa_samples != xenos::MSAASamples::k1X;
  id<MTLTexture> texture = ms ? (__bridge id<MTLTexture>)target->msaa_texture()
                              : (__bridge id<MTLTexture>)target->texture();
  if (!texture) {
    return false;
  }
  // Host pixels the canonical clear writes: the guest rectangle at the draw
  // resolution scale, in the surface's (= the attachment's) pixel space.
  const uint32_t x0 = source_x * GetDrawResolutionScaleX();
  const uint32_t y0 = source_y * GetDrawResolutionScaleY();
  const uint32_t w = width * GetDrawResolutionScaleX();
  const uint32_t h = height * GetDrawResolutionScaleY();
  if (uint64_t(x0) + w > texture.width || uint64_t(y0) + h > texture.height) {
    return false;
  }
  const uint32_t clear[4] = {clear_lo, clear_hi, uint32_t(color_format),
                             uint32_t(is_64bpp)};
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  auto mark_cleared = [&]() {
    if (MetalCommandProcessor::PassCensusEnabled()) {
      ++pack_census_.attachment_clears;
    }
    for (uint32_t row = 0; row < rows; ++row) {
      const uint32_t start = span_base + row * span_pitch;
      canonical_authority_.MarkAttachment(attachment, start,
                                          start + row_length_used,
                                          ++canonical_authority_order_);
    }
    ++s_taken;
    if ((s_taken & (s_taken - 1)) == 0) {
      REXLOG_INFO("MetalAttachmentClear: taken={} of attempts={} (latest base={} "
             "pitch={} msaa={} fmt={} {}x{})",
             s_taken, s_attempts, base_tiles, surface_pitch_tiles,
             msaa_samples, uint32_t(color_format), width, height);
    }
  };
  // Single-sample attachments: write only the rectangle from compute instead
  // of opening a render pass that loads and stores the whole (EDRAM-height)
  // attachment. XE_METAL_ATTACHMENT_CLEAR_CS=0 keeps the render pass.
  static const bool s_compute = ParseMetalEnvironmentFlag(
      std::getenv("XE_METAL_ATTACHMENT_CLEAR_CS"), true);
  if (s_compute && !ms && (texture.usage & MTLTextureUsageShaderWrite)) {
    if (!attachment_clear_cs_pipeline_) {
      id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
      id<MTLLibrary> library =
          (__bridge id<MTLLibrary>)canonical_materialize_library_;
      id<MTLFunction> function =
          [library newFunctionWithName:@"clear_color_attachment_cs"];
      NSError* error = nil;
      id<MTLComputePipelineState> pipeline =
          function ? [device newComputePipelineStateWithFunction:function
                                                           error:&error]
                   : nil;
      if (!pipeline) {
        REXLOG_ERROR("MetalRenderTargetCache: attachment clear compute pipeline "
               "failed: {}",
               error ? error.localizedDescription.UTF8String : "no function");
        return false;
      }
      attachment_clear_cs_pipeline_ = (__bridge_retained void*)pipeline;
    }
    id<MTLComputePipelineState> pipeline =
        (__bridge id<MTLComputePipelineState>)attachment_clear_cs_pipeline_;
    const uint32_t rect[4] = {x0, y0, w, h};
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    compute.label = @"rtcache:attachment-resolve-clear-color-cs";
    [compute setComputePipelineState:pipeline];
    [compute setTexture:texture atIndex:0];
    [compute setBytes:clear length:sizeof(clear) atIndex:0];
    [compute setBytes:rect length:sizeof(rect) atIndex:1];
    const NSUInteger tw = pipeline.threadExecutionWidth;
    const NSUInteger th =
        std::max<NSUInteger>(1, pipeline.maxTotalThreadsPerThreadgroup / tw);
    [compute dispatchThreads:MTLSizeMake(w, h, 1)
        threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [compute endEncoding];
    mark_cleared();
    return true;
  }
  const uint64_t pipeline_key =
      uint64_t(texture.pixelFormat) | (uint64_t(texture.sampleCount) << 32u);
  void*& pipeline_slot = attachment_clear_pipelines_[pipeline_key];
  if (!pipeline_slot) {
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    id<MTLLibrary> library =
        (__bridge id<MTLLibrary>)canonical_materialize_library_;
    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"xe_color_vs"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"clear_color_attachment_fs"];
    descriptor.colorAttachments[0].pixelFormat = texture.pixelFormat;
    descriptor.rasterSampleCount = texture.sampleCount;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) {
      REXLOG_ERROR("MetalRenderTargetCache: attachment clear pipeline failed for "
             "format {} x{}: {}",
             uint32_t(texture.pixelFormat), uint32_t(texture.sampleCount),
             error ? error.localizedDescription.UTF8String : "?");
      attachment_clear_pipelines_.erase(pipeline_key);
      return false;
    }
    pipeline_slot = (__bridge_retained void*)pipeline;
  }
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder = [command
      renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(
                                             pass)];
  encoder.label = @"rtcache:attachment-resolve-clear-color";
  [encoder setScissorRect:MTLScissorRect{x0, y0, w, h}];
  [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                      pipeline_slot];
  [encoder setFragmentBytes:clear length:sizeof(clear) atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  mark_cleared();
  return true;
}

void MetalRenderTargetCache::PerformResolveClear(
    const void* command_buffer, uint32_t span_base, uint32_t row_length_used,
    uint32_t rows, uint32_t span_pitch, bool is_depth, uint32_t clear_value) {
  InvalidateMsaaStencilCaches(kMsaaStencilResolveClear);
  // A zero-sized resolve still carries the clear bits (Forza Horizon issues
  // several 0x0 copies a frame). It clears nothing, so it must not take the
  // authority reset below: forgetting every tile's owner there made the next
  // render target materialize stale canonical EDRAM (Forza's banded sky).
  if (!command_buffer || !row_length_used || !rows) {
    return;
  }
  // In canonical mode this function is only the degraded fallback. The legacy
  // rectangle clear may touch multiple attachment owners (and represents wrap
  // in owner-local coordinates), so no single exact authority assignment can
  // be made here. Forget authority before any early return rather than letting
  // a stale clean-attachment claim survive a fallback attempt.
  if (sample_edram_raster_enabled_) {
    canonical_authority_.ResetUnknown();
    ResetTileClears();
  }
  resolve_dump_rectangles_.clear();
  GetResolveCopyRectanglesToDump(span_base, row_length_used, rows, span_pitch,
                                 resolve_dump_rectangles_);
  if (resolve_dump_rectangles_.empty()) {
    return;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!clear_color_pipeline_) {
    NSString* src = @R"METAL(
      #include <metal_stdlib>
      using namespace metal;
      struct ClearParams {
        uint dst_base; uint dst_pitch; uint start_tiles; uint end_tiles;
        uint shifts; uint pad0; uint pad1; uint pad2;
        float4 color;  // or .x = depth.
      };
      struct VOut { float4 pos [[position]]; };
      vertex VOut xe_clear_vs(uint vid [[vertex_id]]) {
        VOut o;
        float2 t = float2(float((vid << 1) & 2), float(vid & 2));
        o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
        return o;
      }
      static bool xe_clear_out_of_span(uint2 dp, constant ClearParams& p) {
        uint2 ds = uint2(dp.x << (p.shifts & 15u),
                         dp.y << ((p.shifts >> 4) & 15u));
        uint tile = p.dst_base + (ds.y / 16u) * p.dst_pitch + (ds.x / 80u);
        return tile < p.start_tiles || tile >= p.end_tiles;
      }
      fragment float4 xe_clear_color_fs(VOut in [[stage_in]],
                                        constant ClearParams& p [[buffer(0)]]) {
        if (xe_clear_out_of_span(uint2(in.pos.xy), p)) discard_fragment();
        return p.color;
      }
      struct DepthOut { float depth [[depth(any)]]; };
      fragment DepthOut xe_clear_depth_fs(VOut in [[stage_in]],
                                          constant ClearParams& p
                                          [[buffer(0)]]) {
        if (xe_clear_out_of_span(uint2(in.pos.xy), p)) discard_fragment();
        DepthOut o;
        o.depth = p.color.x;
        return o;
      }
    )METAL";
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: clear library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return;
    }
    {
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_clear_vs"];
      d.fragmentFunction = [lib newFunctionWithName:@"xe_clear_color_fs"];
      d.colorAttachments[0].pixelFormat = (MTLPixelFormat)kColorFormat;
      NSError* perr = nil;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&perr];
      if (ps) {
        clear_color_pipeline_ = (__bridge_retained void*)ps;
      }
    }
    {
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_clear_vs"];
      d.fragmentFunction = [lib newFunctionWithName:@"xe_clear_color_fs"];
      d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Unorm;
      NSError* perr = nil;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&perr];
      if (ps) {
        clear_gamma_color_pipeline_ = (__bridge_retained void*)ps;
      }
    }
    {
      MTLRenderPipelineDescriptor* d =
          [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [lib newFunctionWithName:@"xe_clear_vs"];
      d.fragmentFunction = [lib newFunctionWithName:@"xe_clear_depth_fs"];
      d.depthAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
      d.stencilAttachmentPixelFormat = (MTLPixelFormat)kDepthFormat;
      NSError* perr = nil;
      id<MTLRenderPipelineState> ps =
          [device newRenderPipelineStateWithDescriptor:d error:&perr];
      if (ps) {
        clear_depth_pipeline_ = (__bridge_retained void*)ps;
      }
      MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
      ds.depthCompareFunction = MTLCompareFunctionAlways;
      ds.depthWriteEnabled = YES;
      // The guest clear dword is depth24<<8 | stencil: the same rect clear
      // also REPLACEs stencil with the encoder's reference (clear & 0xFF).
      MTLStencilDescriptor* sten = [[MTLStencilDescriptor alloc] init];
      sten.stencilCompareFunction = MTLCompareFunctionAlways;
      sten.depthStencilPassOperation = MTLStencilOperationReplace;
      sten.stencilFailureOperation = MTLStencilOperationReplace;
      sten.depthFailureOperation = MTLStencilOperationReplace;
      sten.writeMask = 0xFF;
      ds.frontFaceStencil = sten;
      ds.backFaceStencil = sten;
      clear_depth_state_ =
          (__bridge_retained void*)[device
              newDepthStencilStateWithDescriptor:ds];
    }
  }
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  for (const ResolveCopyDumpRectangle& rect : resolve_dump_rectangles_) {
    auto* owner = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!owner || owner->GetKey().is_depth != uint32_t(is_depth)) {
      continue;
    }
    uint32_t oxs, oys;
    GetMsaaShifts(uint32_t(owner->GetKey().msaa_samples), oxs, oys);
    struct ClearParams {
      uint32_t dst_base, dst_pitch, start_tiles, end_tiles;
      uint32_t shifts, pad0, pad1, pad2;
      float color[4];
    } params{owner->GetKey().base_tiles,
             owner->GetKey().GetPitchTiles(),
             span_base,
             span_base + rows * span_pitch,
             oxs | (oys << 4),
             0,
             0,
             0,
             {0, 0, 0, 0}};
    if (is_depth) {
      if (!clear_depth_pipeline_ || !clear_depth_state_) {
        return;
      }
      // Raw EDRAM depth word: depth24 in the high bits (unorm24; float24
      // clears are typically 0 or max, both map fine through unorm here).
      uint32_t clear_d24 = clear_value >> 8;
      // Canonical unorm24 decode (see XeUNorm24To32).
      params.color[0] =
          float(clear_d24 + (clear_d24 >> 23)) * (1.0f / 16777216.0f);
      MTLRenderPassDescriptor* pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.depthAttachment.texture = (__bridge id<MTLTexture>)owner->texture();
      pass.depthAttachment.loadAction = MTLLoadActionLoad;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.stencilAttachment.texture =
          (__bridge id<MTLTexture>)owner->texture();
      pass.stencilAttachment.loadAction = MTLLoadActionLoad;
      pass.stencilAttachment.storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> enc =
          [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
      [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                      clear_depth_pipeline_];
      enc.label = @"rtcache:resolve-clear-depth";
      [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)
                                    clear_depth_state_];
      [enc setStencilReferenceValue:clear_value & 0xFF];
      [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [enc endEncoding];
    } else {
      const bool gamma_owner =
          owner->pixel_format() == uint64_t(MTLPixelFormatRGBA16Unorm) &&
          owner->GetKey().GetColorFormat() ==
              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
      void* clear_pipeline = gamma_owner ? clear_gamma_color_pipeline_
                                         : clear_color_pipeline_;
      if (!clear_pipeline ||
          (!gamma_owner && owner->pixel_format() != kColorFormat)) {
        continue;
      }
      // Raw 8888 EDRAM word -> canonical logical channels (guest byte0 = .r,
      // RGBA8 storage == guest bytes; matches d2c/c2d/c2f/f2c and the dump).
      params.color[0] = float(clear_value & 0xFF) / 255.0f;          // R
      params.color[1] = float((clear_value >> 8) & 0xFF) / 255.0f;   // G
      params.color[2] = float((clear_value >> 16) & 0xFF) / 255.0f;  // B
      params.color[3] = float(clear_value >> 24) / 255.0f;           // A
      if (gamma_owner) {
        // Match canonical materialization's byte-bin midpoint so RGBA16Unorm
        // quantization followed by canonical PWL packing reproduces the clear
        // dword bit-exactly.
        for (uint32_t channel = 0; channel < 3; ++channel) {
          const float byte = float((clear_value >> (channel * 8u)) & 0xFFu);
          params.color[channel] =
              byte < 64.0f    ? (byte + 0.5f) / 1023.0f
              : byte < 96.0f  ? (byte - 31.5f) / 511.5f
              : byte < 192.0f ? (byte - 63.5f) / 255.75f
                               : (byte - 127.5f) / 127.875f;
        }
      }
      MTLRenderPassDescriptor* pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture =
          (__bridge id<MTLTexture>)owner->texture();
      pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> enc =
          [cb renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
      [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                      clear_pipeline];
      enc.label = @"rtcache:resolve-clear-color";
      [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [enc endEncoding];
    }
    static uint32_t s_logs = 0;
    if (s_logs++ < 12) {
      REXLOG_INFO("MetalRenderTargetCache: resolve clear {} = {:08X} into {}",
             is_depth ? "depth" : "color", clear_value,
             owner->GetKey().GetDebugName());
    }
  }
}

void* MetalRenderTargetCache::EnsureResolveSnapshot(uint32_t dest_base,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint64_t pixel_format) {
  // Some Xenos resolve destinations (for example 4:4:4:4) have no compatible
  // Metal render-target container in the snapshot bridge. Canonical writeback
  // still packs them exactly into guest RAM; fail closed here instead of
  // passing MTLPixelFormatInvalid to Metal and aborting the process.
  if (pixel_format == uint64_t(MTLPixelFormatInvalid)) {
    static uint32_t s_unsupported_snapshot_logs = 0;
    if (s_unsupported_snapshot_logs++ < 8) {
      REXLOG_WARN(
          "MetalRenderTargetCache: no snapshot view for resolve {:08X} "
          "{}x{}; using canonical guest-RAM writeback",
          dest_base, width, height);
    }
    return nullptr;
  }
  // Scene-scale resolves (depth AND color, e.g. the 720p albedo) arrive as
  // vertical strips of ONE 720-row buffer sampled full-height from the first
  // strip's base; allocate full-height so the later strip lands inside
  // (FindContainingSnapshot).
  if (width >= 1280 && height < 720) {
    height = 720;
  }
  const uint64_t key =
      (uint64_t(dest_base) << 1) |
      (pixel_format == uint64_t(MTLPixelFormatR32Float) ? 1 : 0);
  // Other widths: the height later strips were seen to continue to.
  auto strips = snapshot_strip_heights_.find(key);
  if (strips != snapshot_strip_heights_.end() &&
      strips->second.first == width && strips->second.second > height) {
    height = strips->second.second;
  }
  return EnsureSnapshotTexture(key, width, height, pixel_format);
}

namespace {
// Guest bytes per texel of a resolve snapshot's rows in guest memory: the
// 64 bpp destinations (16_16_16_16 and its float form) are 8, everything
// else a snapshot is made for is 4.
uint32_t SnapshotGuestBytesPerTexel(uint64_t pixel_format) {
  switch (MTLPixelFormat(pixel_format)) {
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
      return 8;
    default:
      return 4;
  }
}
}  // namespace

void MetalRenderTargetCache::NoteSnapshotStripContinuation(
    uint32_t dest_base, uint32_t width, uint32_t height,
    uint64_t pixel_format) {
  // Default on; XE_METAL_SNAPSHOT_STRIPS=0 keeps every strip its own snapshot.
  static const bool s_enabled =
      ParseMetalEnvironmentFlag(std::getenv("XE_METAL_SNAPSHOT_STRIPS"), true);
  if (!s_enabled) {
    return;
  }
  const uint64_t format_bit =
      pixel_format == uint64_t(MTLPixelFormatR32Float) ? 1 : 0;
  for (const auto& pair : resolve_snapshots_) {
    const Snapshot& snap = pair.second;
    if ((pair.first & kAssemblySnapshotKeyFlag) ||
        (pair.first & 1) != format_bit || !snap.texture ||
        snap.pixel_format != pixel_format || snap.width != width) {
      continue;
    }
    // The same row layout FindContainingSnapshot assumes. The strip
    // starts a whole number of rows into the snapshot, at its end or inside
    // rows a 1280-wide snapshot was only padded to (720): Burnout Paradise's
    // three 1280x640 shadow cascades, sampled as one 1280x1920 texture,
    // put the second at row 640 of the first's 720-row snapshot, where it
    // did not fit and became its own snapshot; the fetch saw 720 rows.
    const uint32_t snap_base = uint32_t(pair.first >> 1);
    const uint64_t row_bytes =
        uint64_t(snap.width) * SnapshotGuestBytesPerTexel(snap.pixel_format);
    if (dest_base <= snap_base || (dest_base - snap_base) % row_bytes) {
      continue;
    }
    const uint64_t row = (dest_base - snap_base) / row_bytes;
    if (row > snap.height || row + height <= snap.height) {
      continue;
    }
    const uint32_t full_height = uint32_t(row) + height;
    if (full_height > 2048) {
      return;
    }
    auto& strips = snapshot_strip_heights_[pair.first];
    if (strips.first != width || strips.second < full_height) {
      strips = {width, full_height};
      REXLOG_INFO(
          "MetalResolve: strip {:08X} {}x{} continues the {}x{} snapshot at "
          "{:08X}; its next resolve allocates {} rows",
          dest_base, width, height, snap.width, snap.height, snap_base,
          full_height);
    }
    return;
  }
}

void* MetalRenderTargetCache::EnsureAssemblySnapshot(uint32_t base,
                                                     uint32_t width,
                                                     uint32_t height,
                                                     uint64_t pixel_format) {
  if (pixel_format == uint64_t(MTLPixelFormatInvalid)) {
    return nullptr;
  }
  return EnsureSnapshotTexture(
      kAssemblySnapshotKeyFlag | (uint64_t(base) << 1) |
          (pixel_format == uint64_t(MTLPixelFormatR32Float) ? 1 : 0),
      width, height, pixel_format);
}

bool MetalRenderTargetCache::BeginDirectColorResolve(
    uint32_t base_tiles, uint32_t pitch_tiles, uint32_t msaa_samples,
    uint32_t format, bool is_64bpp, uint32_t span_base,
    uint32_t span_row_length, uint32_t span_rows, uint32_t span_pitch,
    uint32_t source_x, uint32_t source_y, uint32_t width, uint32_t height,
    uint32_t sample_select, const void* snapshot) {
  direct_color_source_ = nullptr;
  direct_color_texture_ = nullptr;
  // Default on (reduce-edram-pass-traffic Lever 4);
  // XE_METAL_DIRECT_COLOR_RESOLVE=0 restores the pack-first resolve.
  static const bool s_enabled = ParseMetalEnvironmentFlag(
      std::getenv("XE_METAL_DIRECT_COLOR_RESOLVE"), true);
  // Engagement census, logged at the first 16 and every 1024 attempts.
  static uint64_t s_attempts = 0, s_counts[6] = {};
  auto note = [&](uint32_t reason) {
    ++s_counts[reason];
    if ((++s_attempts & 1023) == 0 || s_attempts <= 16) {
      REXLOG_INFO("MetalDirectColorResolve: attempts={} taken={} not-eligible={} "
             "no-attachment={} out-of-texture={} tracker={} pipeline={}",
             s_attempts, s_counts[0], s_counts[1], s_counts[2], s_counts[3],
             s_counts[4], s_counts[5]);
    }
  };
  if (!s_enabled) {
    return false;
  }
  // Draw resolution scaling: the direct kernels read the representative host
  // pixel of each guest pixel (xe_resolve_host_pixel, and every host pixel of
  // it for snapshots) exactly as the canonical ones do, so only the bounds
  // below change. XE_METAL_DIRECT_COLOR_RESOLVE_SCALED=0 keeps scaled resolves
  // on the pack-first path.
  static const bool s_scaled = ParseMetalEnvironmentFlag(
      std::getenv("XE_METAL_DIRECT_COLOR_RESOLVE_SCALED"), true);
  const bool scaled =
      GetDrawResolutionScaleX() != 1 || GetDrawResolutionScaleY() != 1;
  if (!sample_edram_raster_enabled_ || !canonical_edram_ || is_64bpp ||
      (scaled && !s_scaled) ||
      !span_row_length || !span_rows || !width || !height ||
      (snapshot && !(((__bridge id<MTLTexture>)snapshot).usage &
                     MTLTextureUsageShaderWrite)) ||
      IsCanonicalColorRawViewFormat(xenos::ColorRenderTargetFormat(format)) ||
      !EnsureCanonicalMaterializePipeline()) {
    note(1);
    return false;
  }
  // The canonical path packs with the attachment's own format and reads the
  // stored words as the resolve's storage format (8888 gamma stores 8888
  // bytes), so formats match by storage format.
  auto storage_of = [](uint32_t f) {
    xenos::ColorRenderTargetFormat storage =
        xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(f));
    return storage == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA
               ? xenos::ColorRenderTargetFormat::k_8_8_8_8
               : storage;
  };
  RenderTarget* render_target = nullptr;
  RenderTarget* const* used = GetLastUpdateUsedRenderTargets();
  for (uint32_t slot = 1; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    RenderTarget* candidate = used[slot];
    if (!candidate) {
      continue;
    }
    const RenderTargetKey& key = candidate->GetKey();
    if (!key.is_depth && key.base_tiles == base_tiles &&
        key.GetPitchTiles() == pitch_tiles &&
        uint32_t(key.msaa_samples) == msaa_samples &&
        storage_of(uint32_t(key.GetColorFormat())) == storage_of(format) &&
        !IsCanonicalColorRawViewFormat(key.GetColorFormat()) &&
        !key.Is64bpp()) {
      render_target = candidate;
      break;
    }
  }
  if (!render_target ||
      !IsCanonicalColorFormatSupported(
          render_target->GetKey().GetColorFormat())) {
    static uint32_t s_mismatch_logs = 0;
    if (s_mismatch_logs++ < 12) {
      std::string bound;
      for (uint32_t slot = 1; slot <= xenos::kMaxColorRenderTargets; ++slot) {
        if (used[slot]) {
          const RenderTargetKey& k = used[slot]->GetKey();
          bound += fmt::format(" [{} base={} pitch={} msaa={} fmt={}]", slot,
                               k.base_tiles, k.GetPitchTiles(),
                               uint32_t(k.msaa_samples),
                               uint32_t(k.GetColorFormat()));
        }
      }
      REXLOG_INFO("MetalDirectColorResolve: no attachment for base={} pitch={} "
             "msaa={} fmt={}; bound:{}",
             base_tiles, pitch_tiles, msaa_samples, format, bound);
    }
    note(2);
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  const bool ms = msaa_samples != uint32_t(xenos::MSAASamples::k1X);
  id<MTLTexture> texture =
      ms ? (__bridge id<MTLTexture>)target->msaa_texture()
         : (__bridge id<MTLTexture>)target->texture();
  if (!texture ||
      (ms && target->sample_count() != 2u && target->sample_count() != 4u)) {
    note(2);
    return false;
  }
  // Every pixel the resolve's samples read: at 1x (x and y) and 2x (x) a
  // sample index carrying the sub-offset steps to the next pixel.
  uint32_t sample_mask;
  switch (sample_select) {
    case 1:
    case 2:
    case 3:
      sample_mask = 1u << sample_select;
      break;
    case 4:
      sample_mask = 0b0011;
      break;
    case 5:
      sample_mask = 0b1100;
      break;
    case 6:
      sample_mask = 0b1111;
      break;
    default:
      sample_mask = 0b0001;
      break;
  }
  const uint32_t x_shift =
      msaa_samples >= uint32_t(xenos::MSAASamples::k4X) ? 1u : 0u;
  const uint32_t y_shift =
      msaa_samples >= uint32_t(xenos::MSAASamples::k2X) ? 1u : 0u;
  uint32_t x_extra = 0, y_extra = 0;
  for (uint32_t sample = 0; sample < 4; ++sample) {
    if (!(sample_mask & (1u << sample))) {
      continue;
    }
    if (!x_shift) {
      x_extra = std::max(x_extra, (sample >> 1) & 1u);
    }
    if (!y_shift) {
      y_extra = std::max(y_extra, sample & 1u);
    }
  }
  // Host pixels: guest pixels times the draw resolution scale.
  if ((uint64_t(source_x) + width) * GetDrawResolutionScaleX() + x_extra >
          texture.width ||
      (uint64_t(source_y) + height) * GetDrawResolutionScaleY() + y_extra >
          texture.height) {
    note(3);
    return false;
  }
  const MetalEdramAuthorityTracker::AttachmentId attachment =
      GetCanonicalAttachmentId(render_target);
  for (uint32_t row = 0; row < span_rows; ++row) {
    const uint32_t row_start = span_base + row * span_pitch;
    for (uint32_t t = row_start; t < row_start + span_row_length; ++t) {
      if (t >= ::rex::xenos::kEDRAMTileCount) {
        note(4);
        return false;
      }
      const MetalEdramAuthorityTracker::TileState& state =
          canonical_authority_.tile(t);
      const bool attachment_newest =
          (state.location ==
               MetalEdramAuthorityTracker::Location::kAttachment &&
           state.authority_attachment == attachment) ||
          (state.location ==
               MetalEdramAuthorityTracker::Location::kCanonical &&
           state.clean_attachment == attachment);
      if (!attachment_newest) {
        note(4);
        return false;
      }
    }
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLLibrary> library =
      (__bridge id<MTLLibrary>)canonical_materialize_library_;
  auto ensure = [&](void*& slot, NSString* name) {
    if (slot) {
      return true;
    }
    id<MTLFunction> function = [library newFunctionWithName:name];
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline =
        function
            ? [device newComputePipelineStateWithFunction:function error:&error]
            : nil;
    if (!pipeline) {
      REXLOG_ERROR("MetalRenderTargetCache: direct colour resolve pipeline {} "
             "failed: {}",
             name.UTF8String,
             error ? error.localizedDescription.UTF8String : "?");
      return false;
    }
    slot = (__bridge_retained void*)pipeline;
    return true;
  };
  if (!ensure(direct_color_cs_pipelines_[ms ? 1 : 0],
              ms ? @"resolve_direct_color_cs_ms"
                 : @"resolve_direct_color_cs_1x") ||
      !ensure(direct_color_packed_pipelines_[ms ? 1 : 0],
              ms ? @"resolve_direct_color_packed_ms"
                 : @"resolve_direct_color_packed_1x")) {
    note(5);
    return false;
  }
  direct_color_source_ = target;
  direct_color_texture_ = (__bridge void*)texture;
  direct_color_ms_ = ms;
  direct_color_rt_format_ = uint32_t(render_target->GetKey().GetColorFormat());
  if (std::getenv("XE_METAL_CANONICAL_SYNC_LOG")) {
    REXLOG_WARN("MetalCanonicalSync direct-resolve fmt={} msaa={} base={} pitch={} "
           "span=[{},+{}x{} step {}] src=({},{}) {}x{} (canonical not packed)",
           direct_color_rt_format_, msaa_samples, base_tiles, pitch_tiles,
           span_base, span_row_length, span_rows, span_pitch, source_x,
           source_y, width, height);
  }
  note(0);
  return true;
}

// Resolve snapshots are written by a compute dispatch over the resolve
// rectangle (snapshot textures gain ShaderWrite) instead of a render pass
// that loads and stores the whole snapshot texture per resolve (Red Dead
// gameplay 27.2 -> 28.05 fps; 66 resolves/frame). Exact: corpus 40/40 0 px.
// XE_METAL_SNAPSHOT_COMPUTE=0 restores the render pass.
bool MetalRenderTargetCache::SnapshotComputeEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("XE_METAL_SNAPSHOT_COMPUTE");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return enabled;
}

void* MetalRenderTargetCache::EnsureSnapshotTexture(uint64_t snap_key,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint64_t pixel_format) {
  // Key by (base, format): JC2 resolves color (BGRA8) and depth (R32F) to
  // the same guest base — a single-slot map destroyed/recreated the snapshot
  // EVERY resolve, and each new texture burned a bindless view slot until
  // the heap ran dry a few minutes into gameplay (black samples, then a
  // frozen present).
  auto it = resolve_snapshots_.find(snap_key);
  // EXACT-size reuse (2026-09-17, DOA5). The snapshot is served to fetches
  // that sample it with NORMALIZED coordinates, so its texture dimensions
  // are part of what the consumer sees: a 1024x608 resolve reusing a
  // 1280x720 snapshot ("large enough") placed the scene in the top-left 80%
  // x 84% of every fetch with stale generations around it — DOA5's fight
  // rendered inside a hard-edged box, and its 640x720 character-select
  // panel squeezed into the left half of the panel. A size change means a
  // new texture; the old one retires through the deferred list below.
  // XE_METAL_SNAPSHOT_LOOSE_REUSE=1 restores the "large enough" rule (A/B).
  static const bool s_loose_reuse = [] {
    const char* v = std::getenv("XE_METAL_SNAPSHOT_LOOSE_REUSE");
    return v && v[0] == '1';
  }();
  if (it != resolve_snapshots_.end() && it->second.texture &&
      it->second.pixel_format == pixel_format &&
      (s_loose_reuse ? (it->second.width >= width &&
                        it->second.height >= height)
                     : (it->second.width == width &&
                        it->second.height == height))) {
    return it->second.texture;
  }
  if (it != resolve_snapshots_.end() && it->second.texture) {
    // Do NOT free the old texture now: draws already encoded into the open frame
    // command buffer still reference it by bindless gpuAddress, and freeing it
    // mid-frame GPU-page-faults when they execute (the "GPU froze" hang). Retire
    // it to the deferred list; the command processor frees it after commit+wait.
    deferred_texture_releases_.emplace_back(
        MetalCommandProcessor::S7TagsOff()
            ? 0
            : command_processor_.CurrentEncodingSubmission(),
        it->second.texture);
    it->second.texture = nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
  // 2D array so the texture cache (ForceTextureArray reads) can sample it.
  desc.textureType = MTLTextureType2DArray;
  desc.pixelFormat = (MTLPixelFormat)pixel_format;
  // At the draw resolution scale; the record below keeps guest dimensions,
  // which the serving rules compare.
  desc.width = width * GetDrawResolutionScaleX();
  desc.height = height * GetDrawResolutionScaleY();
  desc.arrayLength = 1;
  desc.mipmapLevelCount = 1;
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  if (SnapshotComputeEnabled()) {
    desc.usage |= MTLTextureUsageShaderWrite;
  }
  desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> texture = nil;
  if (snapshot_texture_allocator_) {
    texture = (__bridge_transfer id<MTLTexture>)snapshot_texture_allocator_(
        (__bridge void*)desc);
  }
  if (!texture) {
    texture = [device newTextureWithDescriptor:desc];
  }
  if (!texture) {
    REXLOG_ERROR("MetalRenderTargetCache: failed to create {}x{} resolve snapshot",
           width, height);
    return nullptr;
  }
  ClearTextureAtCreation((__bridge void*)texture, /*is_depth=*/false);
  resolve_snapshots_[snap_key] =
      Snapshot{(__bridge_retained void*)texture, width, height, pixel_format};
  return resolve_snapshots_[snap_key].texture;
}

void MetalRenderTargetCache::DrainDeferredReleases(
    uint64_t completed_submission) {
  // UINT64_MAX is the quiesced teardown/clear drain: nothing is in flight and
  // the texture cache's bridges are retired by those paths themselves.
  bool guarded = deferred_release_guard_ && completed_submission != UINT64_MAX;
  std::vector<void*> held;
  auto it = std::remove_if(
      deferred_texture_releases_.begin(), deferred_texture_releases_.end(),
      [&](const std::pair<uint64_t, void*>& entry) {
        if (entry.first > completed_submission) {
          return false;
        }
        if (guarded && deferred_release_guard_(entry.second)) {
          held.push_back(entry.second);
          return true;
        }
        id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)entry.second;
        (void)released;
        return true;
      });
  deferred_texture_releases_.erase(it, deferred_texture_releases_.end());
  if (!held.empty()) {
    uint64_t submission = MetalCommandProcessor::S7TagsOff()
                              ? 0
                              : command_processor_.CurrentEncodingSubmission();
    static uint32_t s_held_logs = 0;
    if (s_held_logs++ < 8) {
      REXLOG_WARN("MetalRenderTargetCache: {} replaced snapshot(s) still named by a "
             "texture cache bridge at release; bridge retired, release held "
             "to submission {}",
             held.size(), submission);
    }
    for (void* texture : held) {
      deferred_texture_releases_.emplace_back(submission, texture);
    }
  }
}

uint32_t MetalRenderTargetCache::ReleaseResolveSnapshotsForBases(
    const std::vector<std::pair<uint32_t, void*>>& bases,
    std::vector<uint32_t>* released_bases_out) {
  uint32_t released = 0;
  for (const auto& entry : bases) {
    uint32_t base = entry.first;
    void* expected_texture = entry.second;
    // Color snapshots only: key is (base << 1) | is_r32f, and the depth/R32F
    // companion at the same base belongs to a bridge that was retained.
    auto it = resolve_snapshots_.find(uint64_t(base) << 1);
    if (it == resolve_snapshots_.end()) {
      continue;
    }
    // Identity check: the snapshot at this base may have been REPLACED since
    // the bridge was registered (EnsureResolveSnapshot creates a new texture
    // when a resolve needs a bigger one). Releasing then would destroy a newer
    // object the bridge never named and other consumers may still hold.
    if (!expected_texture || it->second.texture != expected_texture) {
      static uint32_t s_mismatch_logs = 0;
      if (s_mismatch_logs++ < 8) {
        REXLOG_WARN("MetalRenderTargetCache: cache clear skipped resolve snapshot "
               "{:08X} -- the bridge named a different texture than the one now "
               "at that base (snapshot replaced since registration); the whole "
               "current generation at this base is retained",
               base);
      }
      continue;  // NOT reported as released: the newer generation stays whole.
    }
    if (it->second.texture) {
      // Immediate release: the caller runs only at the quiesced clear
      // boundary, where no submission can still be reading this texture.
      id<MTLTexture> snapshot = (__bridge_transfer id<MTLTexture>)it->second.texture;
      (void)snapshot;
    }
    resolve_snapshots_.erase(it);
    if (released_bases_out) {
      released_bases_out->push_back(base);
    }
    ++released;
  }
  return released;
}

void MetalRenderTargetCache::LogResolveSnapshotHashes(void* queue,
                                                      uint32_t swap_index) {
  if (!queue || resolve_snapshots_.empty()) {
    return;
  }
  id<MTLCommandQueue> command_queue = (__bridge id<MTLCommandQueue>)queue;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  // Deterministic emission order so per-loop logs diff line-for-line.
  std::vector<uint64_t> keys;
  keys.reserve(resolve_snapshots_.size());
  for (const auto& pair : resolve_snapshots_) {
    if (pair.second.texture) {
      keys.push_back(pair.first);
    }
  }
  std::sort(keys.begin(), keys.end());
  for (uint64_t key : keys) {
    const Snapshot& snap = resolve_snapshots_[key];
    id<MTLTexture> texture = (__bridge id<MTLTexture>)snap.texture;
    uint32_t bytes_per_pixel = 0;
    switch (texture.pixelFormat) {
      case MTLPixelFormatBGRA8Unorm:
      case MTLPixelFormatRGBA8Unorm:
      case MTLPixelFormatR32Float:
      case MTLPixelFormatRG16Float:
      case MTLPixelFormatRG16Unorm:
      case MTLPixelFormatRGB10A2Unorm:
        bytes_per_pixel = 4;
        break;
      case MTLPixelFormatRGBA16Float:
      case MTLPixelFormatRGBA16Unorm:
      case MTLPixelFormatRG32Float:
        bytes_per_pixel = 8;
        break;
      default:
        REXLOG_WARN("MetalSnapHash swap={} base={:08X} kind={} fmt={} UNHASHED",
               swap_index, uint32_t(key >> 1), (key & 1) ? "r32f" : "color",
               uint64_t(texture.pixelFormat));
        continue;
    }
    size_t width = texture.width;
    size_t height = texture.height;
    size_t row_bytes = width * bytes_per_pixel;
    size_t total_bytes = row_bytes * height;
    id<MTLBuffer> readback =
        [device newBufferWithLength:total_bytes
                            options:MTLResourceStorageModeShared];
    if (!readback) {
      REXLOG_WARN("MetalSnapHash swap={} base={:08X} readback alloc failed",
             swap_index, uint32_t(key >> 1));
      continue;
    }
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    blit.label = @"rtcache:snaphash";
    [blit copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:row_bytes
        destinationBytesPerImage:total_bytes];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const uint8_t* bytes = static_cast<const uint8_t*>(readback.contents);
    uint64_t hash = 14695981039346656037ull;  // FNV-1a 64.
    for (size_t i = 0; i < total_bytes; ++i) {
      hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    REXLOG_INFO("MetalSnapHash swap={} base={:08X} kind={} {}x{} fmt={} hash={:016X}",
           swap_index, uint32_t(key >> 1), (key & 1) ? "r32f" : "color",
           uint32_t(width), uint32_t(height), uint64_t(texture.pixelFormat),
           hash);
  }
}

uint32_t MetalRenderTargetCache::ReleaseAllResolveSnapshots() {
  // See the header. Immediate (not deferred) release: the replay-reset seam
  // runs only where every submission has completed, the same argument that
  // makes ReleaseResolveSnapshotsForBases' immediate release legal.
  uint32_t released = 0;
  for (auto& pair : resolve_snapshots_) {
    if (pair.second.texture) {
      id<MTLTexture> snapshot =
          (__bridge_transfer id<MTLTexture>)pair.second.texture;
      (void)snapshot;
      pair.second.texture = nullptr;
      ++released;
    }
  }
  resolve_snapshots_.clear();
  return released;
}

void MetalRenderTargetCache::ResetForReplayRestore() {
  // See the header. Order: destroy first (retirements land in the deferred
  // list), drain last -- the same shape Shutdown uses.
  d2c_lineage_.InvalidateAll();
  presentable_color_target_ = nullptr;  // borrowed from a render target.
  presentable_color_rt_ = nullptr;
  // Destroys every pool render target regardless of EDRAM ownership --
  // ownership is re-seeded from scratch by the imminent restore
  // (PrepareFullEdram1280xRenderTargetForSnapshotRestoration) -- and re-seeds
  // the empty full-range ownership entry.
  DestroyAllRenderTargets();
  // last_update_* attachment pointers may name just-destroyed targets; force
  // the next Update to rebuild instead of reusing them.
  ResetAccumulatedRenderTargets();
  DrainDeferredReleases(UINT64_MAX);
}

void MetalRenderTargetCache::ClearCache() {
  // Derived state keyed by render-target identity, dropped BEFORE the shared
  // clear can destroy the targets it names. Both are same-frame/per-present
  // records, so dropping them costs nothing but a recomputation.
  d2c_lineage_.InvalidateAll();
  presentable_color_target_ = nullptr;  // borrowed from a render target.
  presentable_color_rt_ = nullptr;
  // Deferred releases are held until the submissions encoded against them
  // complete. The clear runs only at a fully quiesced boundary, so every one
  // of them is now releasable, and leaving them queued against submissions
  // that will never be revisited is exactly the retire-list inconsistency the
  // clear must not leave behind.
  DrainDeferredReleases(UINT64_MAX);
  // Keeps every render target that currently owns EDRAM data; destroys the
  // rest. Canonical EDRAM, its pipelines, and the resolve snapshots are not
  // touched by it and are retained by design.
  ::rex::gpu::RenderTargetCache::ClearCache();
}

void* MetalRenderTargetCache::GetResolveSnapshot(uint32_t dest_base) const {
  // Composite key: base<<1 | is_r32f. Callers (present, RT-as-texture
  // bridge) want the COLOR snapshot; fall back to depth only if that's all
  // there is.
  auto it = resolve_snapshots_.find(uint64_t(dest_base) << 1);
  if (it != resolve_snapshots_.end() && it->second.texture) {
    return it->second.texture;
  }
  it = resolve_snapshots_.find((uint64_t(dest_base) << 1) | 1);
  return it != resolve_snapshots_.end() ? it->second.texture : nullptr;
}

void* MetalRenderTargetCache::FindContainingSnapshot(
    uint32_t dest_base, uint64_t pixel_format, uint32_t* y_offset_out) const {
  static const bool s_match_format = ParseMetalEnvironmentFlag(
      std::getenv("XE_METAL_STRIP_PARENT_FORMAT"), true);
  for (const auto& pair : resolve_snapshots_) {
    const Snapshot& snap = pair.second;
    if (pair.first & kAssemblySnapshotKeyFlag) {
      // Column assemblies are not vertical-strip parents.
      continue;
    }
    if (s_match_format && snap.pixel_format != pixel_format) {
      continue;
    }
    uint32_t snap_base = uint32_t(pair.first >> 1);
    if (!snap.texture || dest_base <= snap_base) {
      continue;
    }
    // Rows are 8 guest bytes per texel for the 64 bpp formats. With 4
    // assumed for those, Gears of War's 1280x208 RGBA16F strip (the bottom
    // of its 1280x720 scene copy, written right after the 1280x512 one) was
    // not found inside its parent, became its own snapshot, and a 2x fetch
    // of the whole copy showed stale rows below 512.
    uint32_t row_bytes =
        snap.width * SnapshotGuestBytesPerTexel(snap.pixel_format);
    uint32_t extent = row_bytes * snap.height;
    uint32_t offset = dest_base - snap_base;
    if (offset < extent && (offset % row_bytes) == 0) {
      if (y_offset_out) {
        *y_offset_out = offset / row_bytes;
      }
      return snap.texture;
    }
  }
  return nullptr;
}

void MetalRenderTargetCache::GetEdramOwnersForDump(
    std::vector<EdramOwnerDump>& out) {
  out.clear();
  // The whole EDRAM as one 16-tile-pitch span (matching the snapshot-restore
  // layout and the offline decoder's expectations).
  resolve_dump_rectangles_.clear();
  GetResolveCopyRectanglesToDump(0, 16, ::rex::xenos::kEDRAMTileCount / 16, 16,
                                 resolve_dump_rectangles_);
  for (const ResolveCopyDumpRectangle& rect : resolve_dump_rectangles_) {
    auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!rt) {
      continue;
    }
    EdramOwnerDump dump;
    dump.texture = rt->texture();
    dump.base_tiles = rt->GetKey().base_tiles;
    dump.pitch_tiles = rt->GetKey().GetPitchTiles();
    dump.msaa_samples = uint32_t(rt->GetKey().msaa_samples);
    dump.is_depth = rt->GetKey().is_depth;
    dump.is_float24_depth =
        rt->GetKey().is_depth &&
        rt->GetKey().GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8;
    dump.pixel_format = rt->pixel_format();
    dump.is_64bpp = uint32_t(rt->GetKey().Is64bpp());
    dump.row_first = rect.row_first;
    dump.rows = rect.rows;
    out.push_back(dump);
  }
}

uint32_t MetalRenderTargetCache::GetTraceDrawIndexForOwnershipTrace() const {
  return command_processor_.trace_draw_index();
}

bool MetalRenderTargetCache::EnsureCanonicalMaterializePipeline() {
  if (canonical_materialize_pipeline_ && canonical_pack_pipeline_ &&
      canonical_materialize64_pipeline_ && canonical_pack64_pipeline_ &&
      canonical_materialize_raw_pipeline_ && canonical_pack_raw_pipeline_ &&
      canonical_materialize64_raw_pipeline_ &&
      canonical_pack64_raw_pipeline_) {
    return true;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!canonical_materialize_library_) {
    // Compile the same shared source used by the Metal conformance harness.
    // This keeps live color materialize/pack, addressing, depth formats, and
    // stencil word helpers under one source contract.
    NSString* src = [NSString stringWithUTF8String:kEdramMsl];
    NSError* err = nil;
    id<MTLLibrary> lib =
        [device newLibraryWithSource:src
                             options:EdramMslCompileOptions(
                                         edram_tile_width_samples(),
                                         edram_tile_height_samples())
                               error:&err];
    if (!lib) {
      REXLOG_ERROR("MetalRenderTargetCache: canonical materialize library failed: {}",
             err ? err.localizedDescription.UTF8String : "?");
      return false;
    }
    canonical_materialize_library_ = (__bridge_retained void*)lib;
  }
  id<MTLLibrary> lib = (__bridge id<MTLLibrary>)canonical_materialize_library_;
  NSError* err = nil;
  id<MTLComputePipelineState> pso = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"materialize_color_range"]
                                    error:&err];
  id<MTLComputePipelineState> pack = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"pack_color_range"]
                                    error:&err];
  id<MTLComputePipelineState> pso64 = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"materialize_color64_range"]
                                    error:&err];
  id<MTLComputePipelineState> pack64 = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"pack_color64_range"]
                                    error:&err];
  id<MTLComputePipelineState> pso_raw = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"materialize_color_raw_range"]
                                    error:&err];
  id<MTLComputePipelineState> pack_raw = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"pack_color_raw_range"]
                                    error:&err];
  id<MTLComputePipelineState> pso64_raw = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"materialize_color64_raw_range"]
                                    error:&err];
  id<MTLComputePipelineState> pack64_raw = [device
      newComputePipelineStateWithFunction:
          [lib newFunctionWithName:@"pack_color64_raw_range"]
                                    error:&err];
  if (!pso || !pack || !pso64 || !pack64 || !pso_raw || !pack_raw ||
      !pso64_raw || !pack64_raw) {
    REXLOG_ERROR("MetalRenderTargetCache: canonical materialize/pack pipeline "
           "failed: {}",
           err ? err.localizedDescription.UTF8String : "?");
    return false;
  }
  canonical_materialize_pipeline_ = (__bridge_retained void*)pso;
  canonical_pack_pipeline_ = (__bridge_retained void*)pack;
  canonical_materialize64_pipeline_ = (__bridge_retained void*)pso64;
  canonical_pack64_pipeline_ = (__bridge_retained void*)pack64;
  canonical_materialize_raw_pipeline_ = (__bridge_retained void*)pso_raw;
  canonical_pack_raw_pipeline_ = (__bridge_retained void*)pack_raw;
  canonical_materialize64_raw_pipeline_ =
      (__bridge_retained void*)pso64_raw;
  canonical_pack64_raw_pipeline_ = (__bridge_retained void*)pack64_raw;
  return true;
}

void* MetalRenderTargetCache::EnsureCanonicalColorMsaaMaterializePipeline(
    MetalRenderTarget* render_target) {
  if (!render_target || !render_target->msaa_texture() ||
      (render_target->sample_count() != 2u &&
       render_target->sample_count() != 4u) ||
      !EnsureCanonicalMaterializePipeline()) {
    return nullptr;
  }
  const xenos::ColorRenderTargetFormat format =
      render_target->GetKey().GetColorFormat();
  const bool raw_view = IsCanonicalColorRawViewFormat(format);
  const bool is_64bpp = render_target->GetKey().Is64bpp();
  const MTLPixelFormat sync_pixel_format = CanonicalColorSyncPixelFormat(
      format, (MTLPixelFormat)render_target->pixel_format());
  const uint64_t key =
      uint64_t(sync_pixel_format) |
      (uint64_t(render_target->sample_count()) << 32u) |
      (uint64_t(raw_view) << 62u) | (uint64_t(is_64bpp) << 63u);
  auto found = canonical_color_msaa_materialize_pipelines_.find(key);
  if (found != canonical_color_msaa_materialize_pipelines_.end()) {
    return found->second;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLLibrary> library =
      (__bridge id<MTLLibrary>)canonical_materialize_library_;
  NSError* error = nil;
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:@"xe_color_vs"];
  NSString* materialize_name =
      raw_view ? (is_64bpp ? @"materialize_color64_raw_ms_range_fs"
                           : @"materialize_color_raw_ms_range_fs")
               : (is_64bpp ? @"materialize_color64_ms_range_fs"
                            : @"materialize_color_ms_range_fs");
  descriptor.fragmentFunction =
      [library newFunctionWithName:materialize_name];
  descriptor.colorAttachments[0].pixelFormat = sync_pixel_format;
  descriptor.rasterSampleCount = render_target->sample_count();
  id<MTLRenderPipelineState> materialize =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!materialize) {
    REXLOG_ERROR(
        "MetalRenderTargetCache: {}x canonical color MSAA materialize "
        "pipeline failed for format {}: {}",
        render_target->sample_count(), uint32_t(sync_pixel_format),
        error ? error.localizedDescription.UTF8String : "?");
    return nullptr;
  }
  void*& pack_pipeline =
      raw_view
          ? (is_64bpp ? canonical_color64_raw_msaa_pack_pipeline_
                      : canonical_color_raw_msaa_pack_pipeline_)
          : (is_64bpp ? canonical_color64_msaa_pack_pipeline_
                      : canonical_color_msaa_pack_pipeline_);
  if (!pack_pipeline) {
    NSString* pack_name = raw_view
                              ? (is_64bpp ? @"pack_color64_raw_ms_range"
                                          : @"pack_color_raw_ms_range")
                              : (is_64bpp ? @"pack_color64_ms_range"
                                          : @"pack_color_ms_range");
    if (MsaaPackPerPixel()) {
      pack_name = [pack_name stringByAppendingString:@"_px"];
    }
    id<MTLComputePipelineState> pack = [device
        newComputePipelineStateWithFunction:[library
                                                newFunctionWithName:pack_name]
                                      error:&error];
    if (!pack) {
      REXLOG_ERROR("MetalRenderTargetCache: canonical color MSAA pack pipeline "
             "failed: {}",
             error ? error.localizedDescription.UTF8String : "?");
      return nullptr;
    }
    pack_pipeline = (__bridge_retained void*)pack;
  }
  void* retained = (__bridge_retained void*)materialize;
  canonical_color_msaa_materialize_pipelines_.emplace(key, retained);
  return retained;
}

bool MetalRenderTargetCache::EncodeCanonicalColorMaterialize(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples != xenos::MSAASamples::k1X ||
      !EnsureCanonicalMaterializePipeline()) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  const xenos::ColorRenderTargetFormat color_format =
      target->GetKey().GetColorFormat();
  uint32_t format = uint32_t(color_format);
  if (!IsCanonicalColorFormatSupported(color_format)) {
    return false;
  }
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> texture = GetCanonicalColorSyncTexture(
      (__bridge id<MTLTexture>)target->texture(), color_format);
  if (!texture) {
    return false;
  }
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  const bool raw_view = IsCanonicalColorRawViewFormat(color_format);
  const bool is_64bpp = target->GetKey().Is64bpp();
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(
          raw_view ? (is_64bpp ? canonical_materialize64_raw_pipeline_
                               : canonical_materialize_raw_pipeline_)
                   : (is_64bpp ? canonical_materialize64_pipeline_
                               : canonical_materialize_pipeline_));
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), 0u,
      uint32_t(target->GetKey().Is64bpp()),
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
  encoder.label = raw_view
                      ? (is_64bpp
                             ? @"rtcache:canonical-color64-raw-materialize-range"
                             : @"rtcache:canonical-color-raw-materialize-range")
                      : (is_64bpp
                             ? @"rtcache:canonical-color64-materialize-range"
                             : @"rtcache:canonical-color-materialize-range");
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:canonical offset:0 atIndex:0];
  [encoder setTexture:texture atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  [encoder setBytes:&format length:sizeof(format) atIndex:2];
  [encoder setBytes:&range length:sizeof(range) atIndex:3];
  NSUInteger tw = pipeline.threadExecutionWidth;
  NSUInteger th = pipeline.maxTotalThreadsPerThreadgroup / tw;
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const uint32_t row_start = uint32_t(GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY()));
  [encoder setBytes:&row_start length:sizeof(row_start) atIndex:7];
  [encoder dispatchThreads:MTLSizeMake(texture.width, work_height - row_start, 1)
      threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::EncodeCanonicalColorPack(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples != xenos::MSAASamples::k1X ||
      !EnsureCanonicalMaterializePipeline()) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  const xenos::ColorRenderTargetFormat color_format =
      target->GetKey().GetColorFormat();
  uint32_t format = uint32_t(color_format);
  if (!IsCanonicalColorFormatSupported(color_format)) {
    return false;
  }
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> texture = GetCanonicalColorSyncTexture(
      (__bridge id<MTLTexture>)target->texture(), color_format);
  if (!texture) {
    return false;
  }
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  const bool raw_view = IsCanonicalColorRawViewFormat(color_format);
  const bool is_64bpp = target->GetKey().Is64bpp();
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(
          raw_view ? (is_64bpp ? canonical_pack64_raw_pipeline_
                               : canonical_pack_raw_pipeline_)
                   : (is_64bpp ? canonical_pack64_pipeline_
                               : canonical_pack_pipeline_));
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), 0u,
      uint32_t(target->GetKey().Is64bpp()),
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
  encoder.label = raw_view
                      ? (is_64bpp
                             ? @"rtcache:canonical-color64-raw-pack-range"
                             : @"rtcache:canonical-color-raw-pack-range")
                      : (is_64bpp ? @"rtcache:canonical-color64-pack-range"
                                  : @"rtcache:canonical-color-pack-range");
  [encoder setComputePipelineState:pipeline];
  [encoder setTexture:texture atIndex:0];
  [encoder setBuffer:canonical offset:0 atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  [encoder setBytes:&format length:sizeof(format) atIndex:2];
  [encoder setBytes:&range length:sizeof(range) atIndex:3];
  NSUInteger tw = pipeline.threadExecutionWidth;
  NSUInteger th = pipeline.maxTotalThreadsPerThreadgroup / tw;
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const uint32_t row_start = uint32_t(GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY()));
  [encoder setBytes:&row_start length:sizeof(row_start) atIndex:7];
  [encoder dispatchThreads:MTLSizeMake(texture.width, work_height - row_start, 1)
      threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::EncodeCanonicalColorMsaaMaterialize(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples == xenos::MSAASamples::k1X) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  const xenos::ColorRenderTargetFormat color_format =
      target->GetKey().GetColorFormat();
  uint32_t format = uint32_t(color_format);
  if (!IsCanonicalColorFormatSupported(color_format)) {
    return false;
  }
  void* pipeline_opaque =
      EnsureCanonicalColorMsaaMaterializePipeline(target);
  if (!pipeline_opaque) {
    return false;
  }
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> texture = GetCanonicalColorSyncTexture(
      (__bridge id<MTLTexture>)target->msaa_texture(), color_format);
  if (!texture) {
    return false;
  }
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  id<MTLRenderPipelineState> pipeline =
      (__bridge id<MTLRenderPipelineState>)pipeline_opaque;
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), 0u,
      uint32_t(target->GetKey().Is64bpp()),
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:metal::MetalApplyXenosSamplePositions(pass)];
  const bool raw_view = IsCanonicalColorRawViewFormat(color_format);
  const bool is_64bpp = target->GetKey().Is64bpp();
  encoder.label =
      raw_view
          ? (is_64bpp
                 ? @"rtcache:canonical-color64-raw-materialize-msaa-range"
                 : @"rtcache:canonical-color-raw-materialize-msaa-range")
          : (is_64bpp
                 ? @"rtcache:canonical-color64-materialize-msaa-range"
                 : @"rtcache:canonical-color-materialize-msaa-range");
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const NSUInteger row_start = GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY());
  [encoder setScissorRect:MTLScissorRect{0, row_start, texture.width,
                                         work_height - row_start}];
  [encoder setRenderPipelineState:pipeline];
  [encoder setFragmentBuffer:canonical offset:0 atIndex:0];
  [encoder setFragmentBytes:&params length:sizeof(params) atIndex:1];
  [encoder setFragmentBytes:&format length:sizeof(format) atIndex:2];
  [encoder setFragmentBytes:&range length:sizeof(range) atIndex:3];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  return true;
}

bool MetalRenderTargetCache::EncodeCanonicalColorMsaaPack(
    const void* command_buffer, RenderTarget* render_target,
    uint32_t start_tiles, uint32_t end_tiles) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_buffer ||
      !render_target || render_target->GetKey().is_depth ||
      render_target->GetKey().msaa_samples == xenos::MSAASamples::k1X) {
    return false;
  }
  auto* target = static_cast<MetalRenderTarget*>(render_target);
  const xenos::ColorRenderTargetFormat color_format =
      target->GetKey().GetColorFormat();
  uint32_t format = uint32_t(color_format);
  if (!IsCanonicalColorFormatSupported(color_format) ||
      !EnsureCanonicalColorMsaaMaterializePipeline(target)) {
    return false;
  }
  id<MTLCommandBuffer> command =
      (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> texture = GetCanonicalColorSyncTexture(
      (__bridge id<MTLTexture>)target->msaa_texture(), color_format);
  if (!texture) {
    return false;
  }
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  const bool raw_view = IsCanonicalColorRawViewFormat(color_format);
  const bool is_64bpp = target->GetKey().Is64bpp();
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(
          raw_view
              ? (is_64bpp ? canonical_color64_raw_msaa_pack_pipeline_
                          : canonical_color_raw_msaa_pack_pipeline_)
              : (is_64bpp ? canonical_color64_msaa_pack_pipeline_
                          : canonical_color_msaa_pack_pipeline_));
  EdramSurfaceParams params{
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), 0u,
      uint32_t(target->GetKey().Is64bpp()),
      canonical_edram_size() / uint32_t(sizeof(uint32_t))};
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } range{start_tiles, end_tiles};
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  encoder.label = raw_view
                      ? (is_64bpp
                             ? @"rtcache:canonical-color64-raw-pack-msaa-range"
                             : @"rtcache:canonical-color-raw-pack-msaa-range")
                      : (is_64bpp
                             ? @"rtcache:canonical-color64-pack-msaa-range"
                             : @"rtcache:canonical-color-pack-msaa-range");
  [encoder setComputePipelineState:pipeline];
  [encoder setTexture:texture atIndex:0];
  [encoder setBuffer:canonical offset:0 atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  [encoder setBytes:&format length:sizeof(format) atIndex:2];
  [encoder setBytes:&range length:sizeof(range) atIndex:3];
  NSUInteger thread_width = pipeline.threadExecutionWidth;
  NSUInteger thread_height =
      pipeline.maxTotalThreadsPerThreadgroup / thread_width;
  NSUInteger work_height = GetCanonicalRangeTextureHeight(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, end_tiles,
      texture.height, GetDrawResolutionScaleY());
  if (!work_height) {
    [encoder endEncoding];
    return false;
  }
  const uint32_t row_start = uint32_t(GetCanonicalRangeTextureRowStart(
      target->GetKey().base_tiles, target->GetKey().GetPitchTiles(),
      uint32_t(target->GetKey().msaa_samples), start_tiles, work_height,
      texture.height, GetDrawResolutionScaleY()));
  [encoder setBytes:&row_start length:sizeof(row_start) atIndex:7];
  [encoder dispatchThreads:MTLSizeMake(texture.width, work_height - row_start,
                                       MsaaPackPerPixel() ? 1
                                                          : texture.sampleCount)
      threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
  [encoder endEncoding];
  return true;
}

void MetalRenderTargetCache::ExpandEdramSnapshotToScale(
    const uint32_t* snapshot, uint32_t* scaled) const {
  const uint32_t sx = GetDrawResolutionScaleX(), sy = GetDrawResolutionScaleY();
  const uint32_t tile_w = ::rex::xenos::kEDRAMTileWidthSamples;
  const uint32_t tile_h = ::rex::xenos::kEDRAMTileHeightSamples;
  const uint32_t scaled_tile_w = tile_w * sx;
  const size_t tile_words = size_t(tile_w) * tile_h;
  const size_t scaled_tile_words = tile_words * sx * sy;
  for (uint32_t tile = 0; tile < ::rex::xenos::kEDRAMTileCount; ++tile) {
    const uint32_t* src = snapshot + tile * tile_words;
    uint32_t* dst = scaled + tile * scaled_tile_words;
    for (uint32_t y = 0; y < tile_h; ++y) {
      for (uint32_t x = 0; x < tile_w; ++x) {
        uint32_t word = src[y * tile_w + x];
        for (uint32_t j = 0; j < sy; ++j) {
          uint32_t* row = dst + size_t(y * sy + j) * scaled_tile_w + x * sx;
          for (uint32_t i = 0; i < sx; ++i) {
            row[i] = word;
          }
        }
      }
    }
  }
}

void MetalRenderTargetCache::CompactScaledEdramToSnapshot(
    const uint32_t* scaled, uint32_t* snapshot) const {
  const uint32_t sx = GetDrawResolutionScaleX(), sy = GetDrawResolutionScaleY();
  const uint32_t tile_w = ::rex::xenos::kEDRAMTileWidthSamples;
  const uint32_t tile_h = ::rex::xenos::kEDRAMTileHeightSamples;
  const uint32_t scaled_tile_w = tile_w * sx;
  const size_t tile_words = size_t(tile_w) * tile_h;
  const size_t scaled_tile_words = tile_words * sx * sy;
  for (uint32_t tile = 0; tile < ::rex::xenos::kEDRAMTileCount; ++tile) {
    const uint32_t* src = scaled + tile * scaled_tile_words;
    uint32_t* dst = snapshot + tile * tile_words;
    for (uint32_t y = 0; y < tile_h; ++y) {
      const uint32_t* row = src + size_t(y * sy + (sy >> 1)) * scaled_tile_w;
      for (uint32_t x = 0; x < tile_w; ++x) {
        dst[y * tile_w + x] = row[x * sx + (sx >> 1)];
      }
    }
  }
}

bool MetalRenderTargetCache::RestoreCanonicalEdram(const void* snapshot) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !snapshot) {
    return false;
  }
  @autoreleasepool {
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
    id<MTLBuffer> staging = nil;
    if (IsDrawResolutionScaled()) {
      // The snapshot is 1x. Each of its words is replicated into its sx x sy
      // block of the scaled tile, as raw single-sample words: exact for
      // single-sample and depth surfaces (the depth column swap commutes with
      // the replication), approximate for MSAA and 64bpp history, whose scaled
      // sample positions depend on the format that later reads them.
      staging = [device newBufferWithLength:canonical_edram_size()
                                    options:MTLResourceStorageModeShared];
      if (staging) {
        ExpandEdramSnapshotToScale(static_cast<const uint32_t*>(snapshot),
                                   static_cast<uint32_t*>(staging.contents));
      }
    } else {
      staging = [device newBufferWithBytes:snapshot
                                    length:kCanonicalEdramSize
                                   options:MTLResourceStorageModeShared];
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!staging || !queue) {
      REXLOG_ERROR("MetalRenderTargetCache: canonical EDRAM restore allocation failed");
      return false;
    }
    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    blit.label = @"rtcache:canonical-restore";
    [blit copyFromBuffer:staging
             sourceOffset:0
                 toBuffer:canonical
        destinationOffset:0
                     size:canonical_edram_size()];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error) {
      REXLOG_ERROR("MetalRenderTargetCache: canonical EDRAM restore failed: {}",
             command_buffer.error.localizedDescription.UTF8String);
      return false;
    }
  }
  canonical_authority_.MarkCanonical(0, ::rex::xenos::kEDRAMTileCount,
                                     ++canonical_authority_order_);
  census_clear_tiles_.fill(0);
  ResetTileClears();
  return true;
}

bool MetalRenderTargetCache::DumpCanonicalEdram(
    void* command_queue, std::vector<uint8_t>& edram_out) {
  if (!sample_edram_enabled_ || !canonical_edram_ || !command_queue) {
    return false;
  }
  @autoreleasepool {
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue;
    id<MTLBuffer> staging =
        [device newBufferWithLength:canonical_edram_size()
                            options:MTLResourceStorageModeShared];
    if (!staging) {
      REXLOG_ERROR("MetalRenderTargetCache: canonical EDRAM dump allocation failed");
      return false;
    }
    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    blit.label = @"rtcache:canonical-dump";
    [blit copyFromBuffer:canonical
            sourceOffset:0
                toBuffer:staging
       destinationOffset:0
                    size:canonical_edram_size()];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.error) {
      REXLOG_ERROR("MetalRenderTargetCache: canonical EDRAM dump failed: {}",
             command_buffer.error.localizedDescription.UTF8String);
      return false;
    }
    edram_out.resize(kCanonicalEdramSize);
    if (IsDrawResolutionScaled()) {
      // The dump keeps the 1x layout: each word is the representative word of
      // its sx x sy block, so restoring a dump and dumping again round-trips.
      CompactScaledEdramToSnapshot(
          static_cast<const uint32_t*>(staging.contents),
          reinterpret_cast<uint32_t*>(edram_out.data()));
    } else {
      std::memcpy(edram_out.data(), staging.contents, kCanonicalEdramSize);
    }
  }
  return true;
}

void MetalRenderTargetCache::RunCanonicalRestoreSelfCheck(
    const void* snapshot, const void* reordered_reference, uint32_t width,
    uint32_t height, uint32_t row_bytes) {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLCommandQueue> queue = [device newCommandQueue];
  std::vector<uint8_t> canonical_dump;
  if (!queue || !DumpCanonicalEdram((__bridge void*)queue, canonical_dump)) {
    REXLOG_ERROR("MetalRenderTargetCache: canonical restore->dump self-check failed");
    return;
  }
  if (canonical_dump.size() != kCanonicalEdramSize ||
      std::memcmp(canonical_dump.data(), snapshot, kCanonicalEdramSize) != 0) {
    size_t first_bad = 0;
    const uint8_t* expected = static_cast<const uint8_t*>(snapshot);
    while (first_bad < canonical_dump.size() &&
           canonical_dump[first_bad] == expected[first_bad]) {
      ++first_bad;
    }
    REXLOG_ERROR(
        "MetalRenderTargetCache: canonical restore->dump FAIL at byte {}",
        first_bad);
    return;
  }
  REXLOG_INFO(
      "MetalRenderTargetCache: canonical restore->dump PASS ({} bytes "
      "byte-exact)",
      canonical_dump.size());
  if (!EnsureCanonicalMaterializePipeline()) {
    return;
  }
  id<MTLBuffer> canonical = (__bridge id<MTLBuffer>)canonical_edram_;
  id<MTLComputePipelineState> pso =
      (__bridge id<MTLComputePipelineState>)canonical_materialize_pipeline_;
  // Scratch host attachment: RGBA8Unorm, shared so we can read it back and
  // compare. tiles-per-row = width/80 gives the pitch in tiles (16 for 1280).
  MTLTextureDescriptor* td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];
  td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  id<MTLTexture> scratch = [device newTextureWithDescriptor:td];
  if (!scratch) {
    return;
  }
  EdramSurfaceParams sp{0u, width / ::rex::xenos::kEDRAMTileWidthSamples, 0u, 0u,
                        0u, kCanonicalEdramSize / 4u};
  uint32_t color_format =
      uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  struct RangeParams {
    uint32_t start_tiles, end_tiles;
  } full_range{0u, ::rex::xenos::kEDRAMTileCount};
  id<MTLBuffer> sp_buf = [device newBufferWithBytes:&sp
                                            length:sizeof(sp)
                                           options:MTLResourceStorageModeShared];
  // Second canonical buffer for the pack (materialize -> pack) leg; shared so
  // the round-trip result can be compared against the snapshot on the CPU.
  id<MTLBuffer> canonical2 =
      [device newBufferWithLength:kCanonicalEdramSize
                          options:MTLResourceStorageModeShared];
  id<MTLComputePipelineState> pack_pso =
      (__bridge id<MTLComputePipelineState>)canonical_pack_pipeline_;
  if (canonical2) {
    std::memset(canonical2.contents, 0, kCanonicalEdramSize);
  }
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  NSUInteger tw = pso.threadExecutionWidth;
  NSUInteger th = pso.maxTotalThreadsPerThreadgroup / tw;
  {
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    enc.label = @"rtcache:canonical-materialize-selfcheck";
    [enc setComputePipelineState:pso];
    [enc setBuffer:canonical offset:0 atIndex:0];
    [enc setTexture:scratch atIndex:0];
    [enc setBuffer:sp_buf offset:0 atIndex:1];
    [enc setBytes:&color_format length:sizeof(color_format) atIndex:2];
    [enc setBytes:&full_range length:sizeof(full_range) atIndex:3];
    const uint32_t row_start = 0;
    [enc setBytes:&row_start length:sizeof(row_start) atIndex:7];
    [enc dispatchThreads:MTLSizeMake(width, height, 1)
        threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [enc endEncoding];
  }
  if (canonical2 && pack_pso) {
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    enc.label = @"rtcache:canonical-pack-selfcheck";
    [enc setComputePipelineState:pack_pso];
    [enc setTexture:scratch atIndex:0];
    [enc setBuffer:canonical2 offset:0 atIndex:0];
    [enc setBuffer:sp_buf offset:0 atIndex:1];
    [enc setBytes:&color_format length:sizeof(color_format) atIndex:2];
    [enc setBytes:&full_range length:sizeof(full_range) atIndex:3];
    const uint32_t row_start = 0;
    [enc setBytes:&row_start length:sizeof(row_start) atIndex:7];
    [enc dispatchThreads:MTLSizeMake(width, height, 1)
        threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [enc endEncoding];
  }
  [cb commit];
  [cb waitUntilCompleted];
  if (cb.error) {
    REXLOG_WARN("MetalRenderTargetCache: canonical self-check GPU error: {}",
           cb.error.localizedDescription.UTF8String);
    return;
  }
  // Pack leg: materialize -> pack must reproduce the original snapshot words for
  // every pixel the surface covers (offsets in [0, covered)); proves the pack
  // direction (host attachment -> canonical) on real trace data.
  if (canonical2) {
    const uint32_t* got_words = static_cast<const uint32_t*>(canonical2.contents);
    const uint32_t* snap_words = static_cast<const uint32_t*>(snapshot);
    uint64_t pack_bad = 0;
    size_t pack_first = 0;
    uint32_t tiles_per_row = width / ::rex::xenos::kEDRAMTileWidthSamples;
    uint32_t covered_tiles =
        tiles_per_row * (height / ::rex::xenos::kEDRAMTileHeightSamples);
    size_t covered_words =
        size_t(covered_tiles) * ::rex::xenos::kEDRAMTileWidthSamples *
        ::rex::xenos::kEDRAMTileHeightSamples;
    for (size_t i = 0; i < covered_words; ++i) {
      if (got_words[i] != snap_words[i]) {
        if (pack_bad == 0) pack_first = i;
        ++pack_bad;
      }
    }
    if (pack_bad == 0) {
      REXLOG_INFO(
          "MetalRenderTargetCache: canonical materialize+pack round-trip PASS "
          "({} words == snapshot)",
          covered_words);
    } else {
      REXLOG_ERROR(
          "MetalRenderTargetCache: canonical materialize+pack round-trip FAIL: "
          "{} / {} words differ, first at word {}",
          pack_bad, covered_words, pack_first);
    }
  }
  // Compare the materialized scratch bytes against the legacy CPU reorder. Both
  // are raw snapshot bytes per pixel, so byte-equality is the exact invariant.
  std::unique_ptr<uint8_t[]> got(new uint8_t[size_t(row_bytes) * height]);
  [scratch getBytes:got.get()
        bytesPerRow:row_bytes
         fromRegion:MTLRegionMake2D(0, 0, width, height)
        mipmapLevel:0];
  const uint8_t* ref = static_cast<const uint8_t*>(reordered_reference);
  size_t total = size_t(row_bytes) * height;
  size_t first_bad = total;
  uint64_t bad_count = 0;
  for (size_t i = 0; i < total; ++i) {
    if (got[i] != ref[i]) {
      if (first_bad == total) first_bad = i;
      ++bad_count;
    }
  }
  if (bad_count == 0) {
    REXLOG_INFO(
        "MetalRenderTargetCache: canonical materialize self-check PASS "
        "({}x{}, {} bytes byte-exact vs legacy reorder)",
        width, height, total);
  } else {
    REXLOG_ERROR(
        "MetalRenderTargetCache: canonical materialize self-check FAIL: {} / {} "
        "bytes differ, first at byte {} (px {},{})",
        bad_count, total, first_bad, (first_bad / 4) % width,
        (first_bad / 4) / width);
  }
}

void MetalRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  // A snapshot restore replaces EDRAM content wholesale: no prior lend can
  // prove a return against it.
  d2c_lineage_.InvalidateAll();
  // Full-EDRAM 1280-pitch k_8_8_8_8 render target owning all tiles.
  RenderTarget* full_rt = PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
      xenos::ColorRenderTargetFormat::k_8_8_8_8);
  if (!full_rt) {
    REXLOG_WARN("MetalRenderTargetCache: EDRAM snapshot restore RT unavailable");
    return;
  }
  auto* rt = static_cast<MetalRenderTarget*>(full_rt);
  // The restore target owns, and is uploaded, every tile.
  if (!GrowRenderTarget(rt, GetFullRenderTargetTextureHeight(rt->GetKey()))) {
    REXLOG_WARN("MetalRenderTargetCache: EDRAM snapshot restore RT unavailable");
    return;
  }
  id<MTLTexture> texture = (__bridge id<MTLTexture>)rt->texture();
  if (IsDrawResolutionScaled()) {
    // The canonical restore below marks every tile canonical, so the host
    // render target is rematerialized from it before use; its 1x upload here
    // would be both superseded and laid out for a 1280-wide texture.
    if (sample_edram_enabled_ && canonical_edram_ &&
        RestoreCanonicalEdram(snapshot)) {
      REXLOG_INFO("MetalRenderTargetCache: EDRAM snapshot restored into the scaled "
             "canonical EDRAM");
    } else {
      REXLOG_WARN("MetalRenderTargetCache: EDRAM snapshot restore at a draw "
             "resolution scale requires canonical EDRAM");
    }
    return;
  }
  {
    // Diagnostic: sample the snapshot content (is it real data or a fill
    // pattern?). Words from the depth region (tiles ~16+) and a simple sum.
    const uint32_t* words = static_cast<const uint32_t*>(snapshot);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 4096; ++i) {
      sum += words[i * 512];
    }
    REXLOG_INFO(
        "MetalRenderTargetCache: EDRAM snapshot sample w[0]={:08X} "
        "w[20K]={:08X} w[500K]={:08X} w[2M]={:08X} sum16M={:016X}",
        words[0], words[20000], words[500000], words[2000000], sum);
  }
  // Reorder the tile-major snapshot (2048 tiles of 80x16 32bpp samples) into
  // the 1280-wide (16 tiles per row) row-major layout.
  uint32_t width = uint32_t(texture.width);    // 1280
  uint32_t height = uint32_t(texture.height);  // covers 2048 tiles
  const uint32_t tile_w = ::rex::xenos::kEDRAMTileWidthSamples;    // 80
  const uint32_t tile_h = ::rex::xenos::kEDRAMTileHeightSamples;   // 16
  uint32_t tiles_per_row = width / tile_w;                  // 16
  uint32_t row_bytes = width * 4;
  std::unique_ptr<uint8_t[]> reordered(new uint8_t[size_t(row_bytes) * height]);
  std::memset(reordered.get(), 0, size_t(row_bytes) * height);
  const uint8_t* src = static_cast<const uint8_t*>(snapshot);
  uint32_t tile_count = ::rex::xenos::kEDRAMTileCount;
  for (uint32_t tile = 0; tile < tile_count; ++tile) {
    uint32_t tx = (tile % tiles_per_row) * tile_w;
    uint32_t ty = (tile / tiles_per_row) * tile_h;
    if (ty + tile_h > height) {
      break;
    }
    const uint8_t* tile_src = src + size_t(tile) * tile_w * tile_h * 4;
    for (uint32_t row = 0; row < tile_h; ++row) {
      std::memcpy(reordered.get() + size_t(ty + row) * row_bytes + tx * 4,
                  tile_src + size_t(row) * tile_w * 4, tile_w * 4);
    }
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLBuffer> staging =
      [device newBufferWithBytes:reordered.get()
                          length:size_t(row_bytes) * height
                         options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)
      command_processor_.PrepareTransferCommandBuffer(uint32_t(MetalTransferSite::kSnapshotRestore));
  if (!staging || !cb) {
    return;
  }
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  blit.label = @"rtcache:edram-restore";
  [blit copyFromBuffer:staging
             sourceOffset:0
        sourceBytesPerRow:row_bytes
      sourceBytesPerImage:size_t(row_bytes) * height
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:texture
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  REXLOG_INFO("MetalRenderTargetCache: EDRAM snapshot restored into {}",
         full_rt->GetKey().GetDebugName());
  // Restoring canonical authority is part of the active path. The expensive
  // full-buffer proof remains available as an explicit diagnostic, but must not
  // be paid on every normal replay or live-title launch.
  if (sample_edram_enabled_ && canonical_edram_) {
    if (RestoreCanonicalEdram(snapshot) &&
        canonical_restore_self_check_enabled_) {
      RunCanonicalRestoreSelfCheck(snapshot, reordered.get(), width, height,
                                   row_bytes);
    }
  }
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
namespace rex::graphics::metal {

int32_t MetalRenderTargetCache::GetWindowOffsetTiles(
    const RegisterFile& regs, xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, uint64_t frame) {
  // The guest's window offset reaches PsParamGen through the viewport unless a draw keeps the transform across frames; the
  // port's resolve and fold paths take the viewport route, so the EDRAM base needs no offset. Returning zero is the value the
  // fork used for every draw outside its cached-transform window.
  (void)regs;
  (void)normalized_depth_control;
  (void)normalized_color_mask;
  (void)frame;
  return 0;
}

}  // namespace rex::graphics::metal
