/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "rex/ui/metal/metal_presenter.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>

#include <rex/logging/log_category.h>
#include "rex/math.h"
#include "rex/ui/surface.h"

#include "rex/ui/shaders/bytecode/metal/guest_output_bilinear_dither_ps.h"
#include "rex/ui/shaders/bytecode/metal/guest_output_bilinear_ps.h"
#include "rex/ui/shaders/bytecode/metal/guest_output_triangle_strip_rect_vs.h"

namespace rex {
namespace ui {
namespace metal {

namespace {

// The CAMetalLayer drawable format.
constexpr MTLPixelFormat kDrawablePixelFormat = MTLPixelFormatBGRA8Unorm;
// The MetalFX scaler's output format, 10bpc like the Vulkan presenter's
// intermediate images, with 8bpc as the fallback when MetalFX can't do it.
constexpr MTLPixelFormat kIntermediatePixelFormat = MTLPixelFormatRGB10A2Unorm;
// The largest 2D texture dimension on all Apple-silicon Macs.
constexpr uint32_t kMaxRenderTargetSize = 16384;

id<MTLFunction> LoadEntryFunction(id<MTLDevice> device, const uint8_t* metallib,
                                  size_t size, const char* name) {
  dispatch_data_t data = dispatch_data_create(metallib, size, nullptr, ^{
  });
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
  if (!library) {
    REXLOG_ERROR("MetalPresenter: failed to load the {} shader: {}", name,
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return nil;
  }
  // The XeSL MSL entry point is always entry_xe.
  id<MTLFunction> function = [library newFunctionWithName:@"entry_xe"];
  if (!function) {
    REXLOG_ERROR("MetalPresenter: no entry_xe in the {} shader", name);
  }
  return function;
}

}  // namespace

MetalPresenter::~MetalPresenter() {
  // device_/command_queue_ are owned by the provider; metal_layer_ by the
  // window. Release the objects we retained: mailbox textures, shaders,
  // pipelines, the sampler and the MetalFX scaler.
  auto release = [](void* object) {
    if (object) {
      CFRelease(object);
    }
  };
  for (void* texture : guest_output_textures_) {
    release(texture);
  }
  for (void* pipeline : guest_output_paint_pipelines_) {
    release(pipeline);
  }
  for (void* ps : guest_output_paint_ps_) {
    release(ps);
  }
  release(guest_output_paint_vs_);
  release(guest_output_paint_sampler_);
  release(metalfx_scaler_);
  release(metalfx_output_texture_);
}

std::unique_ptr<MetalPresenter> MetalPresenter::Create(
    HostGPULossCallback host_gpu_loss_callback,
    const ::rex::gpu::XenosGraphicsDriverBase& driver, void* device,
    void* command_queue) {
  if (!device || !command_queue) {
    return nullptr;
  }
  std::unique_ptr<MetalPresenter> presenter(new MetalPresenter(
      std::move(host_gpu_loss_callback), driver, device, command_queue));
  if (!presenter->InitializeCommonSurfaceIndependent()) {
    return nullptr;
  }
  return presenter;
}

::rex::gpu::Surface::TypeFlags MetalPresenter::GetSupportedSurfaceTypes() const {
  return ::rex::gpu::Surface::kTypeFlagCAMetalLayer;
}

// Breadcrumb: presenter construction is the first place the Metal path can stall, because it waits on the device.
bool MetalPresenter::SupportsGuestOutputPaintEffect(
    GuestOutputPaintEffect effect) const {
  if (effect == GuestOutputPaintEffect::kBilinear) {
    // The device doesn't change, so ask once.
    static const bool metalfx_spatial_supported = [MTLFXSpatialScalerDescriptor
        supportsDevice:(__bridge id<MTLDevice>)device_];
    return metalfx_spatial_supported;
  }
  return true;
}

bool MetalPresenter::CaptureGuestOutput(RawImage& image_out) {
  // Read back the most recently refreshed guest output mailbox texture. This consumes the mailbox, so it belongs to
  // callers that own the frame (the trace dump); the screenshot hook uses ReadbackTextureToImage with the texture the
  // paint already holds, because a second consume would take the frame out from under the presenter.
  void* tex_ptr = nullptr;
  uint32_t width = 0, height = 0;
  {
    uint32_t mailbox_index = UINT32_MAX;
    std::unique_lock<std::mutex> consumer_lock(
        ConsumeGuestOutput(mailbox_index, nullptr, nullptr));
    if (mailbox_index != UINT32_MAX &&
        mailbox_index < guest_output_textures_.size()) {
      tex_ptr = guest_output_textures_[mailbox_index];
      width = guest_output_texture_widths_[mailbox_index];
      height = guest_output_texture_heights_[mailbox_index];
    }
  }
  return ReadbackTextureToImage(tex_ptr, width, height, image_out);
}

bool MetalPresenter::ReadbackTextureToImage(void* texture_ptr, uint32_t width,
                                           uint32_t height,
                                           RawImage& image_out) {
  if (!texture_ptr || !width || !height) {
    return false;
  }
  void* tex_ptr = texture_ptr;
  id<MTLTexture> texture = (__bridge id<MTLTexture>)tex_ptr;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue_;
  size_t row_bytes = size_t(width) * 4;
  id<MTLBuffer> staging =
      [device newBufferWithLength:row_bytes * height
                          options:MTLResourceStorageModeShared];
  if (!staging) {
    return false;
  }
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  [blit copyFromTexture:texture
             sourceSlice:0
             sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(width, height, 1)
                toBuffer:staging
       destinationOffset:0
  destinationBytesPerRow:row_bytes
destinationBytesPerImage:row_bytes * height];
  [blit endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  image_out.width = width;
  image_out.height = height;
  image_out.data.resize(row_bytes * height);
  const uint8_t* src = static_cast<const uint8_t*>(staging.contents);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    image_out.data[i * 4 + 0] = src[i * 4 + 0];  // R
    image_out.data[i * 4 + 1] = src[i * 4 + 1];  // G
    image_out.data[i * 4 + 2] = src[i * 4 + 2];  // B
    image_out.data[i * 4 + 3] = 255;
  }
  return true;
}

gpu::Presenter::SurfacePaintConnectResult
MetalPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    ::rex::gpu::Surface& new_surface, uint32_t new_surface_width,
    uint32_t new_surface_height, [[maybe_unused]] bool was_paintable,
    bool& is_vsync_implicit_out) {
  if (new_surface.GetType() != ::rex::gpu::Surface::kTypeIndexCAMetalLayer) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  auto& metal_surface = static_cast<::rex::gpu::NativeSurface&>(new_surface);
  CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_surface.GetLayer();
  if (!layer) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }

  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  layer.device = device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  // We only render into the drawable and present it (no readback/sampling).
  layer.framebufferOnly = YES;
  // Without this, nextDrawable blocks the calling thread until a drawable is free. During startup the window is still
  // completing its fullscreen transition, that transition needs the main thread, and the main thread is the caller -
  // so the block is unbreakable and the window never gets a frame. Timing out returns nil instead, and the paint
  // reports "not presented" until the layer really has a drawable.
  layer.allowsNextDrawableTimeout = YES;
  layer.drawableSize =
      CGSizeMake(CGFloat(new_surface_width), CGFloat(new_surface_height));

  {
    std::lock_guard<std::mutex> lock(surface_mutex_);
    metal_layer_ = metal_surface.GetLayer();
    surface_width_ = new_surface_width;
    surface_height_ = new_surface_height;
    surface_connected_at_ = std::chrono::steady_clock::now();
  }

  // CAMetalLayer paces presentation to the display refresh; treat vsync as
  // implicitly on so the base class doesn't also try to block for it.
  is_vsync_implicit_out = true;
  return SurfacePaintConnectResult::kSuccess;
}

void MetalPresenter::DriverDisconnectPaintingFromSurface() {
  std::lock_guard<std::mutex> lock(surface_mutex_);
  metal_layer_ = nullptr;
  surface_width_ = 0;
  surface_height_ = 0;
}

void* MetalPresenter::EnsureGuestOutputTexture(uint32_t index, uint32_t width,
                                               uint32_t height) {
  if (index >= guest_output_textures_.size()) {
    return nullptr;
  }
  if (guest_output_textures_[index] &&
      guest_output_texture_widths_[index] == width &&
      guest_output_texture_heights_[index] == height) {
    return guest_output_textures_[index];
  }
  // Release the previous texture for this slot, if any.
  if (guest_output_textures_[index]) {
    id<MTLTexture> old =
        (__bridge_transfer id<MTLTexture>)guest_output_textures_[index];
    (void)old;
    guest_output_textures_[index] = nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];
  // RGBA8 matches the backend's canonical color render-target storage (bytes
  // == guest EDRAM bytes) so IssueSwap can blit it in directly; the present
  // pass SAMPLES it into the BGRA8 drawable, so the format difference there is
  // fine. Written by the producer's refresher and sampled by the present blit.
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  if (!texture) {
    REXLOG_ERROR("MetalPresenter: failed to create {}x{} guest output texture", width,
           height);
    return nullptr;
  }
  guest_output_textures_[index] = (__bridge_retained void*)texture;
  guest_output_texture_widths_[index] = width;
  guest_output_texture_heights_[index] = height;
  return guest_output_textures_[index];
}

const char* MetalPresenter::GetGuestOutputPaintEffectName(
    GuestOutputPaintEffect effect) {
  using Effect = GuestOutputPaintEffect;
  switch (effect) {
    case Effect::kBilinear:
      return "bilinear";
    case Effect::kBilinearDither:
      return "bilinear+dither";
    default:
      return "unknown";
  }
}

bool MetalPresenter::EnsureGuestOutputPaintResources() {
  if (guest_output_paint_resources_loaded_) {
    return true;
  }
  if (guest_output_paint_resources_failed_) {
    return false;
  }
  // Fail once, loudly, rather than retrying every frame.
  guest_output_paint_resources_failed_ = true;
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;

  id<MTLFunction> vs = LoadEntryFunction(
      device, guest_output_triangle_strip_rect_vs_metallib,
      sizeof(guest_output_triangle_strip_rect_vs_metallib),
      "guest_output_triangle_strip_rect_vs");
  if (!vs) {
    return false;
  }
  struct PixelShader {
    const uint8_t* metallib;
    size_t size;
  };
  static const PixelShader kPixelShaders[] = {
      {guest_output_bilinear_ps_metallib,
       sizeof(guest_output_bilinear_ps_metallib)},
      {guest_output_bilinear_dither_ps_metallib,
       sizeof(guest_output_bilinear_dither_ps_metallib)},
  };
  static_assert(rex::countof(kPixelShaders) ==
                    size_t(GuestOutputPaintEffect::kCount),
                "A pixel shader is needed for every guest output effect, in "
                "GuestOutputPaintEffect order");
  std::array<id<MTLFunction>, size_t(GuestOutputPaintEffect::kCount)> ps;
  for (size_t i = 0; i < ps.size(); ++i) {
    ps[i] = LoadEntryFunction(
        device, kPixelShaders[i].metallib, kPixelShaders[i].size,
        GetGuestOutputPaintEffectName(GuestOutputPaintEffect(i)));
    if (!ps[i]) {
      return false;
    }
  }

  MTLSamplerDescriptor* sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
  sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
  sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> sampler =
      [device newSamplerStateWithDescriptor:sampler_desc];
  if (!sampler) {
    REXLOG_ERROR("MetalPresenter: failed to create the guest output sampler");
    return false;
  }

  guest_output_paint_vs_ = (__bridge_retained void*)vs;
  for (size_t i = 0; i < ps.size(); ++i) {
    guest_output_paint_ps_[i] = (__bridge_retained void*)ps[i];
  }
  guest_output_paint_sampler_ = (__bridge_retained void*)sampler;
  guest_output_paint_resources_failed_ = false;
  guest_output_paint_resources_loaded_ = true;
  return true;
}

void* MetalPresenter::GetGuestOutputPaintPipeline(
    GuestOutputPaintEffect effect) {
  void*& pipeline_ref = guest_output_paint_pipelines_[size_t(effect)];
  if (pipeline_ref) {
    return pipeline_ref;
  }
  // The functions and the sampler are loaded once; without them the descriptor below carries a
  // nil vertex function, which Metal rejects with an assertion rather than an NSError.
  if (!guest_output_paint_resources_loaded_ && !EnsureGuestOutputPaintResources()) {
    return nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLRenderPipelineDescriptor* desc =
      [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = (__bridge id<MTLFunction>)guest_output_paint_vs_;
  desc.fragmentFunction =
      (__bridge id<MTLFunction>)guest_output_paint_ps_[size_t(effect)];
  desc.colorAttachments[0].pixelFormat = kDrawablePixelFormat;
  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:desc error:&error];
  if (!pipeline) {
    REXLOG_ERROR("MetalPresenter: failed to create the {} pipeline: {}",
           GetGuestOutputPaintEffectName(effect),
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return nullptr;
  }
  pipeline_ref = (__bridge_retained void*)pipeline;
  return pipeline_ref;
}

void* MetalPresenter::EnsureMetalFxScaler(uint32_t input_width,
                                          uint32_t input_height,
                                          uint32_t output_width,
                                          uint32_t output_height,
                                          void*& output_texture_out) {
  if (metalfx_scaler_ && metalfx_input_width_ == input_width &&
      metalfx_input_height_ == input_height &&
      metalfx_output_width_ == output_width &&
      metalfx_output_height_ == output_height) {
    output_texture_out = metalfx_output_texture_;
    return metalfx_scaler_;
  }
  const uint32_t sizes[4] = {input_width, input_height, output_width,
                             output_height};
  if (std::equal(std::begin(sizes), std::end(sizes),
                 std::begin(metalfx_failed_sizes_))) {
    return nullptr;
  }
  if (metalfx_scaler_) {
    CFRelease(metalfx_scaler_);
    metalfx_scaler_ = nullptr;
  }
  if (metalfx_output_texture_) {
    CFRelease(metalfx_output_texture_);
    metalfx_output_texture_ = nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  // 10bpc output like the Vulkan presenter's intermediates, 8bpc if MetalFX
  // can't.
  id<MTLFXSpatialScaler> scaler = nil;
  for (MTLPixelFormat output_format :
       {kIntermediatePixelFormat, MTLPixelFormatRGBA8Unorm}) {
    MTLFXSpatialScalerDescriptor* desc =
        [[MTLFXSpatialScalerDescriptor alloc] init];
    desc.inputWidth = input_width;
    desc.inputHeight = input_height;
    desc.outputWidth = output_width;
    desc.outputHeight = output_height;
    // The guest output mailbox texture.
    desc.colorTextureFormat = MTLPixelFormatRGBA8Unorm;
    desc.outputTextureFormat = output_format;
    // The guest output is gamma-encoded.
    desc.colorProcessingMode =
        MTLFXSpatialScalerColorProcessingModePerceptual;
    scaler = [desc newSpatialScalerWithDevice:device];
    if (scaler) {
      MTLTextureDescriptor* texture_desc = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:output_format
                                       width:output_width
                                      height:output_height
                                   mipmapped:NO];
      texture_desc.usage = scaler.outputTextureUsage | MTLTextureUsageShaderRead;
      texture_desc.storageMode = MTLStorageModePrivate;
      id<MTLTexture> output_texture =
          [device newTextureWithDescriptor:texture_desc];
      if (!output_texture) {
        scaler = nil;
        break;
      }
      metalfx_output_texture_ = (__bridge_retained void*)output_texture;
      break;
    }
  }
  if (!scaler) {
    REXLOG_WARN("MetalPresenter: MetalFX can't scale {}x{} to {}x{}; painting "
           "bilinear instead",
           input_width, input_height, output_width, output_height);
    std::copy(std::begin(sizes), std::end(sizes),
              std::begin(metalfx_failed_sizes_));
    return nullptr;
  }
  REXLOG_INFO("MetalPresenter: MetalFX spatial scaling {}x{} to {}x{}",
         input_width, input_height, output_width, output_height);
  metalfx_scaler_ = (__bridge_retained void*)scaler;
  metalfx_input_width_ = input_width;
  metalfx_input_height_ = input_height;
  metalfx_output_width_ = output_width;
  metalfx_output_height_ = output_height;
  output_texture_out = metalfx_output_texture_;
  return metalfx_scaler_;
}

bool MetalPresenter::EncodeGuestOutputPaintEffect(
    void* render_encoder, const GuestOutputPaintFlow& flow,
    void* source_texture, uint32_t target_width, uint32_t target_height) {
  GuestOutputPaintEffect effect = flow.effect;
  void* pipeline = GetGuestOutputPaintPipeline(effect);
  if (!pipeline) {
    return false;
  }
  id<MTLRenderCommandEncoder> encoder =
      (__bridge id<MTLRenderCommandEncoder>)render_encoder;
  [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
                                      pipeline];

  // The rectangle in normalized device coordinates. Unlike Vulkan's, Metal's
  // Y axis points up, so the rectangle grows downwards from its top edge. The
  // letterbox around it is the pass's black clear.
  const std::pair<uint32_t, uint32_t>& output_size = flow.effect_output_size;
  float x_to_ndc = 2.0f / float(target_width);
  float y_to_ndc = 2.0f / float(target_height);
  float rect[4] = {
      -1.0f + float(flow.output_x) * x_to_ndc,
      1.0f - float(flow.output_y) * y_to_ndc,
      float(output_size.first) * x_to_ndc,
      -float(output_size.second) * y_to_ndc,
  };
  [encoder setVertexBytes:rect length:sizeof(rect) atIndex:0];

  // MSL lays the effect constants out from offset 0 of their own buffer, and
  // may pad the structure past the C++ size, so pass a zero-padded block.
  union {
    BilinearConstants bilinear;
    uint8_t padded[32];
  } constants = {};
  switch (effect) {
    case GuestOutputPaintEffect::kBilinear:
    case GuestOutputPaintEffect::kBilinearDither:
      constants.bilinear.Initialize(flow);
      break;
    default:
      break;
  }
  [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
  [encoder setFragmentTexture:(__bridge id<MTLTexture>)source_texture
                      atIndex:0];
  [encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)
                                       guest_output_paint_sampler_
                           atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
              vertexStart:0
              vertexCount:4];
  return true;
}

bool MetalPresenter::DriverRefreshGuestOutput(
    uint32_t mailbox_index, uint32_t frontbuffer_width,
    uint32_t frontbuffer_height,
    ::rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher,
    bool& is_8bpc_out_ref) {
  void* texture =
      EnsureGuestOutputTexture(mailbox_index, frontbuffer_width,
                               frontbuffer_height);
  if (!texture) {
    return false;
  }
  // Hand the mailbox texture to the producer (IssueSwap) to fill with the
  // current frontbuffer; it records + submits its own Metal work.
  MetalGuestOutputRefreshContext context(is_8bpc_out_ref, texture);
  // The mailbox texture is RGBA8, so whatever the guest format, the image has
  // no more than 8 bits per channel and needs no dithering at 1:1.
  context.SetIs8bpc(true);
  return refresher(context);
}

void MetalPresenter::LogGuestOutputPaintFlowIfChanged(
    const GuestOutputPaintFlow& flow) {
  if (flow.has_effect == last_logged_has_effect_ &&
      (!flow.has_effect ||
       (flow.effect == last_logged_effect_ &&
        flow.effect_output_size == last_logged_effect_output_size_))) {
    return;
  }
  last_logged_has_effect_ = flow.has_effect;
  last_logged_effect_ = flow.effect;
  last_logged_effect_output_size_ = flow.effect_output_size;
  if (flow.has_effect) {
    REXLOG_INFO("MetalPresenter: guest output {}x{} painted with {} to {}x{}",
           flow.properties.frontbuffer_width,
           flow.properties.frontbuffer_height,
           GetGuestOutputPaintEffectName(flow.effect), flow.effect_output_size.first,
           flow.effect_output_size.second);
  } else {
    REXLOG_INFO("MetalPresenter: guest output {}x{} painted with nothing",
           flow.properties.frontbuffer_width,
           flow.properties.frontbuffer_height);
  }
}

gpu::Presenter::PaintResult MetalPresenter::DriverPaintAndPresent(
    bool execute_ui_drawers) {
  void* layer_ptr;
  {
    std::lock_guard<std::mutex> lock(surface_mutex_);
    layer_ptr = metal_layer_;
  }
  if (!layer_ptr) {
    return PaintResult::kNotPresented;
  }

  @autoreleasepool {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_ptr;
    {
      // A freshly connected layer during a fullscreen transition has no drawables yet; nextDrawable would block the UI
      // thread inside window setup, and since that thread is the one that has to finish the transition, it would never
      // return. Skip until the transition has had a chance to complete.
      const auto connected_at = [this] {
        std::lock_guard<std::mutex> lock(surface_mutex_);
        return surface_connected_at_;
      }();
      if (std::chrono::steady_clock::now() - connected_at < std::chrono::milliseconds(400)) {
        return PaintResult::kNotPresented;
      }
    }
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
      return PaintResult::kNotPresented;
    }

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue_;
    // Per-encoder execution status so a present-path GPU fault is attributed
    // (Apple GPUs; see MetalEncoderExecutionStatusEnabled for the AMD cost).
    MTLCommandBufferDescriptor* cb_desc =
        [[MTLCommandBufferDescriptor alloc] init];
    static const bool s_encoder_status = [queue] {
      if (const char* v = std::getenv("XE_METAL_ENCODER_STATUS")) {
        return v[0] == '1';
      }
      return bool([queue.device supportsFamily:MTLGPUFamilyApple7]);
    }();
    if (s_encoder_status) {
      cb_desc.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
    }
    id<MTLCommandBuffer> command_buffer =
        [queue commandBufferWithDescriptor:cb_desc];
    command_buffer.label = @"presenter:paint";

    if (const char* clear = std::getenv("REX_METAL_PRESENT_CLEAR"); clear && *clear == '1') {
      // Minimal present: clear the drawable and commit. Isolates "the layer/queue cannot present at all" from "the guest
      // output paint path hangs the GPU", which look identical when frames never complete.
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = drawable.texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.35, 0.0, 1.0);
      id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
      [encoder endEncoding];
      [command_buffer presentDrawable:drawable];
      [command_buffer commit];
      return PaintResult::kPresented;
    }

    // Take the ready guest-output image (if any). The consumer lock keeps the
    // acquired mailbox slot from being recycled while we sample it, so hold it
    // through commit.
    uint32_t mailbox_index = UINT32_MAX;
    GuestOutputProperties guest_output_properties;
    GuestOutputPaintConfig guest_output_paint_config;
    std::unique_lock<std::mutex> consumer_lock = ConsumeGuestOutput(
        mailbox_index, &guest_output_properties, &guest_output_paint_config);
    id<MTLTexture> guest_output_texture = nil;
    if (mailbox_index != UINT32_MAX &&
        mailbox_index < guest_output_textures_.size() &&
        guest_output_textures_[mailbox_index]) {
      guest_output_texture =
          (__bridge id<MTLTexture>)guest_output_textures_[mailbox_index];
    }

    uint32_t drawable_width = uint32_t(drawable.texture.width);
    uint32_t drawable_height = uint32_t(drawable.texture.height);
    GuestOutputPaintFlow flow = GetGuestOutputPaintFlow(
        guest_output_properties, drawable_width, drawable_height,
        kMaxRenderTargetSize, kMaxRenderTargetSize, guest_output_paint_config);
    LogGuestOutputPaintFlowIfChanged(flow);

    void* effect_source = (__bridge void*)guest_output_texture;
    // MetalFX spatial scaling: the flow painted for it is a bilinear stretch
    // to the output size. MetalFX upscales to that size instead, and the
    // bilinear pass then copies its output 1:1.
    if (guest_output_texture && flow.has_effect &&
        flow.effect == GuestOutputPaintEffect::kBilinear &&
        SupportsGuestOutputPaintEffect(GuestOutputPaintEffect::kBilinear)) {
      uint32_t input_width = guest_output_properties.frontbuffer_width;
      uint32_t input_height = guest_output_properties.frontbuffer_height;
      const std::pair<uint32_t, uint32_t>& output_size = flow.effect_output_size;
      // Upscaling only.
      if (output_size.first >= input_width &&
          output_size.second >= input_height &&
          (output_size.first > input_width || output_size.second > input_height)) {
        void* metalfx_output = nullptr;
        void* scaler_ptr =
            EnsureMetalFxScaler(input_width, input_height, output_size.first,
                                output_size.second, metalfx_output);
        if (scaler_ptr) {
          id<MTLFXSpatialScaler> scaler =
              (__bridge id<MTLFXSpatialScaler>)scaler_ptr;
          scaler.colorTexture = guest_output_texture;
          scaler.outputTexture = (__bridge id<MTLTexture>)metalfx_output;
          scaler.inputContentWidth = input_width;
          scaler.inputContentHeight = input_height;
          [scaler encodeToCommandBuffer:command_buffer];
          effect_source = metalfx_output;
        }
      }
    }

    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // Black letterbox/pillarbox bars around the aspect-corrected guest image.
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (flow.has_effect && effect_source) {
      EncodeGuestOutputPaintEffect((__bridge void*)encoder, flow, effect_source,
                                   drawable_width, drawable_height);
    }

    // UI overlays (ImGui: emulator dialogs AND guest system dialogs) draw into
    // this same pass, on top of the guest image, before the encoder ends. They
    // composite into the drawable only -- no guest state is reachable from
    // here, which is what makes overlay drawing incapable of perturbing guest
    // output.
    //
    // Not drawing these is not cosmetic: xeXamDispatchDialog blocks its guest
    // thread on fence.Wait() until an ImGui-drawn modal is dismissed, so a
    // title that raises a system dialog hangs forever if nothing renders it.
    if (execute_ui_drawers) {
      MetalUIDrawContext ui_draw_context(
          *this, uint32_t(drawable.texture.width),
          uint32_t(drawable.texture.height), (__bridge void*)command_buffer,
          (__bridge void*)encoder);
      ExecuteOverlayLayersFromUIThread(ui_draw_context);
    }

    [encoder endEncoding];

    // A silent GPU fault in the present blit leaves the window on the last good
    // frame while the game keeps running — reads as a "GPU freeze". Log it.
    // Also report the paint's GPU time every 600 paints, which is what the
    // upscaling costs on top of the guest's own rendering.
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      static std::atomic<uint64_t> s_paint_gpu_ns{0};
      static std::atomic<uint32_t> s_paint_count{0};
      if (cb.status == MTLCommandBufferStatusCompleted &&
          cb.GPUEndTime > cb.GPUStartTime) {
        uint64_t paint_gpu_ns =
            uint64_t((cb.GPUEndTime - cb.GPUStartTime) * 1e9);
        uint64_t gpu_ns =
            s_paint_gpu_ns.fetch_add(paint_gpu_ns) + paint_gpu_ns;
        uint32_t paint_count = s_paint_count.fetch_add(1) + 1;
        if (paint_count >= 600) {
          REXLOG_INFO("MetalPresenter: paint GPU time {:.3f} ms average over {} "
                 "paints",
                 double(gpu_ns) / double(paint_count) * 1e-6, paint_count);
          s_paint_gpu_ns.store(0);
          s_paint_count.store(0);
        }
      }
      {
        // First frames: does the present command buffer actually complete, and when? A drawable is only recycled on
        // completion, so three frames that never complete block nextDrawable forever and the window stays black.
        static MTLCommandBufferStatus s_last_status = (MTLCommandBufferStatus)-1;
        static uint32_t s_completions = 0;
        if (s_completions < 5 || cb.status != s_last_status) {
          REXLOG_INFO("MetalPresenter: present {} status {} gpu {:.3f} ms", s_completions, int(cb.status),
                      double(cb.GPUEndTime - cb.GPUStartTime) * 1e3);
          s_last_status = cb.status;
        }
        ++s_completions;
      }
      if (cb.status == MTLCommandBufferStatusError) {
        static uint32_t s_present_errors = 0;
        if (s_present_errors++ < 32) {
          NSError* err = cb.error;
          REXLOG_ERROR("Metal GPU FAULT (present): {} (code {})",
                 err.localizedDescription ? err.localizedDescription.UTF8String
                                          : "(no description)",
                 err ? long(err.code) : -1L);
        }
      }
    }];
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
    // REX_SCREENSHOT=<frame>[,<frame>...]: write the guest output the presenter just put up. This lives here rather than
    // in the generic presenter because the Metal backend presents from its own thread, so nothing drives the SDK's
    // paint path - the frames exist, they are simply not produced where that hook looks.
    {
      static const std::vector<uint32_t> screenshot_frames = [] {
        std::vector<uint32_t> frames;
        if (const char* spec = std::getenv("REX_SCREENSHOT")) {
          std::stringstream list(spec);
          std::string item;
          while (std::getline(list, item, ',')) {
            frames.push_back(uint32_t(std::strtoul(item.c_str(), nullptr, 10)));
          }
        }
        return frames;
      }();
      static uint32_t presented_count = 0;
      ++presented_count;
      if (!screenshot_frames.empty() &&
          std::find(screenshot_frames.begin(), screenshot_frames.end(), presented_count) != screenshot_frames.end()) {
        RawImage image;
        if (ReadbackTextureToImage((__bridge void*)guest_output_texture,
                                   uint32_t(guest_output_texture.width),
                                   uint32_t(guest_output_texture.height), image) &&
            image.width && image.height) {
          char path[512];
          std::snprintf(path, sizeof(path), "guest_output_%u.ppm", presented_count);
          if (FILE* file = std::fopen(path, "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
            for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
              const unsigned char rgb[3] = {image.data[i], image.data[i + 1], image.data[i + 2]};
              std::fwrite(rgb, 1, 3, file);
            }
            std::fclose(file);
            REXLOG_INFO("Screenshot: guest_output_{}.ppm ({}x{})", presented_count, image.width, image.height);
          }
        } else {
          REXLOG_WARN("Screenshot: no guest output to capture at presented frame {}", presented_count);
        }
      }
    }
    consumer_lock.unlock();
  }

  return PaintResult::kPresented;
}

}  // namespace metal
}  // namespace ui
}  // namespace rex
