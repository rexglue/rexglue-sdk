/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: Full ImGui rendering pipeline
 */

#import <Metal/Metal.h>

#include <cstring>

#include <rex/ui/metal/metal_immediate_drawer.h>
#include <rex/ui/metal/metal_presenter.h>
#include <rex/ui/metal/metal_provider.h>
#include <rex/logging.h>

namespace rex {
namespace ui {
namespace metal {

// ============================================================================
// ImGui-compatible vertex/fragment shaders for Metal
// ============================================================================
static const char* kImGuiShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct ImGuiVertex {
  float2 position [[attribute(0)]];
  float2 texcoord [[attribute(1)]];
  uchar4 color    [[attribute(2)]];
};

struct VertexOut {
  float4 position [[position]];
  float2 texcoord;
  float4 color;
};

struct Uniforms {
  float2 viewport_size_inv;  // 1.0 / viewport_size
};

vertex VertexOut imgui_vertex(ImGuiVertex in [[stage_in]],
                               constant Uniforms& uniforms [[buffer(1)]]) {
  VertexOut out;
  // Convert from pixel coordinates to NDC.
  // ImGui uses top-left origin with Y-down.
  float2 ndc;
  ndc.x = in.position.x * uniforms.viewport_size_inv.x * 2.0 - 1.0;
  ndc.y = 1.0 - in.position.y * uniforms.viewport_size_inv.y * 2.0;
  out.position = float4(ndc, 0.0, 1.0);
  out.texcoord = in.texcoord;
  // Convert RGBA8 color to float4.
  out.color = float4(in.color) / 255.0;
  return out;
}

fragment float4 imgui_fragment(VertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]],
                                sampler samp [[sampler(0)]]) {
  float4 tex_color = tex.sample(samp, in.texcoord);
  return in.color * tex_color;
}
)";

// ============================================================================
// MetalImmediateTexture
// ============================================================================

MetalImmediateTexture::MetalImmediateTexture(
    uint32_t width, uint32_t height, ImmediateTextureFilter filter,
    bool is_repeated)
    : ImmediateTexture(width, height),
      filter_(filter),
      is_repeated_(is_repeated) {}

MetalImmediateTexture::~MetalImmediateTexture() {
  texture_ = nil;
  sampler_ = nil;
}

// ============================================================================
// MetalImmediateDrawer
// ============================================================================

std::unique_ptr<MetalImmediateDrawer> MetalImmediateDrawer::Create(
    MetalProvider& provider) {
  auto drawer = std::unique_ptr<MetalImmediateDrawer>(
      new MetalImmediateDrawer(provider));
  if (!drawer->Initialize()) {
    return nullptr;
  }
  return drawer;
}

MetalImmediateDrawer::MetalImmediateDrawer(MetalProvider& provider)
    : provider_(provider) {}

MetalImmediateDrawer::~MetalImmediateDrawer() {
  white_texture_ = nil;
  sampler_bilinear_ = nil;
  sampler_nearest_ = nil;
  sampler_bilinear_repeat_ = nil;
  sampler_nearest_repeat_ = nil;
  pipeline_state_line_ = nil;
  pipeline_state_triangle_ = nil;
  depth_stencil_disabled_ = nil;
  vertex_buffer_ = nil;
  index_buffer_ = nil;
}

bool MetalImmediateDrawer::Initialize() {
  id<MTLDevice> device = provider_.device();
  if (!device) return false;

  const auto& caps = provider_.caps();

  // --- Compile shaders ---
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kImGuiShaderSource];
  id<MTLLibrary> library = [device newLibraryWithSource:source
                                                options:nil
                                                  error:&error];
  if (!library) {
    REXLOG_ERROR("MetalImmediateDrawer: Failed to compile shaders: {}",
           error ? [[error localizedDescription] UTF8String] : "unknown");
    return false;
  }

  id<MTLFunction> vertex_func =
      [library newFunctionWithName:@"imgui_vertex"];
  id<MTLFunction> fragment_func =
      [library newFunctionWithName:@"imgui_fragment"];
  if (!vertex_func || !fragment_func) {
    REXLOG_ERROR("MetalImmediateDrawer: Failed to find shader functions");
    return false;
  }

  // --- Vertex descriptor ---
  MTLVertexDescriptor* vertex_desc =
      [[MTLVertexDescriptor alloc] init];
  // Position: float2 at offset 0
  vertex_desc.attributes[0].format = MTLVertexFormatFloat2;
  vertex_desc.attributes[0].offset = offsetof(ImmediateVertex, x);
  vertex_desc.attributes[0].bufferIndex = 0;
  // Texcoord: float2 at offset 8
  vertex_desc.attributes[1].format = MTLVertexFormatFloat2;
  vertex_desc.attributes[1].offset = offsetof(ImmediateVertex, u);
  vertex_desc.attributes[1].bufferIndex = 0;
  // Color: uchar4 at offset 16
  vertex_desc.attributes[2].format = MTLVertexFormatUChar4;
  vertex_desc.attributes[2].offset = offsetof(ImmediateVertex, color);
  vertex_desc.attributes[2].bufferIndex = 0;
  // Layout
  vertex_desc.layouts[0].stride = sizeof(ImmediateVertex);
  vertex_desc.layouts[0].stepRate = 1;
  vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

  // --- Triangle pipeline ---
  {
    MTLRenderPipelineDescriptor* desc =
        [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vertex_func;
    desc.fragmentFunction = fragment_func;
    desc.vertexDescriptor = vertex_desc;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Alpha blending (standard ImGui blending).
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    desc.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    desc.colorAttachments[0].sourceAlphaBlendFactor =
        MTLBlendFactorSourceAlpha;
    desc.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    desc.label = @"ImGui_Triangle";

    pipeline_state_triangle_ =
        [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline_state_triangle_) {
      REXLOG_ERROR("MetalImmediateDrawer: Failed to create triangle pipeline: {}",
             error ? [[error localizedDescription] UTF8String] : "unknown");
      return false;
    }
  }

  // Line pipeline reuses the same PSO — Metal specifies primitive type at
  // draw time, not in the pipeline state object.
  pipeline_state_line_ = pipeline_state_triangle_;

  // --- Depth/stencil state (disabled for UI) ---
  {
    MTLDepthStencilDescriptor* ds_desc =
        [[MTLDepthStencilDescriptor alloc] init];
    ds_desc.depthCompareFunction = MTLCompareFunctionAlways;
    ds_desc.depthWriteEnabled = NO;
    depth_stencil_disabled_ = [device newDepthStencilStateWithDescriptor:ds_desc];
  }

  // --- Samplers ---
  {
    MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];

    // Nearest, clamp.
    desc.minFilter = MTLSamplerMinMagFilterNearest;
    desc.magFilter = MTLSamplerMinMagFilterNearest;
    desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_nearest_ = [device newSamplerStateWithDescriptor:desc];

    // Bilinear, clamp.
    desc.minFilter = MTLSamplerMinMagFilterLinear;
    desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_bilinear_ = [device newSamplerStateWithDescriptor:desc];

    // Nearest, repeat.
    desc.minFilter = MTLSamplerMinMagFilterNearest;
    desc.magFilter = MTLSamplerMinMagFilterNearest;
    desc.sAddressMode = MTLSamplerAddressModeRepeat;
    desc.tAddressMode = MTLSamplerAddressModeRepeat;
    sampler_nearest_repeat_ = [device newSamplerStateWithDescriptor:desc];

    // Bilinear, repeat.
    desc.minFilter = MTLSamplerMinMagFilterLinear;
    desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_bilinear_repeat_ = [device newSamplerStateWithDescriptor:desc];
  }

  // --- 1x1 white fallback texture ---
  {
    MTLTextureDescriptor* tex_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                    width:1
                                   height:1
                                mipmapped:NO];
    tex_desc.usage = MTLTextureUsageShaderRead;
    if (caps.PreferSharedStorage()) {
      tex_desc.storageMode = MTLStorageModeShared;
    } else {
      tex_desc.storageMode = MTLStorageModeManaged;
    }
    white_texture_ = [device newTextureWithDescriptor:tex_desc];
    if (white_texture_) {
      uint32_t white = 0xFFFFFFFF;
      [white_texture_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                        mipmapLevel:0
                          withBytes:&white
                        bytesPerRow:4];
    }
  }

  // --- Dynamic vertex/index buffers ---
  {
    MTLResourceOptions buf_opts = caps.PreferSharedStorage()
        ? (MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined)
        : MTLResourceStorageModeManaged;

    vertex_buffer_ = [device newBufferWithLength:kVertexBufferSize
                                         options:buf_opts];
    vertex_buffer_.label = @"ImGui_VertexBuffer";

    index_buffer_ = [device newBufferWithLength:kIndexBufferSize
                                        options:buf_opts];
    index_buffer_.label = @"ImGui_IndexBuffer";
  }

  REXLOG_INFO("MetalImmediateDrawer: Initialized with full pipeline");
  return true;
}

std::unique_ptr<ImmediateTexture> MetalImmediateDrawer::CreateTexture(
    uint32_t width, uint32_t height, ImmediateTextureFilter filter,
    bool is_repeated, const uint8_t* data) {
  id<MTLDevice> device = provider_.device();
  if (!device || width == 0 || height == 0) return nullptr;

  auto texture = std::make_unique<MetalImmediateTexture>(
      width, height, filter, is_repeated);

  const auto& caps = provider_.caps();

  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];
  desc.usage = MTLTextureUsageShaderRead;
  if (caps.PreferSharedStorage()) {
    desc.storageMode = MTLStorageModeShared;
  } else {
    desc.storageMode = MTLStorageModeManaged;
  }

  id<MTLTexture> mtl_texture = [device newTextureWithDescriptor:desc];
  if (!mtl_texture) {
    REXLOG_ERROR("MetalImmediateDrawer: Failed to create {}x{} texture",
           width, height);
    return nullptr;
  }

  if (data) {
    [mtl_texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:data
                   bytesPerRow:width * 4];
  }

  texture->set_texture(mtl_texture);

  // Select appropriate sampler.
  id<MTLSamplerState> sampler;
  if (is_repeated) {
    sampler = (filter == ImmediateTextureFilter::kLinear)
                  ? sampler_bilinear_repeat_
                  : sampler_nearest_repeat_;
  } else {
    sampler = (filter == ImmediateTextureFilter::kLinear)
                  ? sampler_bilinear_
                  : sampler_nearest_;
  }
  texture->set_sampler(sampler);

  return texture;
}

void MetalImmediateDrawer::Begin(UIDrawContext& ui_draw_context,
                                  float coordinate_space_width,
                                  float coordinate_space_height) {
  // Call base class to store context and coordinate space dimensions.
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width,
                         coordinate_space_height);

  // Extract the Metal render encoder from the UI draw context.
  auto& metal_context = static_cast<metal::MetalUIDrawContext&>(ui_draw_context);
  current_encoder_ = metal_context.encoder();

  current_render_target_width_ = int(ui_draw_context.render_target_width());
  current_render_target_height_ = int(ui_draw_context.render_target_height());
  // Reset offsets at the start of each frame. The previous frame's command
  // buffer has been committed (and typically completed) by the time the
  // presenter calls Begin() again, so reusing the buffer from offset 0 is
  // safe. This prevents cross-frame races when wrapping mid-buffer.
  batch_vertex_offset_ = 0;
  batch_index_offset_ = 0;
}

void MetalImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  if (!current_encoder_) return;

  // Upload vertices to the dynamic buffer.
  size_t vertex_bytes = batch.vertex_count * sizeof(ImmediateVertex);
  if (vertex_bytes > kVertexBufferSize) {
    REXLOG_ERROR("MetalImmediateDrawer: Batch too large ({} bytes) for vertex "
           "buffer ({} bytes) — skipping",
           vertex_bytes, kVertexBufferSize);
    return;
  }
  if (batch_vertex_offset_ + vertex_bytes > kVertexBufferSize) {
    // Wrap to start of buffer. Previous batches in this frame have already
    // been encoded into the command buffer so their data won't be read again
    // until the GPU finishes (which happens after commit).
    batch_vertex_offset_ = 0;
  }

  // Track batch start offsets for didModifyRange in EndDrawBatch.
  batch_vertex_start_ = batch_vertex_offset_;
  batch_index_start_ = batch_index_offset_;

  uint8_t* vb_ptr = static_cast<uint8_t*>([vertex_buffer_ contents]);
  std::memcpy(vb_ptr + batch_vertex_offset_, batch.vertices, vertex_bytes);

  // Upload indices if present.
  batch_has_indices_ = (batch.indices != nullptr && batch.index_count > 0);
  if (batch_has_indices_) {
    size_t index_bytes = batch.index_count * sizeof(uint16_t);
    if (batch_index_offset_ + index_bytes > kIndexBufferSize) {
      REXLOG_WARN("MetalImmediateDrawer: Index buffer overflow");
      batch_index_offset_ = 0;
      batch_index_start_ = 0;
    }

    uint8_t* ib_ptr = static_cast<uint8_t*>([index_buffer_ contents]);
    std::memcpy(ib_ptr + batch_index_offset_, batch.indices, index_bytes);
    batch_index_end_ = batch_index_offset_ + index_bytes;
  }

  batch_vertex_count_ = batch.vertex_count;

  // Set the vertex buffer.
  [current_encoder_ setVertexBuffer:vertex_buffer_
                             offset:batch_vertex_offset_
                            atIndex:0];

  // Set viewport uniforms.
  float uniforms[2] = {
      1.0f / float(current_render_target_width_),
      1.0f / float(current_render_target_height_),
  };
  [current_encoder_ setVertexBytes:uniforms length:sizeof(uniforms) atIndex:1];

  // Set depth/stencil state (disabled).
  [current_encoder_ setDepthStencilState:depth_stencil_disabled_];

  // Set viewport.
  MTLViewport viewport = {
      0, 0,
      double(current_render_target_width_),
      double(current_render_target_height_),
      0.0, 1.0};
  [current_encoder_ setViewport:viewport];
}

void MetalImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!current_encoder_) return;
  if (draw.count == 0) return;

  // Select pipeline.
  // Single PSO for both primitives — type is specified at draw time.
  [current_encoder_ setRenderPipelineState:pipeline_state_triangle_];

  // Set texture + sampler.
  auto* metal_texture = static_cast<MetalImmediateTexture*>(draw.texture);
  if (metal_texture && metal_texture->texture()) {
    [current_encoder_ setFragmentTexture:metal_texture->texture() atIndex:0];
    [current_encoder_ setFragmentSamplerState:metal_texture->sampler()
                                       atIndex:0];
  } else {
    // Fallback white texture.
    [current_encoder_ setFragmentTexture:white_texture_ atIndex:0];
    [current_encoder_ setFragmentSamplerState:sampler_nearest_ atIndex:0];
  }

  // Scissor rect.
  if (draw.scissor) {
    MTLScissorRect scissor;
    scissor.x = std::max(0, int(draw.scissor_left));
    scissor.y = std::max(0, int(draw.scissor_top));
    uint32_t right = std::max(0, int(draw.scissor_right));
    uint32_t bottom = std::max(0, int(draw.scissor_bottom));
    right = std::min(right, uint32_t(current_render_target_width_));
    bottom = std::min(bottom, uint32_t(current_render_target_height_));
    scissor.width = (right > scissor.x) ? (right - scissor.x) : 0;
    scissor.height = (bottom > scissor.y) ? (bottom - scissor.y) : 0;
    // Metal requires non-zero scissor.
    if (scissor.width == 0) scissor.width = 1;
    if (scissor.height == 0) scissor.height = 1;
    [current_encoder_ setScissorRect:scissor];
  } else {
    MTLScissorRect scissor = {
        0, 0,
        NSUInteger(current_render_target_width_),
        NSUInteger(current_render_target_height_)};
    [current_encoder_ setScissorRect:scissor];
  }

  // Draw.
  MTLPrimitiveType prim_type =
      (draw.primitive_type == ImmediatePrimitiveType::kLines)
          ? MTLPrimitiveTypeLine
          : MTLPrimitiveTypeTriangle;

  if (batch_has_indices_) {
    size_t index_byte_offset =
        batch_index_offset_ + draw.index_offset * sizeof(uint16_t);
    [current_encoder_
        drawIndexedPrimitives:prim_type
                   indexCount:draw.count
                    indexType:MTLIndexTypeUInt16
                  indexBuffer:index_buffer_
            indexBufferOffset:index_byte_offset
                instanceCount:1
                   baseVertex:draw.base_vertex
                 baseInstance:0];
  } else {
    [current_encoder_ drawPrimitives:prim_type
                         vertexStart:draw.base_vertex
                         vertexCount:draw.count];
  }
}

void MetalImmediateDrawer::EndDrawBatch() {
  // Advance vertex offset for next batch.
  batch_vertex_offset_ += batch_vertex_count_ * sizeof(ImmediateVertex);

  // For managed storage, notify Metal of the exact modified ranges.
  if (vertex_buffer_.storageMode == MTLStorageModeManaged) {
    size_t vertex_bytes = batch_vertex_count_ * sizeof(ImmediateVertex);
    if (vertex_bytes > 0) {
      [vertex_buffer_ didModifyRange:NSMakeRange(batch_vertex_start_,
                                                  vertex_bytes)];
    }
  }
  if (batch_has_indices_ && index_buffer_.storageMode == MTLStorageModeManaged) {
    size_t index_bytes = batch_index_end_ - batch_index_start_;
    if (index_bytes > 0) {
      [index_buffer_ didModifyRange:NSMakeRange(batch_index_start_,
                                                 index_bytes)];
    }
    // Advance index offset now that we've notified.
    batch_index_offset_ = batch_index_end_;
  }
}

void MetalImmediateDrawer::End() {
  current_encoder_ = nil;
  current_render_target_width_ = 0;
  current_render_target_height_ = 0;
  ImmediateDrawer::End();
}

}  // namespace metal
}  // namespace ui
}  // namespace rex
