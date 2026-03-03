/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 */

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <rex/ui/metal/metal_presenter.h>
#include <rex/ui/metal/metal_provider.h>
#include <rex/ui/surface.h>
#include <rex/ui/surface_macos.h>
#include <rex/logging.h>

namespace rex {
namespace ui {
namespace metal {

// Minimal vertex/fragment shader for presenting the guest output texture.
static const char* kPresentShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float2 texcoord;
};

vertex VertexOut present_vertex(uint vertex_id [[vertex_id]]) {
  // Full-screen triangle strip: 0,1,2 -> CCW triangle covering the screen.
  VertexOut out;
  float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  out.texcoord = uv;
  return out;
}

fragment float4 present_fragment(VertexOut in [[stage_in]],
                                  texture2d<float> tex [[texture(0)]],
                                  sampler samp [[sampler(0)]]) {
  return tex.sample(samp, in.texcoord);
}
)";

std::unique_ptr<MetalPresenter> MetalPresenter::Create(
    MetalProvider& provider,
    HostGpuLossCallback host_gpu_loss_callback) {
  auto presenter = std::unique_ptr<MetalPresenter>(
      new MetalPresenter(provider, std::move(host_gpu_loss_callback)));
  if (!presenter->Initialize()) {
    return nullptr;
  }
  return presenter;
}

MetalPresenter::MetalPresenter(MetalProvider& provider,
                               HostGpuLossCallback host_gpu_loss_callback)
    : Presenter(std::move(host_gpu_loss_callback)), provider_(provider) {}

MetalPresenter::~MetalPresenter() {
  DestroyGuestOutputPipeline();
  DestroyGuestOutputTextures();

  capture_staging_texture_ = nil;
  metal_layer_ = nil;
}

bool MetalPresenter::Initialize() {
  if (!InitializeCommonSurfaceIndependent()) {
    return false;
  }

  if (!CreateGuestOutputPipeline()) {
    REXLOG_WARN("MetalPresenter: Failed to create guest output pipeline "
            "(will retry on surface connection)");
  }

  return true;
}

Surface::TypeFlags MetalPresenter::GetSupportedSurfaceTypes() const {
  return Surface::kTypeFlag_MacOSMetalLayer;
}

MetalPresenter::SurfacePaintConnectResult
MetalPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width,
    uint32_t new_surface_height, bool was_paintable,
    bool& is_vsync_implicit_out) {
  // Get the CAMetalLayer from the macOS surface.
  if (new_surface.GetType() != Surface::kTypeIndex_MacOSMetalLayer) {
    REXLOG_ERROR("MetalPresenter: Surface does not support Metal layer");
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }

  auto& macos_surface = static_cast<MacOSMetalSurface&>(new_surface);
  CAMetalLayer* layer = macos_surface.metal_layer();
  if (!layer) {
    REXLOG_ERROR("MetalPresenter: Surface has no Metal layer");
    return SurfacePaintConnectResult::kFailure;
  }

  // Configure the Metal layer.
  layer.device = provider_.device();
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = YES;

  // Set drawable size to match the surface (with Retina scaling).
  layer.drawableSize = CGSizeMake(new_surface_width, new_surface_height);

  // Disable vsync by default for lower latency.
  // On macOS, displaySyncEnabled controls vsync on the layer.
  if (@available(macOS 10.13, *)) {
    layer.displaySyncEnabled = NO;
    is_vsync_implicit_out = false;
  } else {
    is_vsync_implicit_out = true;
  }

  metal_layer_ = layer;

  // Ensure the guest output pipeline exists.
  if (!guest_output_pipeline_) {
    if (!CreateGuestOutputPipeline()) {
      REXLOG_ERROR("MetalPresenter: Failed to create guest output pipeline");
      metal_layer_ = nil;
      return SurfacePaintConnectResult::kFailure;
    }
  }

  return SurfacePaintConnectResult::kSuccess;
}

void MetalPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  metal_layer_ = nil;
}

bool MetalPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width,
    uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher,
    bool& is_8bpc_out_ref) {
  // Ensure guest output textures are the right size.
  if (guest_output_texture_width_ != frontbuffer_width ||
      guest_output_texture_height_ != frontbuffer_height) {
    DestroyGuestOutputTextures();
    if (!CreateGuestOutputTextures(frontbuffer_width, frontbuffer_height)) {
      return false;
    }
  }

  id<MTLTexture> target_texture = guest_output_textures_[mailbox_index];
  if (!target_texture) {
    return false;
  }

  MetalGuestOutputRefreshContext context(target_texture, is_8bpc_out_ref);
  return refresher(context);
}

MetalPresenter::PaintResult MetalPresenter::PaintAndPresentImpl(
    bool execute_ui_drawers) {
  if (!metal_layer_) {
    return PaintResult::kNotPresented;
  }

  @autoreleasepool {
    // Get the next drawable from the layer.
    id<CAMetalDrawable> drawable = [metal_layer_ nextDrawable];
    if (!drawable) {
      return PaintResult::kNotPresented;
    }

    id<MTLCommandBuffer> command_buffer =
        [provider_.command_queue() commandBuffer];
    if (!command_buffer) {
      return PaintResult::kNotPresented;
    }

    // Get the guest output to paint.
    // Hold the consumer lock while accessing guest_output_textures_ to
    // prevent another thread from updating the mailbox and reusing the
    // texture between lock release and texture access.
    uint32_t guest_output_mailbox_index;
    GuestOutputProperties guest_output_properties;
    GuestOutputPaintConfig guest_output_paint_config;
    id<MTLTexture> guest_texture = nil;
    {
      auto consumer_lock = ConsumeGuestOutput(
          guest_output_mailbox_index, &guest_output_properties,
          &guest_output_paint_config);
      if (guest_output_mailbox_index != UINT32_MAX &&
          guest_output_properties.IsActive()) {
        guest_texture = guest_output_textures_[guest_output_mailbox_index];
      }
    }

    // Create render pass to draw to the drawable's texture.
    MTLRenderPassDescriptor* render_pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    render_pass.colorAttachments[0].texture = drawable.texture;
    render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    render_pass.colorAttachments[0].clearColor =
        MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:render_pass];

    // If we have guest output, draw it.
    if (guest_texture && guest_output_pipeline_) {
      [encoder setRenderPipelineState:guest_output_pipeline_];
      [encoder setFragmentTexture:guest_texture atIndex:0];
      [encoder setFragmentSamplerState:guest_output_sampler_bilinear_
                               atIndex:0];

      // Draw full-screen triangle (3 vertices, no vertex buffer needed).
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                  vertexStart:0
                  vertexCount:3];
    }

    // Execute UI drawers (ImGui) on top of the guest output.
    if (execute_ui_drawers) {
      MetalUIDrawContext ui_draw_context(
          *this, uint32_t(drawable.texture.width),
          uint32_t(drawable.texture.height), encoder,
          present_submission_current_,
          present_submission_completed_.load(std::memory_order_relaxed));
      ExecuteUIDrawersFromUIThread(ui_draw_context);
    }

    [encoder endEncoding];

    [command_buffer presentDrawable:drawable];

    // Track presentation submission completion.
    uint64_t this_submission = present_submission_current_;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
      // Use CAS loop — the completion handler runs on an arbitrary Metal
      // driver thread. Matches the pattern in command_processor.mm.
      uint64_t prev =
          present_submission_completed_.load(std::memory_order_relaxed);
      while (prev < this_submission) {
        if (present_submission_completed_.compare_exchange_weak(
                prev, this_submission, std::memory_order_relaxed)) {
          break;
        }
      }
    }];
    present_submission_current_++;

    [command_buffer commit];
  }

  return PaintResult::kPresented;
}

bool MetalPresenter::CaptureGuestOutput(RawImage& image_out) {
  @autoreleasepool {
    // Find the latest guest output texture.
    uint32_t mailbox_index;
    GuestOutputProperties properties;
    {
      auto lock = ConsumeGuestOutput(mailbox_index, &properties, nullptr);
    }

    if (mailbox_index == UINT32_MAX || !properties.IsActive()) {
      return false;
    }

    id<MTLTexture> source = guest_output_textures_[mailbox_index];
    if (!source) {
      return false;
    }

    uint32_t width = uint32_t([source width]);
    uint32_t height = uint32_t([source height]);
    if (width == 0 || height == 0) {
      return false;
    }

    // If source is private storage, blit to a readable staging texture.
    id<MTLTexture> readable = source;
    id<MTLCommandBuffer> cmd_buf = nil;

    if ([source storageMode] == MTLStorageModePrivate) {
      // Reuse cached staging texture if dimensions match.
      if (!capture_staging_texture_ ||
          capture_staging_width_ != width ||
          capture_staging_height_ != height) {
        MTLTextureDescriptor* staging_desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:[source pixelFormat]
                                        width:width
                                       height:height
                                    mipmapped:NO];
        staging_desc.usage = MTLTextureUsageShaderRead;
        if (provider_.has_unified_memory()) {
          staging_desc.storageMode = MTLStorageModeShared;
        } else {
          staging_desc.storageMode = MTLStorageModeManaged;
        }

        capture_staging_texture_ =
            [provider_.device() newTextureWithDescriptor:staging_desc];
        if (!capture_staging_texture_) return false;
        capture_staging_width_ = width;
        capture_staging_height_ = height;
      }

      readable = capture_staging_texture_;

      cmd_buf = [provider_.command_queue() commandBuffer];
      if (!cmd_buf) return false;

      id<MTLBlitCommandEncoder> blit = [cmd_buf blitCommandEncoder];
      MTLSize copy_size = MTLSizeMake(width, height, 1);
      [blit copyFromTexture:source sourceSlice:0 sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:copy_size
                  toTexture:readable destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
      if ([readable storageMode] == MTLStorageModeManaged) {
        [blit synchronizeTexture:readable slice:0 level:0];
      }
      [blit endEncoding];
      [cmd_buf commit];
      [cmd_buf waitUntilCompleted];
    }

    // Read pixels into the RawImage (RGBX8).
    size_t row_bytes = width * 4;
    image_out.width = width;
    image_out.height = height;
    image_out.stride = row_bytes;
    image_out.data.resize(row_bytes * height);

    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [readable getBytes:image_out.data.data()
           bytesPerRow:row_bytes
            fromRegion:region
           mipmapLevel:0];

    return true;
  }
}

bool MetalPresenter::CreateGuestOutputTextures(uint32_t width,
                                               uint32_t height) {
  id<MTLDevice> device = provider_.device();
  if (!device || width == 0 || height == 0) {
    return false;
  }

  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;

  // Use private storage for render targets — faster for GPU-only work.
  // The blit in IssueSwap and CaptureGuestOutput handles the
  // private→readable path when CPU access is needed.
  desc.storageMode = MTLStorageModePrivate;

  for (size_t i = 0; i < kGuestOutputMailboxSize; ++i) {
    guest_output_textures_[i] = [device newTextureWithDescriptor:desc];
    if (!guest_output_textures_[i]) {
      REXLOG_ERROR("MetalPresenter: Failed to create guest output texture {}",
             i);
      DestroyGuestOutputTextures();
      return false;
    }
  }

  guest_output_texture_width_ = width;
  guest_output_texture_height_ = height;

  REXLOG_INFO("MetalPresenter: Created {}x{} guest output textures", width, height);
  return true;
}

void MetalPresenter::DestroyGuestOutputTextures() {
  for (size_t i = 0; i < kGuestOutputMailboxSize; ++i) {
    guest_output_textures_[i] = nil;
  }
  guest_output_texture_width_ = 0;
  guest_output_texture_height_ = 0;
}

bool MetalPresenter::CreateGuestOutputPipeline() {
  id<MTLDevice> device = provider_.device();
  if (!device) {
    return false;
  }

  // Compile the presentation shaders.
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kPresentShaderSource];
  id<MTLLibrary> library = [device newLibraryWithSource:source
                                                options:nil
                                                  error:&error];
  if (!library) {
    REXLOG_ERROR("MetalPresenter: Failed to compile present shaders: {}",
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return false;
  }

  id<MTLFunction> vertex_func =
      [library newFunctionWithName:@"present_vertex"];
  id<MTLFunction> fragment_func =
      [library newFunctionWithName:@"present_fragment"];

  if (!vertex_func || !fragment_func) {
    REXLOG_ERROR("MetalPresenter: Failed to find shader functions");
    return false;
  }

  // Create the render pipeline state.
  MTLRenderPipelineDescriptor* pipeline_desc =
      [[MTLRenderPipelineDescriptor alloc] init];
  pipeline_desc.vertexFunction = vertex_func;
  pipeline_desc.fragmentFunction = fragment_func;
  pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  // Enable alpha blending for UI overlay support.
  pipeline_desc.colorAttachments[0].blendingEnabled = NO;

  guest_output_pipeline_ = [device
      newRenderPipelineStateWithDescriptor:pipeline_desc
                                     error:&error];
  if (!guest_output_pipeline_) {
    REXLOG_ERROR("MetalPresenter: Failed to create render pipeline: {}",
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return false;
  }

  // ARC manages Objective-C object lifetimes automatically.

  // Create samplers.
  MTLSamplerDescriptor* sampler_desc =
      [[MTLSamplerDescriptor alloc] init];

  // Bilinear sampler.
  sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
  sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
  sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
  guest_output_sampler_bilinear_ =
      [device newSamplerStateWithDescriptor:sampler_desc];

  // Nearest sampler.
  sampler_desc.minFilter = MTLSamplerMinMagFilterNearest;
  sampler_desc.magFilter = MTLSamplerMinMagFilterNearest;
  guest_output_sampler_nearest_ =
      [device newSamplerStateWithDescriptor:sampler_desc];

  // ARC manages sampler_desc lifetime.

  if (!guest_output_sampler_bilinear_ || !guest_output_sampler_nearest_) {
    REXLOG_ERROR("MetalPresenter: Failed to create samplers");
    DestroyGuestOutputPipeline();
    return false;
  }

  REXLOG_INFO("MetalPresenter: Guest output pipeline created successfully");
  return true;
}

void MetalPresenter::DestroyGuestOutputPipeline() {
  guest_output_sampler_nearest_ = nil;
  guest_output_sampler_bilinear_ = nil;
  guest_output_pipeline_ = nil;
}

}  // namespace metal
}  // namespace ui
}  // namespace rex
