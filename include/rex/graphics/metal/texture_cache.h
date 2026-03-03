/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 5: Full texture binding + Morton untiling + samplers
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/xenos.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace rex::graphics::metal {

class MetalCommandProcessor;
class MetalSharedMemory;

class MetalTextureCache : public TextureCache {
 public:
  MetalTextureCache(MetalCommandProcessor& command_processor,
                    const RegisterFile& register_file,
                    MetalSharedMemory& shared_memory);
  ~MetalTextureCache() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void ClearCache() override;

  // Pure virtual overrides from TextureCache
  uint32_t GetHostFormatSwizzle(TextureKey key) const override { return 0; }
  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override { return 16384; }
  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override { return 2048; }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return nullptr;
  }
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture,
                                             bool load_base,
                                             bool load_mips) override {
    return false;
  }
  void CompletedSubmissionUpdated();
  void BeginSubmission();
  void BeginFrame();

  void TextureFetchConstantWritten(uint32_t index);

  // =========================================================================
  // Swap texture (front buffer presentation)
  // =========================================================================
#ifdef __OBJC__
  id<MTLTexture> RequestSwapTexture(uint32_t frontbuffer_ptr,
                                    uint32_t frontbuffer_width,
                                    uint32_t frontbuffer_height,
                                    uint32_t& width_out,
                                    uint32_t& height_out,
                                    xenos::TextureFormat& format_out);
#endif

  uint32_t draw_resolution_scale_x() const { return 1; }
  uint32_t draw_resolution_scale_y() const { return 1; }

  // =========================================================================
  // Texture binding for draw calls
  // =========================================================================

  // Cached texture entry for reuse across draws.
  struct CachedTexture {
#ifdef __OBJC__
    id<MTLTexture> texture = nil;
    id<MTLSamplerState> sampler = nil;
#else
    void* texture = nullptr;
    void* sampler = nullptr;
#endif
    uint32_t guest_address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
    bool tiled = false;
    uint64_t last_used_submission = 0;
  };

  // Request a texture+sampler for the given fetch constant index (0-31).
  // Returns the cached texture entry or nullptr if the fetch constant
  // is invalid or the texture could not be created.
  const CachedTexture* RequestTexture(uint32_t fetch_index);

  // =========================================================================
  // Texture untiling (Morton/Z-order)
  // =========================================================================

  // Untile a block of Xenos texture data from guest memory into a
  // linear buffer suitable for uploading to MTLTexture.
  // Handles both micro-tiled and macro-tiled layouts.
  static void UntileTexture(const uint8_t* src, uint8_t* dst,
                            uint32_t width, uint32_t height,
                            uint32_t bytes_per_pixel,
                            uint32_t src_pitch, bool tiled,
                            xenos::Endian endian);

  // =========================================================================
  // Format conversion
  // =========================================================================

  // Xenos texture format → Metal pixel format.
  // Returns MTLPixelFormatInvalid if the format is not supported.
  static MTLPixelFormat XenosFormatToMetal(xenos::TextureFormat format);

  // Bytes per pixel/block for a given Xenos format.
  static uint32_t GetBytesPerPixel(xenos::TextureFormat format);

 private:
  MetalCommandProcessor& command_processor_;
  MetalSharedMemory& shared_memory_;

  bool fetch_constants_dirty_[32] = {};

  // Swap texture.
#ifdef __OBJC__
  id<MTLTexture> swap_texture_ = nil;
#else
  void* swap_texture_ = nullptr;
#endif
  uint32_t swap_texture_width_ = 0;
  uint32_t swap_texture_height_ = 0;

  // =========================================================================
  // Texture cache
  // =========================================================================
  struct TextureKey {
    uint32_t guest_address;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    bool tiled;

    bool operator==(const TextureKey& o) const {
      return guest_address == o.guest_address && width == o.width &&
             height == o.height && format == o.format && tiled == o.tiled;
    }
  };
  struct TextureKeyHash {
    size_t operator()(const TextureKey& k) const {
      size_t h = 14695981039346656037ULL;
      auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
      mix(k.guest_address);
      mix(k.width);
      mix(k.height);
      mix(k.format);
      mix(k.tiled ? 1u : 0u);
      return h;
    }
  };

  std::unordered_map<TextureKey, CachedTexture, TextureKeyHash> texture_map_;

  // Sampler cache keyed by filter + address mode.
  struct SamplerKey {
    uint32_t min_filter;  // MTLSamplerMinMagFilter
    uint32_t mag_filter;
    uint32_t mip_filter;
    uint32_t address_u;   // MTLSamplerAddressMode
    uint32_t address_v;
    uint32_t address_w;
    uint32_t aniso_filter;  // Max anisotropy

    bool operator==(const SamplerKey& o) const {
      return min_filter == o.min_filter && mag_filter == o.mag_filter &&
             mip_filter == o.mip_filter && address_u == o.address_u &&
             address_v == o.address_v && address_w == o.address_w &&
             aniso_filter == o.aniso_filter;
    }
  };
  struct SamplerKeyHash {
    size_t operator()(const SamplerKey& k) const {
      size_t h = k.min_filter | (k.mag_filter << 4) | (k.mip_filter << 8);
      h ^= k.address_u | (k.address_v << 4) | (k.address_w << 8);
      h ^= (k.aniso_filter << 16);
      return h;
    }
  };

#ifdef __OBJC__
  std::unordered_map<SamplerKey, id<MTLSamplerState>, SamplerKeyHash>
      sampler_map_;
  id<MTLSamplerState> GetOrCreateSampler(const SamplerKey& key);
#endif

  // Temporary buffer for untiled data.
  std::vector<uint8_t> untile_buffer_;
};

}  // namespace rex::graphics::metal
