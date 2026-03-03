/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: Full render target creation + resolve pipeline
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>

#ifdef __OBJC__
@protocol MTLTexture;
@protocol MTLBuffer;
@protocol MTLRenderCommandEncoder;
@protocol MTLComputeCommandEncoder;
#endif

namespace rex::graphics::metal {

class MetalCommandProcessor;

class MetalRenderTargetCache : public RenderTargetCache {
 public:
  MetalRenderTargetCache(MetalCommandProcessor& command_processor,
                         const RegisterFile& register_file,
                         const memory::Memory& memory);
  ~MetalRenderTargetCache() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // Pure virtual overrides from RenderTargetCache
  Path GetPath() const override { return Path::kHostRenderTargets; }
  RenderTargetCache::RenderTarget* CreateRenderTarget(
      RenderTargetCache::RenderTargetKey key) override { return nullptr; }
  bool IsHostDepthEncodingDifferent(
      xenos::DepthRenderTargetFormat format) const override { return false; }

  void CompletedSubmissionUpdated();
  void BeginSubmission();
  void BeginFrame();

  void ClearCache() override;

  void RestoreEdramSnapshot(const void* snapshot);

#ifdef __OBJC__
  id<MTLBuffer> edram_buffer() const { return edram_buffer_; }
#else
  void* edram_buffer() const { return edram_buffer_; }
#endif

  // =========================================================================
  // Render target descriptor
  // =========================================================================
  struct RenderTarget {
    uint32_t edram_base;
    uint32_t pitch_tiles;
    xenos::MsaaSamples msaa;
    xenos::ColorRenderTargetFormat color_format;
    xenos::DepthRenderTargetFormat depth_format;
    bool is_depth;
    uint32_t width_pixels;
    uint32_t height_pixels;
#ifdef __OBJC__
    id<MTLTexture> texture = nil;
#else
    void* texture = nullptr;
#endif
  };

  // Get current render targets from register state.
  uint32_t GetCurrentRenderTargets(RenderTarget color_targets[4],
                                   RenderTarget& depth_target,
                                   bool& depth_enabled);

  // =========================================================================
  // Texture creation + management
  // =========================================================================

  // Get or create a Metal texture for the given render target configuration.
  // Returns nil if creation fails.
#ifdef __OBJC__
  id<MTLTexture> GetOrCreateRenderTargetTexture(
      uint32_t edram_base, uint32_t pitch_tiles, uint32_t height_pixels,
      bool is_depth, uint32_t format, xenos::MsaaSamples msaa);
#endif

  // Query/mark whether a render target has been rendered to at least once.
  // Used to select MTLLoadActionDontCare vs MTLLoadActionLoad.
  bool HasBeenRendered(uint32_t edram_base, uint32_t pitch_tiles,
                       uint32_t height_pixels, bool is_depth,
                       uint32_t format, xenos::MsaaSamples msaa) const;
  void MarkRendered(uint32_t edram_base, uint32_t pitch_tiles,
                    uint32_t height_pixels, bool is_depth,
                    uint32_t format, xenos::MsaaSamples msaa);

  // Xenos color format → Metal pixel format conversion.
  static uint32_t ColorFormatToMetal(
      xenos::ColorRenderTargetFormat format);

  // Xenos depth format → Metal pixel format conversion.
  static uint32_t DepthFormatToMetal(
      xenos::DepthRenderTargetFormat format);

  // Bytes per pixel for Xenos RT formats used by the simplified EDRAM store /
  // resolve paths.
  static uint32_t ColorFormatBytesPerPixel(
      xenos::ColorRenderTargetFormat format);
  static uint32_t DepthFormatBytesPerPixel(
      xenos::DepthRenderTargetFormat format);

  // =========================================================================
  // EDRAM resolve
  // =========================================================================

  // Resolve a render target from its MTLTexture back into the EDRAM buffer.
  // Used before IssueCopy reads from EDRAM.
#ifdef __OBJC__
  void StoreRenderTargetToEdram(id<MTLRenderCommandEncoder> encoder,
                                 const RenderTarget& rt);
#endif

  // Resolve from EDRAM to a destination texture (for IssueCopy).
  // Uses the compute kernel from pipeline_cache.
#ifdef __OBJC__
  bool ResolveEdramToTexture(id<MTLComputeCommandEncoder> compute_encoder,
                              uint32_t edram_base, uint32_t edram_pitch,
                              uint32_t src_x, uint32_t src_y,
                              uint32_t width, uint32_t height,
                              uint32_t bytes_per_pixel,
                              id<MTLTexture> dest_texture);
#endif

  // =========================================================================
  // Apple Silicon tile shading
  // =========================================================================

  // Whether tile shading is active for this session.
  bool IsTileShadingEnabled() const { return use_tile_shading_; }

  // Create a memoryless render target for use with tile shading.
  // These live only in on-chip tile memory and never touch DRAM.
  // format is a MTLPixelFormat cast to uint32_t (avoids importing Metal.h
  // in the header — MTLPixelFormat is an enum, not a forward-declarable type).
#ifdef __OBJC__
  id<MTLTexture> GetOrCreateMemorylessRenderTarget(
      uint32_t width, uint32_t height, uint32_t format,
      uint32_t sample_count);
#endif

 protected:
  bool IsGammaFormatHostStorageSeparate() const override { return false; }
  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

 private:
  MetalCommandProcessor& command_processor_;

  static constexpr size_t kEdramSize = 10 * 1024 * 1024;
#ifdef __OBJC__
  id<MTLBuffer> edram_buffer_ = nil;
  id<MTLComputePipelineState> resolve_pso_ = nil;
  id<MTLComputePipelineState> store_pso_ = nil;
#else
  void* edram_buffer_ = nullptr;
  void* resolve_pso_ = nullptr;
  void* store_pso_ = nullptr;
#endif

  // Whether Apple Silicon tile shading is in use.
  bool use_tile_shading_ = false;

  // =========================================================================
  // Memoryless render target cache (tile shading path)
  // =========================================================================
  struct MemorylessKey {
    uint32_t width;
    uint32_t height;
    uint32_t format;     // MTLPixelFormat
    uint32_t samples;

    bool operator==(const MemorylessKey& o) const {
      return width == o.width && height == o.height &&
             format == o.format && samples == o.samples;
    }
  };
  struct MemorylessKeyHash {
    size_t operator()(const MemorylessKey& k) const {
      // Use XXH3 for consistency with RenderTargetKeyHash.
      struct Packed {
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t samples;
      };
      Packed p;
      p.width = k.width;
      p.height = k.height;
      p.format = k.format;
      p.samples = k.samples;
      return static_cast<size_t>(XXH3_64bits(&p, sizeof(p)));
    }
  };

#ifdef __OBJC__
  std::unordered_map<MemorylessKey, id<MTLTexture>, MemorylessKeyHash>
      memoryless_cache_;
#endif

  // =========================================================================
  // Cached render target textures
  // =========================================================================
  struct RenderTargetKey {
    uint32_t edram_base;
    uint32_t pitch_tiles;
    uint32_t height_tiles;
    uint32_t format;
    bool is_depth;
    uint32_t msaa;

    bool operator==(const RenderTargetKey& other) const {
      return edram_base == other.edram_base &&
             pitch_tiles == other.pitch_tiles &&
             height_tiles == other.height_tiles &&
             format == other.format && is_depth == other.is_depth &&
             msaa == other.msaa;
    }
  };
  struct RenderTargetKeyHash {
    size_t operator()(const RenderTargetKey& key) const {
      // Pack into contiguous bytes for a single XXH3 call.
      // This avoids weak mixing from is_depth being only 0/1.
      struct Packed {
        uint32_t edram_base;
        uint32_t pitch_tiles;
        uint32_t height_tiles;
        uint32_t format;
        uint32_t is_depth;
        uint32_t msaa;
      };
      Packed p;
      p.edram_base = key.edram_base;
      p.pitch_tiles = key.pitch_tiles;
      p.height_tiles = key.height_tiles;
      p.format = key.format;
      p.is_depth = key.is_depth ? 1u : 0u;
      p.msaa = key.msaa;
      return static_cast<size_t>(XXH3_64bits(&p, sizeof(p)));
    }
  };

  struct CachedRenderTarget {
#ifdef __OBJC__
    id<MTLTexture> texture = nil;
#else
    void* texture = nullptr;
#endif
    uint32_t width;
    uint32_t height;
    uint64_t last_used_submission;
    bool has_been_rendered = false;
  };

  std::unordered_map<RenderTargetKey, CachedRenderTarget, RenderTargetKeyHash>
      render_target_cache_;
};

}  // namespace rex::graphics::metal
