/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: Full render target creation + resolve pipeline
 */

#import <Metal/Metal.h>

#include <cstring>

#include <rex/graphics/metal/render_target_cache.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/metal/pipeline_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/metal/metal_provider.h>
#include <rex/logging.h>
#include <rex/xenia_logging_compat.h>

namespace rex::graphics::metal {

MetalRenderTargetCache::MetalRenderTargetCache(
    MetalCommandProcessor& command_processor,
    const RegisterFile& register_file,
    const memory::Memory& memory)
    : RenderTargetCache(register_file, memory,
                        nullptr, 1, 1),
      command_processor_(command_processor) {}

MetalRenderTargetCache::~MetalRenderTargetCache() {
  Shutdown(true);
}

bool MetalRenderTargetCache::Initialize() {
  auto& provider = command_processor_.GetMetalProvider();
  const auto& caps = provider.caps();

  // Detect tile shading support.
  use_tile_shading_ = caps.CanUseTileShadingForEdram();

  MTLResourceOptions options;
  if (provider.has_unified_memory()) {
    options = MTLResourceStorageModeShared;
  } else {
    options = MTLResourceStorageModeManaged;
  }

  id<MTLDevice> device = provider.device();
  edram_buffer_ = [device newBufferWithLength:kEdramSize options:options];
  if (!edram_buffer_) {
    XELOGE("MetalRenderTargetCache: Failed to allocate {} MB EDRAM buffer",
           kEdramSize / (1024 * 1024));
    return false;
  }
  edram_buffer_.label = @"EDRAM_10MB";

  std::memset([edram_buffer_ contents], 0, kEdramSize);
  if (edram_buffer_.storageMode == MTLStorageModeManaged) {
    [edram_buffer_ didModifyRange:NSMakeRange(0, kEdramSize)];
  }

  XELOGI("MetalRenderTargetCache: Allocated {} MB EDRAM ({})",
         kEdramSize / (1024 * 1024),
         provider.has_unified_memory() ? "shared" : "managed");

  if (use_tile_shading_) {
    XELOGI("MetalRenderTargetCache: Apple Silicon tile shading ENABLED — "
           "memoryless render targets will be used");
  } else {
    XELOGI("MetalRenderTargetCache: Tile shading not available — "
           "using standard GPU-private render targets");
  }

  // Pre-create the resolve compute PSO so it's not rebuilt on every resolve.
  auto& pipeline_cache = command_processor_.GetPipelineCache();
  id<MTLFunction> resolve_func = pipeline_cache.resolve_compute_function();
  if (resolve_func) {
    NSError* pso_error = nil;
    resolve_pso_ =
        [device newComputePipelineStateWithFunction:resolve_func
                                             error:&pso_error];
    if (!resolve_pso_) {
      XELOGW("MetalRenderTargetCache: Failed to create resolve compute PSO: {}"
             " — resolves will be unavailable",
             pso_error ? [[pso_error localizedDescription] UTF8String]
                       : "unknown");
    } else {
      XELOGI("MetalRenderTargetCache: Resolve compute PSO created");
    }
  }

  id<MTLFunction> store_func = pipeline_cache.store_compute_function();
  if (store_func) {
    NSError* pso_error = nil;
    store_pso_ =
        [device newComputePipelineStateWithFunction:store_func
                                             error:&pso_error];
    if (!store_pso_) {
      XELOGW("MetalRenderTargetCache: Failed to create store compute PSO: {}"
             " — render-pass-end EDRAM store will be unavailable",
             pso_error ? [[pso_error localizedDescription] UTF8String]
                       : "unknown");
    } else {
      XELOGI("MetalRenderTargetCache: Store compute PSO created");
    }
  }

  return true;
}

void MetalRenderTargetCache::Shutdown(bool from_destructor) {
  ClearCache();
  memoryless_cache_.clear();
  resolve_pso_ = nil;
  store_pso_ = nil;
  edram_buffer_ = nil;
  use_tile_shading_ = false;
}

void MetalRenderTargetCache::ClearCache() {
  for (auto& [key, cached_rt] : render_target_cache_) {
    cached_rt.texture = nil;
  }
  render_target_cache_.clear();
  memoryless_cache_.clear();
}

void MetalRenderTargetCache::CompletedSubmissionUpdated() {
  // Evict stale entries (unused for >60 submissions).
  uint64_t current = command_processor_.GetCurrentSubmission();
  auto it = render_target_cache_.begin();
  while (it != render_target_cache_.end()) {
    if (current - it->second.last_used_submission > 60) {
      it->second.texture = nil;
      it = render_target_cache_.erase(it);
    } else {
      ++it;
    }
  }
}

uint32_t MetalRenderTargetCache::ColorFormatBytesPerPixel(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return 8;
    default:
      return 4;
  }
}

uint32_t MetalRenderTargetCache::DepthFormatBytesPerPixel(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
    case xenos::DepthRenderTargetFormat::kD24FS8:
    default:
      return 4;
  }
}

void MetalRenderTargetCache::BeginSubmission() {}

void MetalRenderTargetCache::BeginFrame() {}

void MetalRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (!edram_buffer_ || !snapshot) return;
  std::memcpy([edram_buffer_ contents], snapshot, kEdramSize);
  if (edram_buffer_.storageMode == MTLStorageModeManaged) {
    [edram_buffer_ didModifyRange:NSMakeRange(0, kEdramSize)];
  }
  XELOGI("MetalRenderTargetCache: Restored EDRAM snapshot");
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetWidth() const {
  auto& provider = command_processor_.GetMetalProvider();
  const auto& caps = provider.caps();
  return caps.max_texture_width_2d;
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetHeight() const {
  auto& provider = command_processor_.GetMetalProvider();
  const auto& caps = provider.caps();
  return caps.max_texture_height_2d;
}

// ============================================================================
// Xenos format → Metal format conversion
// ============================================================================

uint32_t MetalRenderTargetCache::ColorFormatToMetal(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return (uint32_t)MTLPixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return (uint32_t)MTLPixelFormatRGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // Xenos k_2_10_10_10_FLOAT has 2-bit alpha + 10-bit float RGB.
      // RG11B10Float has no alpha channel, causing incorrect results for
      // games using alpha blending with this format. Use RGBA16Float to
      // preserve the alpha channel.
      return (uint32_t)MTLPixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return (uint32_t)MTLPixelFormatRG16Unorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return (uint32_t)MTLPixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return (uint32_t)MTLPixelFormatRGBA16Unorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return (uint32_t)MTLPixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return (uint32_t)MTLPixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return (uint32_t)MTLPixelFormatRG32Float;
    default:
      return (uint32_t)MTLPixelFormatRGBA8Unorm;
  }
}

uint32_t MetalRenderTargetCache::DepthFormatToMetal(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return (uint32_t)MTLPixelFormatDepth32Float_Stencil8;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      // Xbox 360 uses 24-bit float depth + 8-bit stencil.
      // Metal doesn't have this exact format; use D32F_S8.
      return (uint32_t)MTLPixelFormatDepth32Float_Stencil8;
    default:
      return (uint32_t)MTLPixelFormatDepth32Float_Stencil8;
  }
}

// ============================================================================
// Render target texture creation
// ============================================================================

id<MTLTexture> MetalRenderTargetCache::GetOrCreateRenderTargetTexture(
    uint32_t edram_base, uint32_t pitch_tiles, uint32_t height_pixels,
    bool is_depth, uint32_t format, xenos::MsaaSamples msaa) {
  @autoreleasepool {

  // Calculate width from pitch in tiles.
  // For 32bpp formats, each tile is 80 pixels wide.
  uint32_t width_pixels = pitch_tiles * xenos::kEdramTileWidthSamples;
  if (width_pixels == 0) width_pixels = 1280;
  if (height_pixels == 0) height_pixels = 720;

  // Clamp to reasonable max.
  auto& provider = command_processor_.GetMetalProvider();
  const auto& caps = provider.caps();
  width_pixels = std::min(width_pixels, caps.max_texture_width_2d);
  height_pixels = std::min(height_pixels, caps.max_texture_height_2d);

  uint32_t height_tiles = (height_pixels + 15) / 16;  // 16px per tile row

  // MSAA multiplier.
  uint32_t sample_count = 1;
  switch (msaa) {
    case xenos::MsaaSamples::k2X: sample_count = 2; break;
    case xenos::MsaaSamples::k4X: sample_count = 4; break;
    default: sample_count = 1; break;
  }

  RenderTargetKey key;
  key.edram_base = edram_base;
  key.pitch_tiles = pitch_tiles;
  key.height_tiles = height_tiles;
  key.format = format;
  key.is_depth = is_depth;
  key.msaa = sample_count;

  auto it = render_target_cache_.find(key);
  if (it != render_target_cache_.end()) {
    it->second.last_used_submission = command_processor_.GetCurrentSubmission();
    return it->second.texture;
  }

  // Create new texture.
  id<MTLDevice> device = provider.device();
  MTLTextureDescriptor* desc;

  MTLPixelFormat metal_format = (MTLPixelFormat)format;

  if (sample_count > 1) {
    // MSAA texture.
    desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:metal_format
                                    width:width_pixels
                                   height:height_pixels
                                mipmapped:NO];
    desc.textureType = MTLTextureType2DMultisample;
    desc.sampleCount = sample_count;
  } else {
    desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:metal_format
                                    width:width_pixels
                                   height:height_pixels
                                mipmapped:NO];
  }

  if (is_depth) {
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  } else {
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                 MTLTextureUsageShaderWrite;
  }
  desc.storageMode = MTLStorageModePrivate;  // GPU-only for speed

  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  if (!texture) {
    XELOGE("MetalRenderTargetCache: Failed to create {}x{} {} texture",
           width_pixels, height_pixels, is_depth ? "depth" : "color");
    return nil;
  }

  texture.label = [NSString stringWithFormat:@"RT_%s_%ux%u_base%u",
                   is_depth ? "depth" : "color",
                   width_pixels, height_pixels, edram_base];

  CachedRenderTarget cached;
  cached.texture = texture;
  cached.width = width_pixels;
  cached.height = height_pixels;
  cached.last_used_submission = command_processor_.GetCurrentSubmission();
  render_target_cache_[key] = cached;

  XELOGGPU("MetalRenderTargetCache: Created {}x{} {} RT (base={}, cache={})",
            width_pixels, height_pixels, is_depth ? "depth" : "color",
            edram_base, render_target_cache_.size());

  return texture;

  }  // @autoreleasepool
}

// ============================================================================
// Render target first-use tracking
// ============================================================================

bool MetalRenderTargetCache::HasBeenRendered(
    uint32_t edram_base, uint32_t pitch_tiles, uint32_t height_pixels,
    bool is_depth, uint32_t format, xenos::MsaaSamples msaa) const {
  uint32_t height_tiles = (height_pixels + 15) / 16;
  uint32_t sample_count = 1;
  switch (msaa) {
    case xenos::MsaaSamples::k2X: sample_count = 2; break;
    case xenos::MsaaSamples::k4X: sample_count = 4; break;
    default: break;
  }
  RenderTargetKey key;
  key.edram_base = edram_base;
  key.pitch_tiles = pitch_tiles;
  key.height_tiles = height_tiles;
  key.format = format;
  key.is_depth = is_depth;
  key.msaa = sample_count;
  auto it = render_target_cache_.find(key);
  return it != render_target_cache_.end() && it->second.has_been_rendered;
}

void MetalRenderTargetCache::MarkRendered(
    uint32_t edram_base, uint32_t pitch_tiles, uint32_t height_pixels,
    bool is_depth, uint32_t format, xenos::MsaaSamples msaa) {
  uint32_t height_tiles = (height_pixels + 15) / 16;
  uint32_t sample_count = 1;
  switch (msaa) {
    case xenos::MsaaSamples::k2X: sample_count = 2; break;
    case xenos::MsaaSamples::k4X: sample_count = 4; break;
    default: break;
  }
  RenderTargetKey key;
  key.edram_base = edram_base;
  key.pitch_tiles = pitch_tiles;
  key.height_tiles = height_tiles;
  key.format = format;
  key.is_depth = is_depth;
  key.msaa = sample_count;
  auto it = render_target_cache_.find(key);
  if (it != render_target_cache_.end()) {
    it->second.has_been_rendered = true;
  }
}

// ============================================================================
// Resolve operations
// ============================================================================

void MetalRenderTargetCache::StoreRenderTargetToEdram(
    id<MTLRenderCommandEncoder> encoder, const RenderTarget& rt) {
  (void)encoder;

  if (!REXCVAR_GET(metal_edram_store_on_renderpass_end)) {
    return;
  }
  if (!rt.texture || !edram_buffer_ || !store_pso_) {
    return;
  }
  if (rt.texture.storageMode == MTLStorageModeMemoryless) {
    // Memoryless targets have no DRAM backing. A dedicated tile-imageblock
    // path is needed for correctness here.
    return;
  }
  if (rt.msaa != xenos::MsaaSamples::k1X) {
    // Multisample store is not implemented in this simplified path.
    return;
  }
  if (rt.is_depth) {
    // Depth/stencil store needs dedicated packing and format handling.
    return;
  }

  id<MTLCommandBuffer> command_buffer = command_processor_.GetCurrentCommandBuffer();
  if (!command_buffer) {
    return;
  }
  id<MTLComputeCommandEncoder> compute = [command_buffer computeCommandEncoder];
  if (!compute) {
    return;
  }

  [compute setComputePipelineState:store_pso_];
  [compute setTexture:rt.texture atIndex:0];
  [compute setBuffer:edram_buffer_ offset:0 atIndex:0];

  struct StoreParams {
    uint32_t edram_base;
    uint32_t edram_pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
  };

  uint32_t bytes_per_pixel = ColorFormatBytesPerPixel(rt.color_format);
  uint32_t edram_base_bytes = rt.edram_base << 2;
  uint32_t edram_pitch_bytes =
      rt.pitch_tiles * xenos::kEdramTileWidthSamples * bytes_per_pixel;
  if (edram_base_bytes >= kEdramSize) {
    [compute endEncoding];
    return;
  }

  StoreParams params;
  params.edram_base = edram_base_bytes;
  params.edram_pitch = edram_pitch_bytes;
  params.width = rt.width_pixels;
  params.height = rt.height_pixels;
  params.bytes_per_pixel = bytes_per_pixel;
  [compute setBytes:&params length:sizeof(params) atIndex:1];

  MTLSize threads_per_threadgroup =
      MTLSizeMake(std::min(uint32_t(16), rt.width_pixels),
                  std::min(uint32_t(16), rt.height_pixels), 1);
  MTLSize threadgroups =
      MTLSizeMake((rt.width_pixels + threads_per_threadgroup.width - 1) /
                      threads_per_threadgroup.width,
                  (rt.height_pixels + threads_per_threadgroup.height - 1) /
                      threads_per_threadgroup.height,
                  1);
  [compute dispatchThreadgroups:threadgroups
          threadsPerThreadgroup:threads_per_threadgroup];
  [compute endEncoding];
}

bool MetalRenderTargetCache::ResolveEdramToTexture(
    id<MTLComputeCommandEncoder> compute_encoder,
    uint32_t edram_base, uint32_t edram_pitch,
    uint32_t src_x, uint32_t src_y,
    uint32_t width, uint32_t height,
    uint32_t bytes_per_pixel,
    id<MTLTexture> dest_texture) {
  if (!compute_encoder || !dest_texture || !edram_buffer_) return false;

  // Bind the cached resolve compute pipeline state.
  if (!resolve_pso_) {
    XELOGE("MetalRenderTargetCache: Resolve compute PSO not available");
    return false;
  }
  [compute_encoder setComputePipelineState:resolve_pso_];

  // The resolve compute kernel reads from the EDRAM buffer and writes
  // to the destination texture.
  [compute_encoder setBuffer:edram_buffer_ offset:0 atIndex:0];
  [compute_encoder setTexture:dest_texture atIndex:0];

  // ResolveParams structure matching the compute kernel.
  struct ResolveParams {
    uint32_t edram_base;
    uint32_t edram_pitch;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    uint32_t format;
  };

  ResolveParams params;
  params.edram_base = edram_base;
  params.edram_pitch = edram_pitch;
  params.src_x = src_x;
  params.src_y = src_y;
  params.width = width;
  params.height = height;
  params.bytes_per_pixel = bytes_per_pixel;
  params.format = 0;  // Base format

  [compute_encoder setBytes:&params length:sizeof(params) atIndex:1];

  // Dispatch threads: one per pixel.
  MTLSize threadgroup_size = MTLSizeMake(
      std::min(uint32_t(16), width),
      std::min(uint32_t(16), height), 1);
  MTLSize grid_size = MTLSizeMake(width, height, 1);

  // dispatchThreads:threadsPerThreadgroup: requires non-uniform threadgroup
  // support (MTLGPUFamily.mac1+). Use dispatchThreadgroups for maximum
  // compatibility with older Intel Macs that may lack this support.
  MTLSize threadgroup_count = MTLSizeMake(
      (width + threadgroup_size.width - 1) / threadgroup_size.width,
      (height + threadgroup_size.height - 1) / threadgroup_size.height, 1);
  [compute_encoder dispatchThreadgroups:threadgroup_count
                  threadsPerThreadgroup:threadgroup_size];

  return true;
}

// ============================================================================
// GetCurrentRenderTargets
// ============================================================================

uint32_t MetalRenderTargetCache::GetCurrentRenderTargets(
    RenderTarget color_targets[4], RenderTarget& depth_target,
    bool& depth_enabled) {
  const auto& regs = register_file();

  auto rb_modecontrol = regs.Get<reg::RB_MODECONTROL>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto rb_color_info = regs.Get<reg::RB_COLOR_INFO>();
  auto rb_color1_info =
      regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[1]);
  auto rb_color2_info =
      regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[2]);
  auto rb_color3_info =
      regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[3]);
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();

  uint32_t surface_pitch_tiles = rb_surface_info.surface_pitch;
  xenos::MsaaSamples msaa = rb_surface_info.msaa_samples;

  uint32_t active_color_count = 0;

  struct ColorInfo {
    uint32_t edram_base;
    xenos::ColorRenderTargetFormat format;
  };
  ColorInfo color_infos[4] = {
      {rb_color_info.color_base, rb_color_info.color_format},
      {rb_color1_info.color_base, rb_color1_info.color_format},
      {rb_color2_info.color_base, rb_color2_info.color_format},
      {rb_color3_info.color_base, rb_color3_info.color_format},
  };

  // Determine viewport height from PA_SC registers if available.
  uint32_t viewport_height = 720;  // Default
  auto pa_cl_vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (pa_cl_vte.vport_y_scale_ena) {
    float y_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
    if (y_scale != 0.0f) {
      viewport_height = uint32_t(std::abs(y_scale) * 2.0f);
    }
  }
  viewport_height = std::max(viewport_height, 1u);

  if (rb_modecontrol.edram_mode == xenos::EdramMode::kColorDepth) {
    // Determine how many MRTs the game actually enabled by checking
    // RB_COLOR_MASK — if all channels are masked off, the MRT is unused.
    auto rb_color_mask = regs.Get<reg::RB_COLOR_MASK>();
    uint32_t mrt_count = 4;
    // Walk backwards to find the highest active MRT.
    for (int32_t i = 3; i >= 0; --i) {
      uint32_t mask = (rb_color_mask.value >> (i * 4)) & 0xF;
      if (mask != 0) {
        mrt_count = uint32_t(i) + 1;
        break;
      }
      if (i == 0) mrt_count = 1;  // Always create at least MRT0.
    }

    for (uint32_t i = 0; i < mrt_count; ++i) {
      uint32_t metal_fmt = ColorFormatToMetal(color_infos[i].format);

      color_targets[i].edram_base = color_infos[i].edram_base;
      color_targets[i].pitch_tiles = surface_pitch_tiles;
      color_targets[i].msaa = msaa;
      color_targets[i].color_format = color_infos[i].format;
      color_targets[i].is_depth = false;
      color_targets[i].width_pixels =
          surface_pitch_tiles * xenos::kEdramTileWidthSamples;
      color_targets[i].height_pixels = viewport_height;

      // Use memoryless render targets on Apple Silicon (tile shading path).
      uint32_t sample_count = 1;
      switch (msaa) {
        case xenos::MsaaSamples::k2X: sample_count = 2; break;
        case xenos::MsaaSamples::k4X: sample_count = 4; break;
        default: break;
      }

      if (use_tile_shading_) {
        color_targets[i].texture = GetOrCreateMemorylessRenderTarget(
            color_targets[i].width_pixels, viewport_height,
            metal_fmt, sample_count);
      }
      // Fall back to standard GPU-private texture if tile shading is off
      // or memoryless creation failed.
      if (!color_targets[i].texture) {
        color_targets[i].texture = GetOrCreateRenderTargetTexture(
            color_infos[i].edram_base, surface_pitch_tiles, viewport_height,
            false, metal_fmt, msaa);
      }
    }
    active_color_count = mrt_count;
  }

  // Depth target.
  uint32_t depth_metal_fmt = DepthFormatToMetal(rb_depth_info.depth_format);

  depth_target.edram_base = rb_depth_info.depth_base;
  depth_target.pitch_tiles = surface_pitch_tiles;
  depth_target.msaa = msaa;
  depth_target.depth_format = rb_depth_info.depth_format;
  depth_target.is_depth = true;
  depth_target.width_pixels =
      surface_pitch_tiles * xenos::kEdramTileWidthSamples;
  depth_target.height_pixels = viewport_height;
  depth_enabled =
      (rb_modecontrol.edram_mode == xenos::EdramMode::kColorDepth) ||
      (rb_modecontrol.edram_mode == xenos::EdramMode::kDepthOnly);

  if (depth_enabled) {
    depth_target.texture = GetOrCreateRenderTargetTexture(
        rb_depth_info.depth_base, surface_pitch_tiles, viewport_height,
        true, depth_metal_fmt, msaa);
  } else {
    depth_target.texture = nil;
  }

  return active_color_count;
}

// ============================================================================
// Apple Silicon memoryless render targets (tile shading path)
// ============================================================================

id<MTLTexture> MetalRenderTargetCache::GetOrCreateMemorylessRenderTarget(
    uint32_t width, uint32_t height, uint32_t format,
    uint32_t sample_count) {
  if (!use_tile_shading_) return nil;

  // Guard against zero dimensions -- Metal aborts on a zero-width descriptor.
  // surface_pitch_tiles can be 0 early in boot before the game writes
  // RB_SURFACE_INFO, producing width = pitch_tiles * kEdramTileWidthSamples = 0.
  // GetOrCreateRenderTargetTexture has this same guard; replicate it here.
  if (width == 0) width = 1280;
  if (height == 0) height = 720;

  MemorylessKey key;
  key.width = width;
  key.height = height;
  key.format = (uint32_t)format;
  key.samples = sample_count;

  auto it = memoryless_cache_.find(key);
  if (it != memoryless_cache_.end()) {
    return it->second;
  }

  // Cap the cache size to prevent unbounded growth from many different
  // RT dimensions. Memoryless textures have zero DRAM cost, but the
  // hash map metadata and MTLTexture object overhead still accumulate.
  static constexpr size_t kMaxMemorylessCacheSize = 64;
  if (memoryless_cache_.size() >= kMaxMemorylessCacheSize) {
    memoryless_cache_.clear();
  }

  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) return nil;

  // Clamp to device limits (width/height already guarded against zero above).
  const auto& caps = provider.caps();
  width  = std::min(width,  caps.max_texture_width_2d);
  height = std::min(height, caps.max_texture_height_2d);

  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format
                                  width:width
                                 height:height
                              mipmapped:NO];

  if (sample_count > 1) {
    desc.textureType = MTLTextureType2DMultisample;
    desc.sampleCount = sample_count;
  }

  // Memoryless: lives only in tile memory, never backed by DRAM.
  // This is the key Apple Silicon optimization — on-chip SRAM is
  // dramatically faster than DRAM for render target load/store.
  desc.storageMode = MTLStorageModeMemoryless;
  desc.usage = MTLTextureUsageRenderTarget;

  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  if (!texture) {
    XELOGW("MetalRenderTargetCache: Failed to create memoryless {}x{} texture",
            width, height);
    return nil;
  }

  texture.label = [NSString stringWithFormat:@"Memoryless_%ux%u_fmt%u",
                   width, height, (uint32_t)format];

  memoryless_cache_[key] = texture;

  XELOGGPU("MetalRenderTargetCache: Created memoryless {}x{} RT "
            "(tile shading, cache={})",
            width, height, memoryless_cache_.size());
  return texture;
}

}  // namespace rex::graphics::metal
