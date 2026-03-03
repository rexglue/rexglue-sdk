/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: Full draw path + state conversion helpers
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/metal/graphics_system.h>
#include <rex/graphics/metal/pipeline_cache.h>
#include <rex/graphics/metal/primitive_processor.h>
#include <rex/graphics/metal/render_target_cache.h>
#include <rex/graphics/metal/shader.h>
#include <rex/graphics/metal/shared_memory.h>
#include <rex/graphics/metal/texture_cache.h>
#include <rex/ui/metal/metal_provider.h>

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
@protocol MTLCommandBuffer;
@protocol MTLRenderCommandEncoder;
@protocol MTLComputeCommandEncoder;
@protocol MTLBlitCommandEncoder;
@protocol MTLBuffer;
#endif

namespace rex::graphics::metal {

class MetalCommandProcessor : public CommandProcessor {
 public:
  MetalCommandProcessor(MetalGraphicsSystem* graphics_system,
                        system::KernelState* kernel_state);
  ~MetalCommandProcessor() override;

  ui::metal::MetalProvider& GetMetalProvider() const;
  MetalPipelineCache& GetPipelineCache() const { return *pipeline_cache_; }

#ifdef __OBJC__
  id<MTLDevice> GetMetalDevice() const;
  id<MTLCommandQueue> GetMetalCommandQueue() const;
  id<MTLCommandBuffer> GetCurrentCommandBuffer() const {
    return current_command_buffer_;
  }
#endif

  bool Initialize() override;
  void Shutdown() override;

  void ClearCaches() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id, bool blocking) override;

  void TracePlaybackWroteMemory(uint32_t base_ptr,
                                uint32_t length) override;

  void RestoreEdramSnapshot(const void* snapshot) override;

  uint64_t GetCurrentSubmission() const { return submission_current_; }
  uint64_t GetCompletedSubmission() const {
    return submission_completed_.load(std::memory_order_relaxed);
  }

  std::string GetWindowTitleText() const;

  // =========================================================================
  // Xenos → Metal state conversion helpers
  // =========================================================================
  static uint32_t XenosBlendFactorToMetal(xenos::BlendFactor factor);
  static uint32_t XenosBlendOpToMetal(xenos::BlendOp op);
  static uint32_t XenosCompareFuncToMetal(xenos::CompareFunction func);
  static uint32_t XenosStencilOpToMetal(xenos::StencilOp op);

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;

  Shader* LoadShader(xenos::ShaderType shader_type,
                     uint32_t guest_address,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;

  bool IssueCopy() override;

 private:
  bool BeginSubmission(bool is_guest_command = true);
  bool EndSubmission(bool is_swap = false);
  void AwaitAllSubmissionsCompletion();

  void UpdateSystemConstants();

  void UpdateGammaRampValue(GammaRampType type, uint32_t index,
                            uint32_t value);

  MetalGraphicsSystem* metal_graphics_system_ = nullptr;

  // Submission tracking.
  uint64_t submission_current_ = 1;
  std::atomic<uint64_t> submission_completed_{0};
  bool submission_open_ = false;
  bool frame_open_ = false;
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;

#ifdef __OBJC__
  id<MTLCommandBuffer> current_command_buffer_ = nil;
  id<MTLCommandBuffer> last_committed_command_buffer_ = nil;
#else
  void* current_command_buffer_ = nullptr;
  void* last_committed_command_buffer_ = nullptr;
#endif

  // System constants (uniform buffer for translated shaders).
  struct SystemConstants {
    float ndc_scale[3];
    float ndc_offset[3];
    float point_size_x;
    float point_size_y;
    float point_size_min_max[2];
    uint32_t vertex_index_endian;
    uint32_t vertex_base_index;
    uint32_t pixel_half_pixel_offset;
    float alpha_test_reference;
    uint32_t edram_pitch_tiles;
    uint32_t edram_depth_base_dwords;
    uint32_t color_output_map[4];
  };
  SystemConstants system_constants_;
  bool system_constants_dirty_ = true;

  bool cache_clear_requested_ = false;

  // Index buffer ring buffer — avoids per-draw Metal buffer allocations.
  static constexpr size_t kIndexRingBufferSize = 4 * 1024 * 1024;  // 4 MB
#ifdef __OBJC__
  id<MTLBuffer> index_ring_buffer_ = nil;
#else
  void* index_ring_buffer_ = nullptr;
#endif
  size_t index_ring_buffer_offset_ = 0;

  // Fence tracking for the index ring buffer to prevent overwriting data
  // still being consumed by the GPU. Each entry records the submission ID
  // that was current when data was written to [offset, offset+size).
  struct IndexRingFence {
    size_t offset;
    size_t size;
    uint64_t submission_id;
  };
  std::vector<IndexRingFence> index_ring_fences_;

  // Double-buffered staging textures for IssueCopy async readback.
  // Index alternates each resolve so the CPU reads from the previous
  // frame's staging texture while the current blit is in-flight.
  static constexpr uint32_t kCopyStagingCount = 2;
  uint32_t copy_staging_index_ = 0;
#ifdef __OBJC__
  id<MTLTexture> copy_staging_textures_[kCopyStagingCount] = {nil, nil};
#else
  void* copy_staging_textures_[kCopyStagingCount] = {nullptr, nullptr};
#endif
  uint32_t copy_staging_width_ = 0;
  uint32_t copy_staging_height_ = 0;
  // Submission ID of the last blit into copy_staging_textures_[i].
  uint64_t copy_staging_submission_[kCopyStagingCount] = {0, 0};

  // Cached render encoder — reused across draws when render targets don't
  // change.  Creating a new MTLRenderCommandEncoder per draw triggers a
  // load/store cycle on all attachments, which is extremely expensive.
  void EndCurrentRenderEncoder();
  bool EnsureRenderEncoder(
      const MetalRenderTargetCache::RenderTarget color_targets[4],
      uint32_t color_count,
      const MetalRenderTargetCache::RenderTarget& depth_target,
      bool depth_enabled,
      uint32_t& rt_width_out, uint32_t& rt_height_out);

#ifdef __OBJC__
  id<MTLRenderCommandEncoder> current_render_encoder_ = nil;
#else
  void* current_render_encoder_ = nullptr;
#endif
  // Track which render targets are currently bound so we can detect changes.
  uint32_t current_rt_color_count_ = 0;
  uint32_t current_rt_depth_base_ = UINT32_MAX;
  uint32_t current_rt_color_bases_[4] = {};
  uint32_t current_rt_width_ = 0;
  uint32_t current_rt_height_ = 0;
  MetalRenderTargetCache::RenderTarget current_rt_colors_[4] = {};
  MetalRenderTargetCache::RenderTarget current_rt_depth_ = {};
  bool current_rt_depth_enabled_ = false;

  // GPU sub-systems.
  std::unique_ptr<MetalSharedMemory> shared_memory_;
  std::unique_ptr<MetalPipelineCache> pipeline_cache_;
  std::unique_ptr<MetalRenderTargetCache> render_target_cache_;
  std::unique_ptr<MetalTextureCache> texture_cache_;
  std::unique_ptr<MetalPrimitiveProcessor> primitive_processor_;

  // Shader cache.
  std::unordered_map<uint64_t, MetalShader*> shader_map_;
};

}  // namespace rex::graphics::metal
