/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 5: Full texture binding + Morton untiling + samplers
 */

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/graphics/metal/texture_cache.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/metal/shared_memory.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>
#include <rex/ui/metal/metal_provider.h>
#include <rex/logging.h>
#include <rex/xenia_logging_compat.h>

namespace rex::graphics::metal {

// ============================================================================
// Morton / Z-order utilities for Xenos texture untiling
// ============================================================================

namespace {

// Decode a Morton-encoded coordinate.
// Separates interleaved bits: xy -> x, y.
inline uint32_t MortonDecode2X(uint32_t code) {
  code &= 0x55555555;
  code = (code | (code >> 1)) & 0x33333333;
  code = (code | (code >> 2)) & 0x0F0F0F0F;
  code = (code | (code >> 4)) & 0x00FF00FF;
  code = (code | (code >> 8)) & 0x0000FFFF;
  return code;
}

inline uint32_t MortonDecode2Y(uint32_t code) {
  return MortonDecode2X(code >> 1);
}

// Xenos micro-tiling parameters.
// For 32bpp: micro tile = 32x32 pixels, with Morton order within.
// For 64bpp: micro tile = 32x16 pixels.
// For DXT compressed: micro tile covers 128x128 texels (32x32 blocks).
struct TileParams {
  uint32_t micro_tile_width;   // Pixels per micro tile (width)
  uint32_t micro_tile_height;  // Pixels per micro tile (height)
};

TileParams GetTileParams(uint32_t bytes_per_pixel) {
  TileParams p;
  switch (bytes_per_pixel) {
    case 1:
      p.micro_tile_width = 32;
      p.micro_tile_height = 32;
      break;
    case 2:
      p.micro_tile_width = 32;
      p.micro_tile_height = 32;
      break;
    case 4:
      p.micro_tile_width = 32;
      p.micro_tile_height = 32;
      break;
    case 8:
      p.micro_tile_width = 32;
      p.micro_tile_height = 16;
      break;
    case 16:
      p.micro_tile_width = 16;
      p.micro_tile_height = 16;
      break;
    default:
      p.micro_tile_width = 32;
      p.micro_tile_height = 32;
      break;
  }
  return p;
}

}  // namespace

// ============================================================================
// Texture untiling
// ============================================================================

void MetalTextureCache::UntileTexture(const uint8_t* src, uint8_t* dst,
                                       uint32_t width, uint32_t height,
                                       uint32_t bytes_per_pixel,
                                       uint32_t src_pitch, bool tiled,
                                       xenos::Endian endian) {
  uint32_t dst_pitch = width * bytes_per_pixel;

  if (!tiled) {
    // Linear texture: just copy rows with endian swap.
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* src_row = src + y * src_pitch;
      uint8_t* dst_row = dst + y * dst_pitch;

      if (endian == xenos::Endian::kNone) {
        std::memcpy(dst_row, src_row, dst_pitch);
      } else if (bytes_per_pixel == 4) {
        const uint32_t* s = reinterpret_cast<const uint32_t*>(src_row);
        uint32_t* d = reinterpret_cast<uint32_t*>(dst_row);
        for (uint32_t x = 0; x < width; ++x) {
          uint32_t pixel = s[x];
          if (endian == xenos::Endian::k8in32) {
            pixel = __builtin_bswap32(pixel);
          } else if (endian == xenos::Endian::k8in16) {
            pixel = ((pixel & 0xFF00FF00) >> 8) | ((pixel & 0x00FF00FF) << 8);
          }
          d[x] = pixel;
        }
      } else if (bytes_per_pixel == 2) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(src_row);
        uint16_t* d = reinterpret_cast<uint16_t*>(dst_row);
        for (uint32_t x = 0; x < width; ++x) {
          uint16_t pixel = s[x];
          if (endian == xenos::Endian::k8in16 ||
              endian == xenos::Endian::k8in32) {
            pixel = __builtin_bswap16(pixel);
          }
          d[x] = pixel;
        }
      } else {
        // Byte-level copy for 1bpp, 8bpp, etc.
        std::memcpy(dst_row, src_row, dst_pitch);
      }
    }
    return;
  }

  // ===== Tiled (micro + macro tiling via GetTiledOffset2D) =====
  // Use the proper Xenos tiling function which handles both micro-tile
  // Morton ordering AND macro-tile layout. The naive row-major micro-tile
  // index calculation was incorrect for textures wider than ~256px where
  // macro tiling rearranges micro tiles in a non-linear pattern.
  uint32_t bpp_log2 = rex::log2_floor(bytes_per_pixel);
  // Pitch in blocks, tile-aligned (32-block alignment for Xenos).
  uint32_t pitch_blocks = rex::align(width, xenos::kTextureTileWidthHeight);

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int32_t src_offset = texture_util::GetTiledOffset2D(
          int32_t(x), int32_t(y), pitch_blocks, bpp_log2);

      const uint8_t* src_pixel = src + src_offset;
      uint8_t* dst_pixel =
          dst + y * dst_pitch + x * bytes_per_pixel;

      // Copy + endian swap.
      if (bytes_per_pixel == 4) {
        uint32_t pixel;
        std::memcpy(&pixel, src_pixel, 4);
        if (endian == xenos::Endian::k8in32) {
          pixel = __builtin_bswap32(pixel);
        } else if (endian == xenos::Endian::k8in16) {
          pixel = ((pixel & 0xFF00FF00) >> 8) | ((pixel & 0x00FF00FF) << 8);
        }
        std::memcpy(dst_pixel, &pixel, 4);
      } else if (bytes_per_pixel == 2) {
        uint16_t pixel;
        std::memcpy(&pixel, src_pixel, 2);
        if (endian == xenos::Endian::k8in16 ||
            endian == xenos::Endian::k8in32) {
          pixel = __builtin_bswap16(pixel);
        }
        std::memcpy(dst_pixel, &pixel, 2);
      } else {
        std::memcpy(dst_pixel, src_pixel, bytes_per_pixel);
      }
    }
  }
}

// ============================================================================
// Format conversion
// ============================================================================

MTLPixelFormat MetalTextureCache::XenosFormatToMetal(
    xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8:
      return MTLPixelFormatR8Unorm;
    case xenos::TextureFormat::k_8_8:
      return MTLPixelFormatRG8Unorm;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      return MTLPixelFormatRGBA8Unorm;
    case xenos::TextureFormat::k_5_6_5:
      // No direct Metal equivalent; use RGBA8 and convert on CPU.
      return MTLPixelFormatRGBA8Unorm;
    case xenos::TextureFormat::k_1_5_5_5:
      return MTLPixelFormatBGR5A1Unorm;
    case xenos::TextureFormat::k_4_4_4_4:
      return MTLPixelFormatABGR4Unorm;
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      return MTLPixelFormatRGB10A2Unorm;
    case xenos::TextureFormat::k_16:
      return MTLPixelFormatR16Unorm;
    case xenos::TextureFormat::k_16_16:
      return MTLPixelFormatRG16Unorm;
    case xenos::TextureFormat::k_16_16_16_16:
      return MTLPixelFormatRGBA16Unorm;
    case xenos::TextureFormat::k_16_FLOAT:
      return MTLPixelFormatR16Float;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return MTLPixelFormatRG16Float;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return MTLPixelFormatRGBA16Float;
    case xenos::TextureFormat::k_32_FLOAT:
      return MTLPixelFormatR32Float;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return MTLPixelFormatRG32Float;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return MTLPixelFormatRGBA32Float;
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return MTLPixelFormatBC1_RGBA;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return MTLPixelFormatBC2_RGBA;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return MTLPixelFormatBC3_RGBA;
    case xenos::TextureFormat::k_DXN:
      return MTLPixelFormatBC5_RGUnorm;
    case xenos::TextureFormat::k_CTX1:
      // No BC equivalent; use RG8.
      return MTLPixelFormatRG8Unorm;
    default:
      return MTLPixelFormatRGBA8Unorm;
  }
}

uint32_t MetalTextureCache::GetBytesPerPixel(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8: return 1;
    case xenos::TextureFormat::k_8_8: return 2;
    case xenos::TextureFormat::k_5_6_5: return 2;
    case xenos::TextureFormat::k_1_5_5_5: return 2;
    case xenos::TextureFormat::k_4_4_4_4: return 2;
    case xenos::TextureFormat::k_16: return 2;
    case xenos::TextureFormat::k_16_FLOAT: return 2;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16: return 4;
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16: return 4;
    case xenos::TextureFormat::k_16_16: return 4;
    case xenos::TextureFormat::k_16_16_FLOAT: return 4;
    case xenos::TextureFormat::k_32_FLOAT: return 4;
    case xenos::TextureFormat::k_16_16_16_16: return 8;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: return 8;
    case xenos::TextureFormat::k_32_32_FLOAT: return 8;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: return 16;
    // Block-compressed formats: return bytes per 4x4 block.
    // These are NOT bytes-per-pixel — callers handling BC formats must
    // account for the 4x4 block size when computing buffer sizes.
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
    case xenos::TextureFormat::k_CTX1:
      return 8;  // 8 bytes per 4x4 block
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXN:
      return 16;  // 16 bytes per 4x4 block
    default: return 4;
  }
}

// ============================================================================
// Sampler creation
// ============================================================================

static MTLSamplerMinMagFilter XenosFilterToMetal(
    xenos::TextureFilter filter) {
  switch (filter) {
    case xenos::TextureFilter::kPoint:
      return MTLSamplerMinMagFilterNearest;
    case xenos::TextureFilter::kLinear:
      return MTLSamplerMinMagFilterLinear;
    default:
      return MTLSamplerMinMagFilterLinear;
  }
}

static MTLSamplerAddressMode XenosClampToMetal(
    xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return MTLSamplerAddressModeRepeat;
    case xenos::ClampMode::kMirroredRepeat:
      return MTLSamplerAddressModeMirrorRepeat;
    case xenos::ClampMode::kClampToEdge:
      return MTLSamplerAddressModeClampToEdge;
    case xenos::ClampMode::kClampToHalfway:
      return MTLSamplerAddressModeClampToEdge;
    case xenos::ClampMode::kClampToBorder:
      return MTLSamplerAddressModeClampToBorderColor;
    case xenos::ClampMode::kMirrorClampToEdge:
      return MTLSamplerAddressModeMirrorClampToEdge;
    case xenos::ClampMode::kMirrorClampToHalfway:
      return MTLSamplerAddressModeMirrorClampToEdge;
    case xenos::ClampMode::kMirrorClampToBorder:
      return MTLSamplerAddressModeClampToBorderColor;
    default:
      return MTLSamplerAddressModeClampToEdge;
  }
}

id<MTLSamplerState> MetalTextureCache::GetOrCreateSampler(
    const SamplerKey& key) {
  auto it = sampler_map_.find(key);
  if (it != sampler_map_.end()) return it->second;

  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) return nil;

  MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];
  desc.minFilter = (MTLSamplerMinMagFilter)key.min_filter;
  desc.magFilter = (MTLSamplerMinMagFilter)key.mag_filter;
  desc.mipFilter = (MTLSamplerMipFilter)key.mip_filter;
  desc.sAddressMode = (MTLSamplerAddressMode)key.address_u;
  desc.tAddressMode = (MTLSamplerAddressMode)key.address_v;
  desc.rAddressMode = (MTLSamplerAddressMode)key.address_w;
  desc.maxAnisotropy = std::max(1u, key.aniso_filter);

  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:desc];
  if (sampler) {
    sampler_map_[key] = sampler;
  }
  return sampler;
}

// ============================================================================
// MetalTextureCache
// ============================================================================

MetalTextureCache::MetalTextureCache(
    MetalCommandProcessor& command_processor,
    const RegisterFile& register_file,
    MetalSharedMemory& shared_memory)
    : TextureCache(register_file, shared_memory, 1, 1),
      command_processor_(command_processor),
      shared_memory_(shared_memory) {
  std::memset(fetch_constants_dirty_, 0, sizeof(fetch_constants_dirty_));
}

MetalTextureCache::~MetalTextureCache() {
  Shutdown(true);
}

bool MetalTextureCache::Initialize() {
  XELOGI("MetalTextureCache: Initialized with untiling + sampler cache");
  return true;
}

void MetalTextureCache::Shutdown(bool from_destructor) {
  ClearCache();
}

void MetalTextureCache::ClearCache() {
  swap_texture_ = nil;
  swap_texture_width_ = 0;
  swap_texture_height_ = 0;
  texture_map_.clear();
  sampler_map_.clear();
  untile_buffer_.clear();
}

void MetalTextureCache::CompletedSubmissionUpdated() {
  // Evict textures unused for >120 submissions.
  uint64_t current = command_processor_.GetCurrentSubmission();
  auto it = texture_map_.begin();
  while (it != texture_map_.end()) {
    if (current - it->second.last_used_submission > 120) {
      it->second.texture = nil;
      it->second.sampler = nil;
      it = texture_map_.erase(it);
    } else {
      ++it;
    }
  }
}

void MetalTextureCache::BeginSubmission() {}

void MetalTextureCache::BeginFrame() {
  // Do NOT mark all fetch constants dirty here — that forces every texture
  // to be re-untiled and re-uploaded every frame, which is very expensive.
  // Fetch constants are already marked dirty individually via
  // TextureFetchConstantWritten() when the game writes new values.
}

void MetalTextureCache::TextureFetchConstantWritten(uint32_t index) {
  if (index < 32) {
    fetch_constants_dirty_[index] = true;
  }
}

// ============================================================================
// Texture binding for draw calls
// ============================================================================

const MetalTextureCache::CachedTexture* MetalTextureCache::RequestTexture(
    uint32_t fetch_index) {
  if (fetch_index >= 32) return nullptr;

  const auto& regs = register_file();
  auto fetch = regs.GetTextureFetch(fetch_index);

  if (fetch.type != xenos::FetchConstantType::kTexture) return nullptr;

  uint32_t guest_address = fetch.base_address << 12;
  if (guest_address == 0) return nullptr;

  auto dimension = fetch.dimension;
  if (dimension != xenos::DataDimension::k2DOrStacked) {
    // Only 2D textures for now.
    return nullptr;
  }

  uint32_t width = fetch.size_2d.width + 1;
  uint32_t height = fetch.size_2d.height + 1;
  xenos::TextureFormat format =
      static_cast<xenos::TextureFormat>(fetch.format);
  bool tiled = fetch.tiled;
  xenos::Endian endian = fetch.endianness;

  if (width == 0 || height == 0) return nullptr;

  MTLPixelFormat metal_format = XenosFormatToMetal(format);
  uint32_t bpp = GetBytesPerPixel(format);

  // Build cache key.
  TextureKey key;
  key.guest_address = guest_address;
  key.width = width;
  key.height = height;
  key.format = (uint32_t)format;
  key.tiled = tiled;

  // Check if we already have this texture and it's not dirty.
  auto it = texture_map_.find(key);
  if (it != texture_map_.end() && !fetch_constants_dirty_[fetch_index]) {
    it->second.last_used_submission =
        command_processor_.GetCurrentSubmission();
    return &it->second;
  }

  // Create or update the texture.
  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) return nullptr;

  // Get source data from shared memory.
  const uint8_t* shared_mem =
      static_cast<const uint8_t*>(shared_memory_.buffer_contents());
  if (!shared_mem) return nullptr;

  // Calculate data size accounting for pitch alignment. Tiled textures have
  // their pitch aligned to tile boundaries (32 blocks), and even linear
  // textures may have row alignment. Use the aligned pitch for the bounds
  // check to avoid rejecting valid textures whose pitch-aligned footprint
  // exceeds width * height * bpp.
  uint32_t aligned_pitch;
  if (tiled) {
    uint32_t pitch_blocks = rex::align(width, xenos::kTextureTileWidthHeight);
    aligned_pitch = pitch_blocks * bpp;
  } else {
    aligned_pitch = rex::align(width * bpp,
                               xenos::kTextureLinearRowAlignmentBytes);
  }
  uint32_t data_size = aligned_pitch * height;
  if (guest_address + data_size > MetalSharedMemory::kBufferSize) {
    return nullptr;
  }

  const uint8_t* src = shared_mem + guest_address;

  // Untile the texture data.
  uint32_t dst_pitch = width * bpp;
  // For tiled textures, src_pitch is unused — GetTiledOffset2D computes
  // source offsets from (x, y, pitch_blocks, bpp_log2) independently.
  // For linear textures, src_pitch is the byte stride per row.
  uint32_t src_pitch = tiled ? 0 : width * bpp;

  untile_buffer_.resize(dst_pitch * height);
  UntileTexture(src, untile_buffer_.data(), width, height, bpp,
                src_pitch, tiled, endian);

  // Create MTLTexture if needed.
  CachedTexture& cached = texture_map_[key];
  bool use_private = !provider.caps().PreferSharedStorage();
  if (!cached.texture || cached.width != width || cached.height != height) {
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:metal_format
                                    width:width
                                   height:height
                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    if (use_private) {
      desc.storageMode = MTLStorageModePrivate;
    } else {
      desc.storageMode = MTLStorageModeShared;
    }

    cached.texture = [device newTextureWithDescriptor:desc];
    if (!cached.texture) {
      XELOGE("MetalTextureCache: Failed to create {}x{} texture", width,
             height);
      return nullptr;
    }
    cached.width = width;
    cached.height = height;
    cached.format = format;
    cached.guest_address = guest_address;
    cached.tiled = tiled;
  }

  // Upload untiled data.
  // For formats that need CPU conversion (e.g., 5_6_5 → RGBA8), do it here.
  const uint8_t* upload_data = untile_buffer_.data();
  uint32_t upload_pitch = dst_pitch;
  std::vector<uint32_t> rgba;

  if (format == xenos::TextureFormat::k_5_6_5) {
    // Convert 565 → RGBA8.
    rgba.resize(width * height);
    const uint16_t* src16 =
        reinterpret_cast<const uint16_t*>(untile_buffer_.data());
    for (uint32_t i = 0; i < width * height; ++i) {
      uint16_t p = src16[i];
      uint32_t r = ((p >> 11) & 0x1F) * 255 / 31;
      uint32_t g = ((p >> 5) & 0x3F) * 255 / 63;
      uint32_t b = (p & 0x1F) * 255 / 31;
      rgba[i] = r | (g << 8) | (b << 16) | 0xFF000000;
    }
    upload_data = reinterpret_cast<const uint8_t*>(rgba.data());
    upload_pitch = width * 4;
  }

  if (use_private) {
    // For private storage, upload via a temporary shared buffer + blit
    // encoded on the command processor's current command buffer.  This
    // avoids creating a separate command buffer with waitUntilCompleted
    // per texture, which would stall the GPU pipeline inside the draw loop.
    uint32_t upload_size = upload_pitch * height;
    id<MTLBuffer> staging = [device newBufferWithBytes:upload_data
                                               length:upload_size
                                              options:MTLResourceStorageModeShared];
    if (staging) {
      id<MTLCommandBuffer> cmd = command_processor_.GetCurrentCommandBuffer();
      if (cmd) {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit copyFromBuffer:staging
                sourceOffset:0
           sourceBytesPerRow:upload_pitch
         sourceBytesPerImage:upload_size
                  sourceSize:MTLSizeMake(width, height, 1)
                   toTexture:cached.texture
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
      }
    }
  } else {
    [cached.texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                      mipmapLevel:0
                        withBytes:upload_data
                      bytesPerRow:upload_pitch];
  }

  // Create sampler from fetch constant state.
  SamplerKey skey;
  skey.min_filter = (uint32_t)XenosFilterToMetal(fetch.min_filter);
  skey.mag_filter = (uint32_t)XenosFilterToMetal(fetch.mag_filter);
  skey.mip_filter = (uint32_t)MTLSamplerMipFilterNearest;
  if (fetch.mip_filter == xenos::TextureFilter::kLinear) {
    skey.mip_filter = (uint32_t)MTLSamplerMipFilterLinear;
  }
  skey.address_u = (uint32_t)XenosClampToMetal(fetch.clamp_x);
  skey.address_v = (uint32_t)XenosClampToMetal(fetch.clamp_y);
  skey.address_w = (uint32_t)XenosClampToMetal(fetch.clamp_z);
  skey.aniso_filter = std::max(1u, uint32_t(fetch.aniso_filter));

  cached.sampler = GetOrCreateSampler(skey);
  cached.last_used_submission = command_processor_.GetCurrentSubmission();

  fetch_constants_dirty_[fetch_index] = false;

  return &cached;
}

// ============================================================================
// Swap texture (presentation)
// ============================================================================

id<MTLTexture> MetalTextureCache::RequestSwapTexture(
    uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
    uint32_t frontbuffer_height, uint32_t& width_out, uint32_t& height_out,
    xenos::TextureFormat& format_out) {
  uint32_t guest_address = frontbuffer_ptr;
  uint32_t width = frontbuffer_width;
  uint32_t height = frontbuffer_height;
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  bool is_tiled = false;
  xenos::Endian endian = xenos::Endian::kNone;

  const auto& regs = register_file();
  auto fetch = regs.GetTextureFetch(0);
  bool fetch_valid =
      fetch.type == xenos::FetchConstantType::kTexture &&
      fetch.dimension == xenos::DataDimension::k2DOrStacked;
  if (fetch_valid) {
    uint32_t fetch_address = fetch.base_address << 12;
    uint32_t fetch_width = fetch.size_2d.width + 1;
    uint32_t fetch_height = fetch.size_2d.height + 1;
    if (guest_address == 0) {
      guest_address = fetch_address;
    }
    if (width == 0) {
      width = fetch_width;
    }
    if (height == 0) {
      height = fetch_height;
    }

    format = static_cast<xenos::TextureFormat>(fetch.format);
    is_tiled = fetch.tiled;
    endian = fetch.endianness;

    if (frontbuffer_ptr != 0 && fetch_address != 0 &&
        frontbuffer_ptr != fetch_address) {
      static uint32_t swap_ptr_mismatch_log_count = 0;
      if (swap_ptr_mismatch_log_count < 32) {
        XELOGW("MetalTextureCache: Swap ptr mismatch packet=0x{:08X} fetch0=0x{:08X}",
               frontbuffer_ptr, fetch_address);
        swap_ptr_mismatch_log_count++;
      }
    }
  }

  if (guest_address == 0 || width == 0 || height == 0) {
    return nil;
  }

  format_out = format;
  width_out = width;
  height_out = height;

  MTLPixelFormat metal_format = MTLPixelFormatRGBA8Unorm;
  uint32_t bpp = GetBytesPerPixel(format);
  if (bpp == 0) {
    bpp = 4;
  }

  // Always output swap as RGBA8 for simplicity.
  if (swap_texture_ && (swap_texture_width_ != width ||
                        swap_texture_height_ != height)) {
    swap_texture_ = nil;
  }

  if (!swap_texture_) {
    auto& provider = command_processor_.GetMetalProvider();
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:metal_format
                                    width:width
                                   height:height
                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    if (provider.has_unified_memory()) {
      desc.storageMode = MTLStorageModeShared;
    } else {
      desc.storageMode = MTLStorageModeManaged;
    }

    swap_texture_ = [provider.device() newTextureWithDescriptor:desc];
    if (!swap_texture_) {
      XELOGE("MetalTextureCache: Failed to create {}x{} swap texture",
             width, height);
      return nil;
    }
    swap_texture_width_ = width;
    swap_texture_height_ = height;
  }

  const uint8_t* shared_mem =
      static_cast<const uint8_t*>(shared_memory_.buffer_contents());
  if (!shared_mem) return nil;

  uint64_t required_bytes = uint64_t(width) * uint64_t(height) * uint64_t(bpp);
  if (uint64_t(guest_address) + required_bytes > MetalSharedMemory::kBufferSize) {
    return nil;
  }

  const uint8_t* src = shared_mem + guest_address;

  // Untile + endian swap into temp buffer, then upload.
  uint32_t row_pitch = width * 4;  // RGBA8 output
  untile_buffer_.resize(row_pitch * height);

  // For tiled textures, src_pitch is unused (GetTiledOffset2D computes
  // offsets independently). Pass 0 to make this explicit.
  if (format == xenos::TextureFormat::k_8_8_8_8 ||
      format == xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16) {
    UntileTexture(src, untile_buffer_.data(), width, height, 4,
                  is_tiled ? 0 : width * 4, is_tiled, endian);
  } else {
    // For other formats: linear copy with endian swap as RGBA8.
    UntileTexture(src, untile_buffer_.data(), width, height, bpp,
                  is_tiled ? 0 : width * bpp, is_tiled, endian);
  }

  [swap_texture_ replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:untile_buffer_.data()
                   bytesPerRow:row_pitch];

  return swap_texture_;
}

}  // namespace rex::graphics::metal
