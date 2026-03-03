/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: Full ImGui rendering pipeline
 */

#pragma once

#include <deque>
#include <memory>
#include <vector>

#include <rex/ui/immediate_drawer.h>

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
@protocol MTLRenderPipelineState;
@protocol MTLDepthStencilState;
@protocol MTLBuffer;
@protocol MTLTexture;
@protocol MTLSamplerState;
@protocol MTLRenderCommandEncoder;
#endif

namespace rex {
namespace ui {
namespace metal {

class MetalProvider;

class MetalImmediateTexture : public ImmediateTexture {
 public:
  MetalImmediateTexture(uint32_t width, uint32_t height,
                        ImmediateTextureFilter filter,
                        bool is_repeated);
  ~MetalImmediateTexture() override;

#ifdef __OBJC__
  id<MTLTexture> texture() const { return texture_; }
  void set_texture(id<MTLTexture> t) { texture_ = t; }
  id<MTLSamplerState> sampler() const { return sampler_; }
  void set_sampler(id<MTLSamplerState> s) { sampler_ = s; }
#endif

 private:
  ImmediateTextureFilter filter_;
  bool is_repeated_;
#ifdef __OBJC__
  id<MTLTexture> texture_ = nil;
  id<MTLSamplerState> sampler_ = nil;
#else
  void* texture_ = nullptr;
  void* sampler_ = nullptr;
#endif
};

class MetalImmediateDrawer : public ImmediateDrawer {
 public:
  static std::unique_ptr<MetalImmediateDrawer> Create(
      MetalProvider& provider);

  ~MetalImmediateDrawer() override;

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width,
                                                   uint32_t height,
                                                   ImmediateTextureFilter filter,
                                                   bool is_repeated,
                                                   const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context,
             float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  explicit MetalImmediateDrawer(MetalProvider& provider);
  bool Initialize();

  MetalProvider& provider_;

  // Per-frame dynamic vertex/index buffer ring.
  static constexpr size_t kVertexBufferSize = 512 * 1024;   // 512 KB
  static constexpr size_t kIndexBufferSize = 256 * 1024;    // 256 KB

#ifdef __OBJC__
  id<MTLRenderPipelineState> pipeline_state_triangle_ = nil;
  id<MTLRenderPipelineState> pipeline_state_line_ = nil;
  id<MTLDepthStencilState> depth_stencil_disabled_ = nil;
  id<MTLSamplerState> sampler_nearest_ = nil;
  id<MTLSamplerState> sampler_bilinear_ = nil;
  id<MTLSamplerState> sampler_nearest_repeat_ = nil;
  id<MTLSamplerState> sampler_bilinear_repeat_ = nil;
  id<MTLTexture> white_texture_ = nil;

  // Dynamic buffers for current frame.
  id<MTLBuffer> vertex_buffer_ = nil;
  id<MTLBuffer> index_buffer_ = nil;

  // Active render encoder (set by presenter before drawing).
  id<MTLRenderCommandEncoder> current_encoder_ = nil;
#else
  void* pipeline_state_triangle_ = nullptr;
  void* pipeline_state_line_ = nullptr;
  void* depth_stencil_disabled_ = nullptr;
  void* sampler_nearest_ = nullptr;
  void* sampler_bilinear_ = nullptr;
  void* sampler_nearest_repeat_ = nullptr;
  void* sampler_bilinear_repeat_ = nullptr;
  void* white_texture_ = nullptr;
  void* vertex_buffer_ = nullptr;
  void* index_buffer_ = nullptr;
  void* current_encoder_ = nullptr;
#endif

  int current_render_target_width_ = 0;
  int current_render_target_height_ = 0;

  // Batch state.
  size_t batch_vertex_offset_ = 0;
  size_t batch_index_offset_ = 0;
  size_t batch_vertex_start_ = 0;   // Start offset of current batch's vertices
  size_t batch_index_start_ = 0;    // Start offset of current batch's indices
  size_t batch_index_end_ = 0;      // End offset of current batch's indices
  int batch_vertex_count_ = 0;
  bool batch_has_indices_ = false;
};

}  // namespace metal
}  // namespace ui
}  // namespace rex
