/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/graphics/metal/metal_edram_authority.h"
#include "rex/graphics/metal/metal_exact_cover_predicate.h"
#include "rex/graphics/metal/metal_settings.h"  // depth_float24_* settings
#include "rex/graphics/metal/metal_fetch_constant_type.h"
#include "rex/graphics/metal/metal_resolved_fetch_service.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_load_eligibility.h"

#import <Metal/Metal.h>

#include <dlfcn.h>

#include <fmt/format.h>
#include <algorithm>
#include <string>
#include <vector>

#include "xxhash.h"
#include <rex/types.h>
#include <chrono>
#include <optional>
#include <rex/logging/log_category.h>
#include "rex/math.h"
#include "rex/graphics/texture_address.h"
#include "rex/graphics/metal/metal_texture_key.h"
#include "rex/graphics/metal/metal_packed_base.h"
#include "rex/graphics/metal/metal_texture_format_decode.h"
#include "rex/graphics/texture_util.h"
#include "rex/graphics/trace_writer.h"
#include "rex/memory.h"

#include "metal_irconverter_runtime.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {
// Guest physical memory is 512 MiB; guard reads to it.
constexpr uint32_t kPhysicalSize = 512u * 1024 * 1024;

// Describes how a guest texture format maps to a host upload. `bytes_per_block`
// / `block_log2` / `block_texels` are the compressed-block geometry (1x1 for
// uncompressed 8_8_8_8). `host_format` is the Metal pixel format we upload to;
// for compressed formats we hand the raw (endian-corrected) blocks to the
// hardware BC decoder, for 8_8_8_8 we build linear RGBA8.
struct FormatPlan {
  bool supported;
  bool compressed;
  uint32_t bytes_per_block;
  uint32_t block_log2;   // log2(bytes_per_block), for the tiled addresser.
  uint32_t block_texels;  // 1 (uncompressed) or 4 (DXT).
  MTLPixelFormat host_format;
  // If true, the untiled 16bpp k_4_4_4_4 texels are CPU-expanded to RGBA8 before
  // upload (host_format is RGBA8Unorm) so the same swizzle path as k_8_8_8_8
  // applies. Avoids the channel-reorder load shader XeniOS/Vulkan use.
  bool decode_4444 = false;
  // If true, the untiled 16bpp single-channel k_16 texels are expanded to RGBA8
  // (high byte -> R=G=B, A=255); the fetch swizzle (typically R,R,R,1) selects R.
  bool decode_r16 = false;
  // SNORM variant served to SIGNED texture bindings (the DXBC translator makes
  // signed fetches a SEPARATE binding expecting -1..1 values; guest signed
  // 8-bit texels are two's-complement, i.e. exactly host snorm bytes).
  MTLPixelFormat host_format_signed = MTLPixelFormatInvalid;
  // Host channel swizzle of the shared D3D12/Vulkan tables (RRRR for depth,
  // RGGG for two-channel formats): which host channel each Xenos component
  // X/Y/Z/W reads. Composed with the guest fetch swizzle in FinalizeTexture.
  uint32_t host_swizzle = xenos::REXGPU_SWIZZLE_RGBA;
  // If true, each 32-bit guest word is a k_24_8 / k_24_8_FLOAT depth texel
  // and is CPU-converted to R32Float (host_format) with the shared
  // unorm24 / float20e4 rule (metal_texture_format_decode.h).
  bool decode_depth_unorm = false;
  bool decode_depth_float = false;
  // If true, each 8-byte guest block is a CTX1 (Xenos two-channel normal-map
  // compression) 4x4 block, CPU-decoded to RG8 (host_format RG8Unorm) with
  // the shared load shader's interpolation (texture_load_ctx1.cs.xesl).
  bool decode_ctx1 = false;
  // If true, each 8-byte guest block is a DXT3A (4-bit alpha per texel, the
  // DXT3 alpha block alone) 4x4 block, CPU-expanded to R8 (host_format
  // R8Unorm), as Vulkan's texture_load_dxt3a.cs.xesl does.
  bool decode_dxt3a = false;
  // If true, the untiled 16bpp k_5_6_5 texels are CPU-expanded to RGBA8
  // (host_format RGBA8Unorm): the fallback where B5G6R5Unorm is unavailable.
  bool decode_565 = false;
  // If true, the untiled 16bpp k_1_5_5_5 texels are CPU-expanded to RGBA8
  // (host_format RGBA8Unorm): the fallback where BGR5A1Unorm is unavailable.
  bool decode_1555 = false;
};

// Shared-table channel replication for host formats narrower than RGBA. The
// D3D12/Vulkan texture caches map single-channel guest formats as RRRR and
// two-channel ones as RGGG (a fetch of .w from a k_8 texture returns the
// texel, not 1.0). Halo 3's smoke sprites read their k_8 mask through .w:
// with the identity host swizzle every sprite went opaque (grey squares).
// XE_METAL_HOST_SWIZZLE_LEGACY=1 restores the identity mapping (rollback).
FormatPlan ReplicateNarrowHostChannels(FormatPlan plan, uint32_t swizzle) {
  static const bool s_legacy =
      std::getenv("XE_METAL_HOST_SWIZZLE_LEGACY") != nullptr;
  if (!s_legacy) {
    plan.host_swizzle = swizzle;
  }
  return plan;
}

FormatPlan PlanForFormat(xenos::TextureFormat format, bool bc_supported,
                         bool packed16_supported) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
    // AS_16_16_16_16 (fmt 50): a memory-layout alias of 8888 — same bytes,
    // same decode (JC2's temporal composite fetches its history buffer with
    // it).
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    // k_8_8_8_8_A: the shared GetBaseFormat maps it to 8888 (D3D12 serves it
    // through that host format); until 2026-10-26 it had no plan here.
    case xenos::TextureFormat::k_8_8_8_8_A:
      return {true,  false, 4, 2, 1, MTLPixelFormatRGBA8Unorm,
              false, false, MTLPixelFormatRGBA8Snorm};
    case xenos::TextureFormat::k_8_8:
      // Two-channel (bump/normal maps: swizzle typically (X,X,X,Y), the
      // shader reconstructs Z).
      return ReplicateNarrowHostChannels(
          {true, false, 2, 1, 1, MTLPixelFormatRG8Unorm, false, false,
           MTLPixelFormatRG8Snorm},
          xenos::REXGPU_SWIZZLE_RGGG);
    case xenos::TextureFormat::k_8:
    // k_8_A / k_8_B: GetBaseFormat aliases of k_8 (same bytes, D3D12 R8).
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      return ReplicateNarrowHostChannels(
          {true, false, 1, 0, 1, MTLPixelFormatR8Unorm, false, false,
           MTLPixelFormatR8Snorm},
          xenos::REXGPU_SWIZZLE_RRRR);
    case xenos::TextureFormat::k_32_FLOAT:
      // Raw f32 words (the resolve dest format of the luminance pyramid).
      return ReplicateNarrowHostChannels(
          {true, false, 4, 2, 1, MTLPixelFormatR32Float},
          xenos::REXGPU_SWIZZLE_RRRR);
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      // 128bpp RGBA32F: four raw f32 words per texel, k8in32 swapped per word
      // by SwapEndian. Engaged by Gears of War 3 (4D5308AB), 2026-09-18: two
      // 4096x1 linear tables (UE3 GPU float tables) were served null, 300
      // times per run, and the whole lit world rendered black while emissive
      // geometry and the HUD survived. k_32_32_FLOAT (fmt 37) has no
      // observed engagement and stays unimplemented.
      {
        // XE_METAL_RGBA32F=0: bisect rollback (serves null as before).
        static const bool s_rgba32f_off = [] {
          const char* v = std::getenv("XE_METAL_RGBA32F");
          return v && std::strcmp(v, "0") == 0;
        }();
        if (s_rgba32f_off) {
          return {false, false, 16, 4, 1, MTLPixelFormatInvalid};
        }
      }
      return {true, false, 16, 4, 1, MTLPixelFormatRGBA32Float};
    case xenos::TextureFormat::k_32_32_FLOAT:
      // Two-channel 32-bit float (D3D12 R32G32_FLOAT, RGGG). The shared route
      // serves it since 2026-10-17; this covers its residual classes.
      return ReplicateNarrowHostChannels(
          {true, false, 8, 3, 1, MTLPixelFormatRG32Float},
          xenos::REXGPU_SWIZZLE_RGGG);
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      // 64bpp RGBA16F. Sonic's upscale draw reads its 1x1 EXPOSURE and a
      // 220x180 HDR buffer with this — the NULL fetch fed the tonemap a
      // garbage exposure (white-out candidate). Guest halfwords swap with
      // k8in16 (SwapEndian handles 2-byte units across the 8-byte block).
      // EXPAND is the same half-float storage, with filtering semantics that
      // don't alter the upload representation (Red Dead's full-frame HDR
      // chain declares its resolved RGBA16F surfaces through this alias).
      return {true, false, 8, 3, 1, MTLPixelFormatRGBA16Float};
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
      return ReplicateNarrowHostChannels(
          {true, false, 4, 2, 1, MTLPixelFormatRG16Float},
          xenos::REXGPU_SWIZZLE_RGGG);
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
      return ReplicateNarrowHostChannels(
          {true, false, 2, 1, 1, MTLPixelFormatR16Float},
          xenos::REXGPU_SWIZZLE_RRRR);
    case xenos::TextureFormat::k_4_4_4_4:
      // 16bpp RGBA4, decoded to RGBA8 on the CPU (see decode_4444). block_log2 =
      // log2(2 source bytes) = 1.
      return {true, false, 2, 1, 1, MTLPixelFormatRGBA8Unorm, true, false};
    case xenos::TextureFormat::k_5_6_5:
      // 16bpp packed RGB. After the fetch-endian swap the guest word is
      // R5 (bits 0-4) G6 (5-10) B5 (11-15); Metal's B5G6R5Unorm, like DXGI's,
      // stores B in the low bits. The shared D3D12 table swaps red and blue in
      // its load shader and then applies swizzle RGBB; here the swap is the
      // host swizzle instead (guest R <- host B, G <- G, B <- host R, and the
      // contract's alpha reads blue, i.e. host R). Engaged by Skate 2
      // (4541087F) boot, 2026-09-18: 300 null serves/frame of an 8x8 fmt-4
      // texture before this plan existed.
      //
      // B5G6R5Unorm exists only on Apple GPU families; Intel/AMD Macs abort
      // on the descriptor (Skate 2 on the RX 6900 XT, 2026-10-10). There the
      // texels are expanded to RGBA8 on the CPU (decode_565) in guest
      // channel order, so the host swizzle is the shared table's RGBB.
      if (!packed16_supported) {
        FormatPlan plan = {true, false, 2, 1, 1, MTLPixelFormatRGBA8Unorm};
        plan.decode_565 = true;
        return ReplicateNarrowHostChannels(
            plan, REXGPU_SWIZZLE(R, G, B, B));
      }
      return ReplicateNarrowHostChannels(
          {true, false, 2, 1, 1, MTLPixelFormatB5G6R5Unorm, false, false},
          REXGPU_SWIZZLE(B, G, R, R));
    case xenos::TextureFormat::k_1_5_5_5: {
      // 16bpp packed RGBA 5:5:5:1. After the fetch-endian swap the guest word
      // is R5 (bits 0-4) G5 (5-9) B5 (10-14) A1 (15) (the D3D12 and Vulkan
      // load shaders swap red and blue into B5G5R5A1). Metal's BGR5A1Unorm
      // reads B from bits 0-4, G 5-9, R 10-14, A 15 (device probe,
      // 2026-10-19), so the texels copy raw and the host swizzle swaps red
      // and blue back, as for 5:6:5. Until now this format had no plan and
      // every fetch was served null: Fable II's 256x256 mipped 1:5:5:5
      // textures, whose draws also waited on the GPU for them (2026-10-19).
      // BGR5A1Unorm exists only on Apple GPU families; elsewhere the texels
      // are expanded to RGBA8 on the CPU (decode_1555) in guest channel
      // order, so the host swizzle is the identity (the shared route's GPU
      // expansion takes the mipped ones). Unserved there until 2026-11-06:
      // Fable II 20.7 fps on the Radeon Pro 580X. XE_METAL_TEX_1555=0
      // restores the unserved plan on every GPU.
      static const bool s_1555 = [] {
        const char* v = std::getenv("XE_METAL_TEX_1555");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_1555) {
        return {false, false, 0, 0, 0, MTLPixelFormatInvalid};
      }
      if (!packed16_supported) {
        FormatPlan plan = {true, false, 2, 1, 1, MTLPixelFormatRGBA8Unorm};
        plan.decode_1555 = true;
        return ReplicateNarrowHostChannels(plan,
                                           xenos::REXGPU_SWIZZLE_RGBA);
      }
      return ReplicateNarrowHostChannels(
          {true, false, 2, 1, 1, MTLPixelFormatBGR5A1Unorm, false, false},
          REXGPU_SWIZZLE(B, G, R, A));
    }
    case xenos::TextureFormat::k_16:
      // 16bpp single channel at FULL precision. (Was decode_r16 high-byte
      // RGBA8 expansion — 8-bit quantization banded JC2's 1024x3072 k_16
      // shadow map into visible stripes on every shadow receiver.)
      return ReplicateNarrowHostChannels(
          {true, false, 2, 1, 1, MTLPixelFormatR16Unorm, false, false,
           MTLPixelFormatR16Snorm},
          xenos::REXGPU_SWIZZLE_RRRR);
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return {bc_supported, true, 8, 3, 4, MTLPixelFormatBC1_RGBA};
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return {bc_supported, true, 16, 4, 4, MTLPixelFormatBC2_RGBA};
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return {bc_supported, true, 16, 4, 4, MTLPixelFormatBC3_RGBA};
    // --- 2026-09-04 engagement census additions (openspec
    // implement-metal-texture-formats). Host formats and host swizzles are
    // the shared D3D12/Vulkan table entries. ---
    // --- 2026-09-17 Halo 3 census additions: k_DXT5A (alpha-only DXT5
    // block == BC4 block layout) and k_CTX1 (Xenos normal-map compression,
    // CPU-decoded to RG8 like Vulkan's load shader). Halo 3 fetches both
    // hundreds of times per frame; unserved they left materials flat
    // (neon weapon, blue skin). ---
    case xenos::TextureFormat::k_DXT3A: {
      FormatPlan plan = {true, true, 8, 3, 4, MTLPixelFormatR8Unorm};
      plan.decode_dxt3a = true;
      plan.host_swizzle = xenos::REXGPU_SWIZZLE_RRRR;
      return plan;
    }
    case xenos::TextureFormat::k_DXT5A: {
      FormatPlan plan = {bc_supported, true, 8, 3, 4, MTLPixelFormatBC4_RUnorm};
      plan.host_swizzle = xenos::REXGPU_SWIZZLE_RRRR;
      return plan;
    }
    case xenos::TextureFormat::k_CTX1: {
      FormatPlan plan = {true, true, 8, 3, 4, MTLPixelFormatRG8Unorm};
      plan.decode_ctx1 = true;
      plan.host_swizzle = xenos::REXGPU_SWIZZLE_RGGG;
      return plan;
    }
    case xenos::TextureFormat::k_DXN: {
      // Two-channel BC5 normal maps (Dead or Alive 5, F.E.A.R.).
      FormatPlan plan = {bc_supported, true, 16, 4, 4, MTLPixelFormatBC5_RGUnorm,
                         false, false, MTLPixelFormatBC5_RGSnorm};
      plan.host_swizzle = xenos::REXGPU_SWIZZLE_RGGG;
      return plan;
    }
    case xenos::TextureFormat::k_16_16: {
      FormatPlan plan = {true, false, 4, 2, 1, MTLPixelFormatRG16Unorm,
                         false, false, MTLPixelFormatRG16Snorm};
      plan.host_swizzle = xenos::REXGPU_SWIZZLE_RGGG;
      return plan;
    }
    case xenos::TextureFormat::k_16_16_16_16:
      return {true, false, 8, 3, 1, MTLPixelFormatRGBA16Unorm,
              false, false, MTLPixelFormatRGBA16Snorm};
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      // Same bit layout as RGB10A2 (R in the low bits). Metal has no snorm
      // sibling; a signed fetch keeps the unorm view (Vulkan's optional-format
      // fallback).
      return {true, false, 4, 2, 1, MTLPixelFormatRGB10A2Unorm};
    case xenos::TextureFormat::k_24_8: {
      FormatPlan plan = {true, false, 4, 2, 1, MTLPixelFormatR32Float};
      plan.host_swizzle = xenos::REXGPU_SWIZZLE_RRRR;
      plan.decode_depth_unorm = true;
      return plan;
    }
    case xenos::TextureFormat::k_24_8_FLOAT: {
      FormatPlan plan = {true, false, 4, 2, 1, MTLPixelFormatR32Float};
      plan.host_swizzle = xenos::REXGPU_SWIZZLE_RRRR;
      plan.decode_depth_float = true;
      return plan;
    }
    default:
      return {false, false, 0, 0, 0, MTLPixelFormatInvalid};
  }
}

// Map one guest 3-bit swizzle component (X/Y/Z/W/0/1, values 6/7 clamped to
// 0/1) to a Metal swizzle channel. Our host formats (RGBA8, BC1/2/3_RGBA) all
// store channels in identity RGBA order, so the host-format swizzle is identity
// and the guest fetch swizzle maps straight through.
MTLTextureSwizzle GuestSwizzleComponentToMetal(uint32_t comp) {
  switch (comp) {
    case 0:
      return MTLTextureSwizzleRed;
    case 1:
      return MTLTextureSwizzleGreen;
    case 2:
      return MTLTextureSwizzleBlue;
    case 3:
      return MTLTextureSwizzleAlpha;
    case 4:
    case 6:  // 6/7 are invalid; clamp to constant 0/1 like GuestToHostSwizzle.
      return MTLTextureSwizzleZero;
    default:  // 5, 7
      return MTLTextureSwizzleOne;
  }
}

// True if the 12-bit guest swizzle is the identity RGBA mapping (xyzw), so no
// swizzled view is needed.
bool GuestSwizzleIsIdentity(uint32_t guest_swizzle) {
  return (guest_swizzle & 0xFFF) == ((0u) | (1u << 3) | (2u << 6) | (3u << 9));
}

// Convert a block (or texel) of `bytes` from the guest endianness to host
// little-endian in place. Guest 32bpp is k8in32; guest DXT is normally k8in16
// (each 16-bit word byte-swapped), but honor whatever the fetch declares.
void SwapEndian(uint8_t* data, uint32_t bytes, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}
}  // namespace

MetalTextureCache::MetalTextureCache(void* device,
                                     ::rex::gpu::SharedMemory* shared_memory,
                                     void* view_heap, uint32_t view_heap_slots)
    : device_(device),
      shared_memory_(shared_memory),
      view_heap_(view_heap),
      view_heap_slots_(view_heap_slots) {
  id<MTLBuffer> heap = (__bridge id<MTLBuffer>)view_heap_;
  view_heap_contents_ = [heap contents];
  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
  bc_supported_ = [mtl_device supportsBCTextureCompression];
  // XE_METAL_PACKED16_FALLBACK=1 takes the non-Apple path on Apple GPUs too
  // (to check the CPU expansion against the native format).
  packed16_supported_ = [mtl_device supportsFamily:MTLGPUFamilyApple1] &&
                        !std::getenv("XE_METAL_PACKED16_FALLBACK");
  if (shared_memory_) {
    shared_memory_watch_ = shared_memory_->RegisterGlobalWatch(
        SharedMemoryGlobalWatchCallback, this);
  }
  REXLOG_INFO("MetalTextureCache: BC (DXT) texture compression {}",
         bc_supported_ ? "supported" : "UNSUPPORTED (DXT stays gray)");
  REXLOG_INFO("MetalTextureCache: packed 16-bit formats {}",
         packed16_supported_ ? "supported"
                             : "unavailable (5:6:5 expanded on the CPU)");
}

void MetalTextureCache::EnableTextureHeaps() {
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!device) {
    return;
  }
  // Default: dedicated GPUs only (no unified memory, not an Apple GPU).
  bool enabled = !device.hasUnifiedMemory &&
                 ![device supportsFamily:MTLGPUFamilyApple7];
  if (const char* v = std::getenv("XE_METAL_TEXTURE_HEAPS")) {
    enabled = v[0] == '1';
  }
  if (!enabled) {
    return;
  }
  // Half the working set, at most 8 GiB; past it textures come from the
  // device and are declared per draw as before.
  heap_budget_ = std::min<uint64_t>(device.recommendedMaxWorkingSetSize / 2,
                                    uint64_t(8) << 30);
  heaps_enabled_ = true;
  REXLOG_INFO("MetalTextureHeaps: enabled, budget {} MiB", heap_budget_ >> 20);
}

void* MetalTextureCache::NewHeapTexture(void* descriptor) {
  if (!heaps_enabled_ || !descriptor) {
    return nullptr;
  }
  MTLTextureDescriptor* desc = (__bridge MTLTextureDescriptor*)descriptor;
  if (desc.storageMode != MTLStorageModePrivate) {
    return nullptr;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  std::lock_guard<std::mutex> lock(heap_mutex_);
  const MTLSizeAndAlign size_align =
      [device heapTextureSizeAndAlignWithDescriptor:desc];
  const uint32_t count = heap_count_.load(std::memory_order_relaxed);
  // Newest heap first: older heaps are mostly full.
  for (uint32_t i = count; i-- > 0;) {
    id<MTLHeap> heap = (__bridge id<MTLHeap>)heaps_[i];
    if ([heap maxAvailableSizeWithAlignment:size_align.align] <
        size_align.size) {
      continue;
    }
    id<MTLTexture> texture = [heap newTextureWithDescriptor:desc];
    if (texture) {
      ++heap_textures_;
      return (__bridge_retained void*)texture;
    }
  }
  if (count >= kMaxTextureHeaps) {
    return nullptr;
  }
  constexpr uint64_t kHeapBytes = uint64_t(256) << 20;
  constexpr uint64_t kMiB = uint64_t(1) << 20;
  const uint64_t heap_size = std::max<uint64_t>(
      kHeapBytes, (uint64_t(size_align.size) + kMiB - 1) & ~(kMiB - 1));
  if (heap_bytes_ + heap_size > heap_budget_) {
    return nullptr;
  }
  MTLHeapDescriptor* heap_desc = [[MTLHeapDescriptor alloc] init];
  heap_desc.type = MTLHeapTypeAutomatic;
  heap_desc.storageMode = MTLStorageModePrivate;
  heap_desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
  heap_desc.size = heap_size;
  id<MTLHeap> heap = [device newHeapWithDescriptor:heap_desc];
  if (!heap) {
    REXLOG_WARN("MetalTextureHeaps: heap of {} MiB failed; device allocation",
           heap_size >> 20);
    return nullptr;
  }
  heap.label = [NSString stringWithFormat:@"xenia textures %u", count];
  heaps_[count] = (__bridge_retained void*)heap;
  heap_bytes_ += heap_size;
  heap_count_.store(count + 1, std::memory_order_release);
  REXLOG_INFO("MetalTextureHeaps: heap {} ({} MiB, {} MiB total)", count,
         heap_size >> 20, heap_bytes_ >> 20);
  id<MTLTexture> texture = [heap newTextureWithDescriptor:desc];
  if (!texture) {
    return nullptr;
  }
  ++heap_textures_;
  return (__bridge_retained void*)texture;
}

MetalTextureCache::~MetalTextureCache() {
  if (shared_memory_watch_) {
    shared_memory_->UnregisterGlobalWatch(shared_memory_watch_);
    shared_memory_watch_ = nullptr;
  }
  for (auto& pair : cache_) {
    if (pair.second.texture) {
      id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)pair.second.texture;
      (void)released;
    }
  }
  cache_.clear();
  ++binding_generation_;
  ClearEntryIndex();
  for (auto& pair : parked_) {
    id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)pair.second.texture;
    (void)released;
  }
  parked_.clear();
  for (auto& sv : null_swizzle_views_) {
    if (sv.second.view) {
      id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)sv.second.view;
      (void)released;
    }
  }
  null_swizzle_views_.clear();
  // Per-swizzle exact-cover views are ours (retained +1); the resolved target
  // snapshots and tip textures are owned elsewhere and are not released here.
  for (auto& pair : resolved_targets_) {
    for (auto& sv : pair.second.swizzle_views) {
      if (sv.second.view) {
        id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)sv.second.view;
        (void)released;
      }
    }
    pair.second.swizzle_views.clear();
    for (auto& sv : pair.second.tip_swizzle_views) {
      if (sv.second.view) {
        id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)sv.second.view;
        (void)released;
      }
    }
    pair.second.tip_swizzle_views.clear();
    if (pair.second.fitted.texture) {
      id<MTLTexture> released =
          (__bridge_transfer id<MTLTexture>)pair.second.fitted.texture;
      (void)released;
    }
    for (auto& sv : pair.second.fitted.swizzle_views) {
      if (sv.second.view) {
        id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)sv.second.view;
        (void)released;
      }
    }
    pair.second.fitted = ResolvedTarget::FittedView{};
  }
  resolved_targets_.clear();
  for (auto& pair : scaled_assemblies_) {
    for (auto& sv : pair.second.swizzle_views) {
      if (sv.second.view) {
        id<MTLTexture> released =
            (__bridge_transfer id<MTLTexture>)sv.second.view;
        (void)released;
      }
    }
  }
  scaled_assemblies_.clear();
  // Heap textures retain their heap; these are the cache's own references.
  for (uint32_t h = 0; h < heap_count_.load(); ++h) {
    id<MTLHeap> released = (__bridge_transfer id<MTLHeap>)heaps_[h];
    (void)released;
    heaps_[h] = nullptr;
  }
  heap_count_.store(0);
}

void MetalTextureCache::ReleaseDerivedResolveBridges(
    std::vector<std::pair<uint32_t, void*>>* released_out) {
  ++registry_epoch_;
  for (auto it = resolved_targets_.begin(); it != resolved_targets_.end();) {
    const ResolvedTarget& target = it->second;
    // See the header for the classification. Only a RAM-backed color bridge is
    // derived: its content is in guest RAM (the clear publishes every pending
    // writeback before this point) and its serve path already reads RAM.
    //
    // generation_ram_backed, NOT ram_backed: the latter is sticky for the life
    // of the base, so a later snapshot generation that skipped its writeback
    // would still look RAM-backed and be released with content RAM never
    // received.
    if (!target.generation_ram_backed || target.is_depth) {
      ++it;
      continue;
    }
    // Retires the bridge's slots and cache-owned swizzle views into the retire
    // lists; ClearCache drains them. The snapshot and tip textures themselves
    // are owned elsewhere and are released by their owners, for exactly the
    // bases reported here.
    // Report the texture this bridge actually named, so the owner releases
    // that object rather than whatever now sits at the same base.
    void* bridge_texture = it->second.texture;
    RetireResolvedTarget(it->second);
    if (released_out) {
      released_out->emplace_back(it->first, bridge_texture);
    }
    it = resolved_targets_.erase(it);
  }
  // Column assemblies are derived from RAM-backed column resolves too. Their
  // textures stay with the render-target cache; only serving retires.
  for (auto& pair : scaled_assemblies_) {
    RetireScaledAssembly(pair.second);
  }
  scaled_assemblies_.clear();
}

void MetalTextureCache::RetireAllResolvedTargets() {
  ++registry_epoch_;
  // See the header. Atomic with the snapshot owners' release at the
  // replay-reset seam: a serving entry must never outlive its texture, and a
  // texture must not survive with no register describing it.
  for (auto& pair : resolved_targets_) {
    RetireResolvedTarget(pair.second);
  }
  resolved_targets_.clear();
  for (auto& pair : scaled_assemblies_) {
    RetireScaledAssembly(pair.second);
  }
  scaled_assemblies_.clear();
}

bool MetalTextureCache::RetireResolvedTargetsNaming(void* texture) {
  if (!texture) {
    return false;
  }
  bool named = false;
  for (auto it = resolved_targets_.begin(); it != resolved_targets_.end();) {
    if (it->second.texture == texture) {
      RetireResolvedTarget(it->second);
      it = resolved_targets_.erase(it);
      named = true;
    } else {
      ++it;
    }
  }
  for (auto it = scaled_assemblies_.begin(); it != scaled_assemblies_.end();) {
    if (it->second.texture == texture) {
      RetireScaledAssembly(it->second);
      it = scaled_assemblies_.erase(it);
      named = true;
    } else {
      ++it;
    }
  }
  return named;
}

void MetalTextureCache::ClearCache() {
  // Ordinary cached textures are pure derived content: every one of them is
  // reconstructible from guest RAM on the next fetch, which is what makes a
  // clear observationally neutral. Release the texture, return its descriptor
  // to the null texture, and return the slot for reuse.
  auto* entries = static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
  id<MTLTexture> null_texture = (__bridge id<MTLTexture>)null_texture_;
  auto free_slot = [&](uint32_t slot) {
    if (!slot || slot >= view_heap_slots_) {
      return;
    }
    // A freed slot must not keep describing a texture that no longer exists.
    // AllocateSlot's contract is that a recycled slot is rewritten before use,
    // but "before use" is not "now", and a clear is exactly when a stale
    // descriptor would otherwise outlive its texture. Rebind to the null
    // texture initialization gave every slot -- writing an empty descriptor
    // instead would leave an INVALID resource reference, which is worse than
    // the stale one.
    if (entries && null_texture) {
      WriteSlotDescriptor(slot, (__bridge void*)null_texture);
    }
    free_slots_.push_back(slot);
  };
  for (auto& pair : cache_) {
    free_slot(pair.second.slot);
    if (pair.second.texture) {
      id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)pair.second.texture;
      (void)released;
    }
  }
  cache_.clear();
  ++binding_generation_;
  ClearEntryIndex();
  for (auto& pair : parked_) {
    id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)pair.second.texture;
    (void)released;
  }
  parked_.clear();
  active_textures_.clear();
  // The retire lists exist to hold objects until the submissions that may read
  // them complete. At the clear boundary every submission HAS completed, so
  // draining them unconditionally is both safe and the point: leaving entries
  // queued against retired submissions is the inconsistency the clear must not
  // create.
  for (const auto& entry : stale_slots_) {
    free_slot(entry.second);
  }
  stale_slots_.clear();
  for (const auto& entry : retired_textures_) {
    if (residency_release_) {
      residency_release_(entry.second);
    }
    id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)entry.second;
    (void)released;
  }
  retired_textures_.clear();
  binding_allocation_failed_ = false;
  {
    // Queued guest-write invalidations describe entries that no longer exist.
    std::lock_guard<std::mutex> lock(pending_invalidations_mutex_);
    pending_invalidations_.clear();
    pending_invalidations_nonempty_.store(false, std::memory_order_relaxed);
  }

  // Bridges surviving ReleaseDerivedResolveBridges are RETAINED because each
  // one is either !generation_ram_backed (its snapshot is the only copy of
  // this generation's content, which no writeback put in guest RAM -- nothing
  // re-runs a past resolve) or is_depth (served to fetches from the snapshot
  // regardless of RAM). Releasing either would destroy or change guest-visible
  // content instead of evicting a cache, the same reason the shared
  // ::rex::gpu::RenderTargetCache clear keeps render targets that still own EDRAM data.
  //
  // The retained set is CHARACTERIZED, not bounded: every member satisfies
  // (!generation_ram_backed || is_depth), which
  // retained_bridges_are_authoritative()
  // asserts, and resolved_bridge_count() reports its size after each clear.
  // Nothing here caps how many such bridges can accumulate across repeated
  // clears -- that is a growth property, and the repeat-clear stress gate
  // (task 4.6) is what would establish it. Their slots stay allocated, so the
  // slots freed above can never alias them.
  //
  // Slot accounting after a clear therefore returns toward baseline through
  // free_slots_ rather than by resetting next_slot_.
}

void MetalTextureCache::SetTracePlayback() {
  if (trace_playback_) {
    return;
  }
  trace_playback_ = true;
  if (shared_memory_watch_) {
    shared_memory_->UnregisterGlobalWatch(shared_memory_watch_);
    shared_memory_watch_ = nullptr;
  }
  std::lock_guard<std::mutex> lock(pending_invalidations_mutex_);
  pending_invalidations_.clear();
  pending_invalidations_nonempty_.store(false, std::memory_order_relaxed);
}

void MetalTextureCache::SharedMemoryGlobalWatchCallback(
    void* context, uint32_t address_first, uint32_t address_last,
    [[maybe_unused]] bool invalidated_by_gpu) {
  static_cast<MetalTextureCache*>(context)->QueueInvalidation(address_first,
                                                               address_last);
}

void MetalTextureCache::QueueInvalidation(uint32_t address_first,
                                          uint32_t address_last) {
  if (address_last < address_first) {
    return;
  }
  std::lock_guard<std::mutex> lock(pending_invalidations_mutex_);
  // Memory invalidation normally arrives in page runs. Coalesce adjacent runs
  // here so a decoder rewriting a frame plane produces one GPU-thread lookup,
  // not one per protected page.
  if (!pending_invalidations_.empty()) {
    auto& last = pending_invalidations_.back();
    uint64_t last_start = last.first;
    uint64_t last_end = uint64_t(last.first) + last.second;
    uint64_t new_start = address_first;
    uint64_t new_end = uint64_t(address_last) + uint64_t(1);
    if (new_start <= last_end && new_end >= last_start) {
      uint64_t merged_start = std::min(last_start, new_start);
      uint64_t merged_end =
          std::max(last_end, new_end);
      last.first = uint32_t(merged_start);
      last.second = uint32_t(merged_end - merged_start);
      return;
    }
  }
  pending_invalidations_.emplace_back(address_first,
                                      address_last - address_first + 1);
  pending_invalidations_nonempty_.store(true, std::memory_order_release);
}

void MetalTextureCache::DrainPendingInvalidations() {
  // A range queued after this check is drained by the next call, as one
  // queued after the swap below always was.
  if (!pending_invalidations_nonempty_.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>>& invalidations =
      draining_invalidations_;
  invalidations.clear();
  {
    std::lock_guard<std::mutex> lock(pending_invalidations_mutex_);
    invalidations.swap(pending_invalidations_);
    pending_invalidations_nonempty_.store(false, std::memory_order_relaxed);
  }
  uint32_t evicted = 0;
  static const bool s_census =
      std::getenv("XE_METAL_UNTILE_TIMING") != nullptr;
  for (const auto& range : invalidations) {
    uint32_t n = InvalidateRange(range.first, range.second,
                                 InvalidationScope::kAllRepresentations,
                                 "cpu-watch");
    evicted += n;
    // Census: the watch ranges that evicted entries since the last report,
    // so the writer behind a per-frame re-upload storm can be named.
    if (s_census && n) {
      auto& r = inval_ranges_[(uint64_t(range.first) << 32) | range.second];
      ++r.first;
      r.second += n;
    }
  }
  static uint32_t s_cpu_invalidation_logs = 0;
  if (evicted && s_cpu_invalidation_logs++ < 16) {
    REXLOG_INFO("MetalTextureCache: CPU/GPU writes refreshed {} cached texture{}",
           evicted, evicted == 1 ? "" : "s");
  }
}

// --- Snapshot-bridge temporal-loop lineage probe (XE_METAL_LINEAGE=<base hex>,
// e.g. 11849000 = JC2's composite temporal history buffer). One probe that
// tags every event on the watched base with the shared draw-index clock, a raw
// guest-RAM content hash, and the reader's cache HIT/MISS + RAM/bridge decision,
// so the whole Frame-N-resolve -> RAM-scatter -> Frame-N+1-fetch chain is
// visible in one run and the failing edge is identified (not iterated visually).
static uint32_t xe_lineage_base() {
  static uint32_t b = [] {
    const char* v = std::getenv("XE_METAL_LINEAGE");
    return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
  }();
  return b;
}
static uint32_t xe_lineage_fnv(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
  return h;
}

// The single texture-key definition. Any producer that pre-populates an
// entry for a later draw (the proven slice-4/5R GPU loads did; a shared-cache
// replay will) MUST derive the key identically or it degrades to a safe
// CPU-path miss (never corruption).
// Composes the cache identity from a fetch. The composition itself lives in
// metal_texture_key.h so conformance exercises the REAL key rather than a
// re-implementation; this function only maps a fetch onto its inputs.
//
// Normalizes the mip range HERE and hands it back, so exactly one normalization
// result reaches both this key and the upload that decides the host texture's
// level count.
//
// There is deliberately NO parameter for the range. An earlier shape took it as
// an argument, which left a call site where raw register levels could be
// substituted for the normalized ones — and that substitution proved invisible
// to every gate available: it changes neither the rendered image nor the entry
// count on any corpus trace, because no two fetches there agree on every other
// key field while differing only in their raw range. Rather than ship a defect
// class that no test can see, the seam is removed: a caller cannot pass raw
// levels because it never supplies levels at all.
static uint64_t MakeTextureKey(const xenos::TextureFetch& fetch,
                               bool is_signed, uint32_t base, uint32_t width,
                               uint32_t height, uint32_t depth,
                               uint32_t pitch_texels,
                               uint32_t* normalized_mip_min_out,
                               uint32_t* normalized_mip_max_out) {
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, nullptr, nullptr, nullptr, nullptr, nullptr, &mip_min_level,
      &mip_max_level);
  if (normalized_mip_min_out) *normalized_mip_min_out = mip_min_level;
  if (normalized_mip_max_out) *normalized_mip_max_out = mip_max_level;
  MetalTextureKeyInputs inputs;
  inputs.base = base;
  inputs.width = width;
  inputs.height = height;
  inputs.pitch_texels = pitch_texels;
  // A packed base level is read at a different block offset than an unpacked
  // one of the same base/size (metal_packed_base.h), so the two must not
  // share an entry. Only the effective decision reaches the key: a packed_mips
  // flag on a texture too large to pack changes nothing and stays excluded.
  uint32_t base_packed =
      ComputeMetalPackedBaseOffset(fetch.packed_mips != 0, fetch.dimension,
                                   width, height, fetch.format)
          .packed
          ? 1u
          : 0u;
  // A 3D texture's depth / a stacked texture's layer count is storage
  // identity (bits 22-31; the format occupies 16-21). Single-layer fetches
  // contribute 0 here, so their keys are unchanged.
  uint32_t depth_bits = (depth - 1) & 0x3FFu;
  inputs.format_tiled_endian =
      (depth_bits << 22) | (uint32_t(fetch.format) << 16) |
      (uint32_t(is_signed) << 15) | (uint32_t(fetch.dimension) << 12) |
      (base_packed << 9) | (uint32_t(fetch.tiled) << 8) |
      uint32_t(fetch.endianness);
  // FinalizeTexture bakes the 12-bit guest fetch swizzle into the sampled
  // view, so two fetches identical in every other field but differing in
  // swizzle must NOT share a cache entry (the second would reuse the first's
  // wrong-channel view — alpha read from the wrong channel discards draws).
  inputs.swizzle = uint32_t(fetch.swizzle & 0xFFF);
  inputs.mip_min_level = mip_min_level;
  inputs.mip_max_level = mip_max_level;
  // Storage identity the key used to omit (2026-10-26): the raw mip address,
  // the raw packed-mips flag and the stacked flag. Census found no in-flight
  // mis-serve from them, but two fetches differing only there describe
  // different guest bytes, as D3D12's TextureKey (mip_page, packed_mips) does.
  inputs.mip_address = uint32_t(fetch.mip_address);
  inputs.packed_mips = fetch.packed_mips ? 1u : 0u;
  inputs.stacked = fetch.stacked ? 1u : 0u;
  return MakeMetalTextureKey(inputs);
}


// Shared service-classification input builder: the ONE place the
// exact-cover and bridge inputs are computed. GetTextureSlot's serve chain
// (and any future consumer deciding how a resolve-backed fetch is served)
// classifies through this — the guards live in
// ClassifyResolvedFetch/ExactCoverSnapshotEligible and are never
// duplicated. Returns false when the base has no registered resolve target.
bool MetalTextureCache::BuildResolvedServiceInputs(
    const xenos::TextureFetch& fetch, bool is_signed,
    ResolvedFetchServiceInputs* out) const {
  // Stage 3 is DEFAULT ON (XE_METAL_STAGE3=0 disables) — keep in sync with
  // the writeback-cap reader in metal_command_processor.mm.
  static const bool s_stage3 = [] {
    const char* v = std::getenv("XE_METAL_STAGE3");
    return !v || v[0] != '0';
  }();
  static const bool s_color_bridge =
      !s_stage3 || std::getenv("XE_METAL_COLOR_BRIDGE") != nullptr;
  static const bool s_tip_ram_fetch =
      std::getenv("XE_METAL_TIP_RAM_FETCH") != nullptr;
  out->has_resolved_target = false;
  auto rit = resolved_targets_.find(fetch.base_address << 12);
  if (rit == resolved_targets_.end() || !rit->second.texture) {
    return false;
  }
  const ResolvedTarget& rt = rit->second;
  uint32_t fetch_w = fetch.size_2d.width + 1;
  uint32_t fetch_h = fetch.size_2d.height + 1;
  // A RAM-backed R32 resolve must use RAM unless the full texture-fetch layout
  // is proven equivalent (format, tiling, pitch, mips, extent, endian). The
  // former dimensions-only shortcut could bypass partial/offset writebacks.
  // Non-RAM-backed R32 destinations still use the ordinary bridge condition.
  bool covered_r32f =
      !rt.is_depth && !rt.ram_backed && rt.width >= fetch_w &&
      rt.height >= fetch_h &&
      rt.texture_pixel_format == MTLPixelFormatR32Float;
  // DIAGNOSTIC (stabilize 6.6): XE_METAL_TIP_RAM_FETCH=1 forces 1x1 R32F
  // fetches (the exposure tips) down the RAM path.
  if (s_tip_ram_fetch && covered_r32f && fetch_w == 1 && fetch_h == 1 &&
      !rt.is_depth && !s_color_bridge && rt.ram_backed) {
    covered_r32f = false;
  }
  out->exact = ExactCoverInputs{
      rt.is_depth,
      s_color_bridge,
      rt.ram_backed,
      rt.full_rect,
      is_signed,
      rt.texture_pixel_format == MTLPixelFormatRGBA8Unorm,
      fetch.format,
      bool(fetch.tiled),
      fetch.dimension,
      bool(fetch.stacked),
      fetch.mip_min_level,
      fetch.mip_max_level,
      fetch_w,
      fetch_h,
      uint32_t(fetch.pitch) << 5,
      rt.width,
      rt.height,
      uint32_t(fetch.endianness),
      rt.wb_endian};
  out->covered_r32f = covered_r32f;
  out->depth_fetch_composite = DepthFetchIsComposite(
      rt, fetch.base_address << 12, fetch_w, fetch_h, uint32_t(fetch.pitch) << 5);
  // XE_METAL_SCALED_COVER=0: exact rollback of scaled-cover serving (scaled
  // sessions then serve these fetches from 1x RAM, as before it existed).
  static const bool s_scaled_cover_enabled =
      ParseMetalEnvironmentFlag(std::getenv("XE_METAL_SCALED_COVER"), true);
  if (IsDrawResolutionScaled() && s_scaled_cover_enabled) {
    // Snapshot formats whose texels are exactly what the RAM upload of the
    // fetch format decodes to.
    MTLPixelFormat snapshot_format = MTLPixelFormat(rt.texture_pixel_format);
    // GTA IV's full-screen HDR chain uses EXPAND. Refusing this alias loses
    // scaled scene detail before FXAA. Set to 0 for the pre-fix RAM fallback.
    static const bool s_expand_cover = ParseMetalEnvironmentFlag(
        std::getenv("XE_METAL_SCALED_COVER_EXPAND"), true);
    const bool fetch_is_rgba16_float =
        ScaledCoverRGBA16FloatFormat(fetch.format, s_expand_cover);
    bool format_matches =
        (snapshot_format == MTLPixelFormatRGBA8Unorm &&
         fetch.format == xenos::TextureFormat::k_8_8_8_8) ||
        (snapshot_format == MTLPixelFormatRGBA16Float &&
         fetch_is_rgba16_float) ||
        (snapshot_format == MTLPixelFormatR32Float &&
         fetch.format == xenos::TextureFormat::k_32_FLOAT);
    ScaledCoverInputs& scaled = out->scaled;
    scaled.scaled_session = true;
    scaled.rt_is_depth = rt.is_depth;
    scaled.ram_backed = rt.ram_backed;
    // A signed fetch of a float format reads the same texels as an unsigned
    // one (no SNORM view exists for it; the RAM upload makes one RGBA16Float
    // or R32Float texture for both), so the snapshot stands in for it too.
    // Project Gotham Racing 3 binds its 1024x600 RGBA16F scene copy signed
    // for the tone-map chain: served from 1x RAM, the whole picture at a 2x
    // draw scale was the 1x image with every pixel doubled.
    // XE_METAL_SCALED_COVER_SIGNED_FLOAT=0 restores the refusal.
    static const bool s_signed_float_cover = ParseMetalEnvironmentFlag(
        std::getenv("XE_METAL_SCALED_COVER_SIGNED_FLOAT"), true);
    const bool fetch_is_float =
        fetch_is_rgba16_float ||
        fetch.format == xenos::TextureFormat::k_32_FLOAT;
    scaled.is_signed = is_signed && !(s_signed_float_cover && fetch_is_float);
    scaled.snapshot_format_matches_fetch = format_matches;
    scaled.fetch_tiled = bool(fetch.tiled);
    scaled.fetch_dimension = fetch.dimension;
    scaled.fetch_stacked = bool(fetch.stacked);
    scaled.fetch_mip_min_level = fetch.mip_min_level;
    scaled.fetch_mip_max_level = fetch.mip_max_level;
    scaled.fetch_width = fetch_w;
    scaled.fetch_height = fetch_h;
    scaled.fetch_pitch_texels = uint32_t(fetch.pitch) << 5;
    scaled.snapshot_width =
        rt.texture_width / draw_resolution_scale_x_;
    scaled.snapshot_height =
        rt.texture_height / draw_resolution_scale_y_;
    scaled.rows_written = rt.scaled_rows_written;
    scaled.fetch_endianness = uint32_t(fetch.endianness);
    scaled.wb_endian = rt.wb_endian;
    // A fetch wider than the base's own snapshot may span a column assembly
    // at this base (columns of a wider destination).
    auto assembly_it = scaled_assemblies_.find(fetch.base_address << 12);
    if (scaled.snapshot_width != fetch_w &&
        assembly_it != scaled_assemblies_.end() &&
        assembly_it->second.texture) {
      const ScaledAssembly& assembly = assembly_it->second;
      id<MTLTexture> assembly_texture =
          (__bridge id<MTLTexture>)assembly.texture;
      MTLPixelFormat assembly_format = assembly_texture.pixelFormat;
      scaled.snapshot_format_matches_fetch =
          (assembly_format == MTLPixelFormatRGBA8Unorm &&
           fetch.format == xenos::TextureFormat::k_8_8_8_8) ||
          (assembly_format == MTLPixelFormatRGBA16Float &&
           fetch_is_rgba16_float) ||
          (assembly_format == MTLPixelFormatR32Float &&
           fetch.format == xenos::TextureFormat::k_32_FLOAT);
      scaled.snapshot_width = assembly.width;
      scaled.snapshot_height = assembly.height;
      // Rows are complete once the full-height columns cover the width.
      scaled.rows_written =
          assembly.covered_width >= assembly.width ? assembly.height : 0;
      scaled.wb_endian = assembly.wb_endian;
      scaled.from_assembly = true;
    }
  }
  out->has_resolved_target = true;
  return true;
}

std::string MetalTextureCache::FormatDimensionCensus() const {
  static const char* kDim[4] = {"1d", "2d", "3d", "cube"};
  std::string out = "MetalTexDimCensus uploads:";
  for (uint32_t i = 0; i < 16; ++i) {
    if (!dim_census_[i]) {
      continue;
    }
    out += fmt::format(" {}{}{}={}", kDim[i >> 2], (i & 1) ? "_stacked" : "",
                       (i & 2) ? "_mip" : "", dim_census_[i]);
    if (dim_census_signed_[i]) {
      out += fmt::format("(signed {})", dim_census_signed_[i]);
    }
  }
  out += fmt::format(" | null: dim={} fmt={} volume_decoded={} other={}",
                     null_census_dim_, null_census_fmt_,
                     null_census_volume_decoded_, null_census_other_);
  out += fmt::format(" | 1d: real={} empty={} | volume_mips_dropped: 3d={} "
                     "stacked={}",
                     census_1d_real_, census_1d_empty_,
                     census_3d_mips_dropped_, census_stacked_mips_dropped_);
  bool any_dropped = false;
  for (uint32_t f = 0; f < 64; ++f) {
    if (mips_dropped_census_[f]) {
      out += fmt::format("{}fmt{}={}", any_dropped ? " " : " | mips_dropped: ",
                         f, mips_dropped_census_[f]);
      any_dropped = true;
    }
  }
  return out;
}

ResolvedFetchService MetalTextureCache::ClassifyFetchService(
    const xenos::TextureFetch& fetch, bool is_signed) const {
  ResolvedFetchServiceInputs in;
  BuildResolvedServiceInputs(fetch, is_signed, &in);
  return ClassifyResolvedFetch(in);
}

uint32_t MetalTextureCache::GetSwizzledSnapshotSlot(
    void* source, uint32_t guest_swizzle,
    std::unordered_map<uint32_t, ResolvedTarget::SwizzleView>& views) {
  auto sv_it = views.find(guest_swizzle);
  if (sv_it != views.end()) {
    active_textures_.push_back(sv_it->second.view);
    return sv_it->second.slot;
  }
  id<MTLTexture> src = (__bridge id<MTLTexture>)source;
  MTLTextureSwizzleChannels channels = MTLTextureSwizzleChannelsMake(
      GuestSwizzleComponentToMetal((guest_swizzle >> 0) & 0b111),
      GuestSwizzleComponentToMetal((guest_swizzle >> 3) & 0b111),
      GuestSwizzleComponentToMetal((guest_swizzle >> 6) & 0b111),
      GuestSwizzleComponentToMetal((guest_swizzle >> 9) & 0b111));
  id<MTLTexture> view =
      [src newTextureViewWithPixelFormat:src.pixelFormat
                            textureType:src.textureType
                                 levels:NSMakeRange(0, src.mipmapLevelCount)
                                 slices:NSMakeRange(0, src.arrayLength)
                                swizzle:channels];
  if (!view) {
    binding_allocation_failed_ = true;
    return 0;
  }
  uint32_t view_slot = AllocateSlot();
  if (!view_slot) {
    return 0;
  }
  auto* entries = static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
  WriteSlotDescriptor(view_slot, (__bridge void*)view);
  void* view_ptr = (__bridge_retained void*)view;
  views.emplace(guest_swizzle, ResolvedTarget::SwizzleView{view_ptr, view_slot});
  active_textures_.push_back(view_ptr);
  return view_slot;
}

namespace {
bool DepthBridgeOff() {
  static const bool s_off = [] {
    const char* v = std::getenv("XE_METAL_DEPTH_BRIDGE");
    return v && v[0] == '0';
  }();
  return s_off;
}
bool SnapshotFitEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_SNAPSHOT_FIT");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}
}  // namespace

// The snapshot OBJECT's guest dims. Not the registered rt.width/height: a
// strip-resolved depth target (JC2/RDR/LEGO 1280x384 + 1280x336 strips) is
// joined into a full-size parent texture while the registration tracks the
// last strip, and a fetch of the parent's size needs no fit (fitting it to
// one strip blanked the lower half of the scene depth: corpus A/B 2026-10-03).
void MetalTextureCache::CacheSnapshotProperties(ResolvedTarget& rt) {
  id<MTLTexture> tex = (__bridge id<MTLTexture>)rt.texture;
  rt.texture_width = tex ? uint32_t(tex.width) : 0;
  rt.texture_height = tex ? uint32_t(tex.height) : 0;
  rt.texture_pixel_format = tex ? uint32_t(tex.pixelFormat) : 0;
}

void MetalTextureCache::SnapshotObjectDims(const ResolvedTarget& rt,
                                           uint32_t* width_out,
                                           uint32_t* height_out) const {
  *width_out = rt.texture_width / std::max(1u, draw_resolution_scale_x_);
  *height_out = rt.texture_height / std::max(1u, draw_resolution_scale_y_);
}

bool MetalTextureCache::SnapshotFitApplies(const ResolvedTarget& rt,
                                           uint32_t fetch_w, uint32_t fetch_h,
                                           bool covered_r32f) const {
  if (!SnapshotFitEnabled() || !snapshot_fit_blit_ || !rt.texture) {
    return false;
  }
  if (DepthBridgeOff() && rt.is_depth) {
    return false;
  }
  if (!fetch_w || !fetch_h || fetch_w > 8192 || fetch_h > 8192) {
    return false;
  }
  uint32_t snap_w = 0, snap_h = 0;
  SnapshotObjectDims(rt, &snap_w, &snap_h);
  if (fetch_w == snap_w && fetch_h == snap_h) {
    return false;
  }
  // The derived 1x1 tip view owns the covered-R32F 1x1 case.
  if (covered_r32f && fetch_w == 1 && fetch_h == 1) {
    return false;
  }
  return true;
}

bool MetalTextureCache::DepthFetchIsComposite(const ResolvedTarget& rt,
                                              uint32_t base, uint32_t fetch_w,
                                              uint32_t fetch_h,
                                              uint32_t fetch_pitch) const {
  // XE_METAL_DEPTH_COMPOSITE_RAM=0: rollback (the snapshot at the fetch base
  // serves every depth fetch, fitted when the sizes differ).
  static const bool s_enabled = ParseMetalEnvironmentFlag(
      std::getenv("XE_METAL_DEPTH_COMPOSITE_RAM"), true);
  if (!s_enabled || !rt.is_depth || !rt.texture) {
    return false;
  }
  uint32_t snap_w = 0, snap_h = 0;
  SnapshotObjectDims(rt, &snap_w, &snap_h);
  if (fetch_w <= snap_w && fetch_h <= snap_h) {
    return false;
  }
  if (!fetch_pitch) {
    fetch_pitch = fetch_w;
  }
  if (rt.composite_epoch == registry_epoch_ && rt.composite_width == fetch_w &&
      rt.composite_height == fetch_h && rt.composite_pitch == fetch_pitch) {
    return rt.composite;
  }
  // Depth fetches are 32bpp (k_24_8 / k_24_8_FLOAT; ClassifyResolvedFetch
  // sends every other format to RAM before this matters).
  uint64_t fetch_bytes = uint64_t(rex::align(fetch_pitch, 32u)) *
                         rex::align(fetch_h, 32u) * 4;
  auto fetch_extent =
      metal::GuestRange::FromBounds(base, uint64_t(base) + fetch_bytes);
  bool composite = false;
  if (fetch_extent) {
    for (const auto& pair : resolved_targets_) {
      const ResolvedTarget& other = pair.second;
      // Strips and columns joined into this snapshot's object are part of it.
      if (pair.first == base || !other.texture || other.texture == rt.texture) {
        continue;
      }
      auto other_extent = metal::GuestRange::FromBaseLength(
          other.guest_extent_length ? other.guest_extent_start : pair.first,
          std::max(other.guest_extent_length, 1u));
      if (other_extent && fetch_extent->Overlaps(*other_extent)) {
        composite = true;
        break;
      }
    }
  }
  rt.composite_epoch = registry_epoch_;
  rt.composite_width = fetch_w;
  rt.composite_height = fetch_h;
  rt.composite_pitch = fetch_pitch;
  rt.composite = composite;
  return composite;
}

void MetalTextureCache::RetireFittedView(ResolvedTarget::FittedView& fitted) {
  uint64_t retire_submission = RetireSubmission();
  if (fitted.slot) {
    RetireSlot(retire_submission, fitted.slot);
  }
  if (fitted.texture) {
    retired_textures_.emplace_back(retire_submission, fitted.texture);
  }
  for (auto& sv : fitted.swizzle_views) {
    RetireSlot(retire_submission, sv.second.slot);
    retired_textures_.emplace_back(retire_submission, sv.second.view);
  }
  fitted = ResolvedTarget::FittedView{};
}

bool MetalTextureCache::EnsureFittedSnapshot(ResolvedTarget& rt,
                                             uint32_t fetch_w,
                                             uint32_t fetch_h) {
  ResolvedTarget::FittedView& current = rt.fitted;
  // Steady state (every draw sampling a fitted view): the view already holds
  // this snapshot generation at these dims. A view is only ever blitted from
  // a source of its own pixel format (a mismatch retires it first), so the
  // format check below cannot fail here; skip its Objective-C reads.
  if (current.texture && current.width == fetch_w &&
      current.height == fetch_h && current.source == rt.texture &&
      current.source_generation == rt.generation) {
    return true;
  }
  id<MTLTexture> src = (__bridge id<MTLTexture>)rt.texture;
  ResolvedTarget::FittedView& fv = rt.fitted;
  // A previous view at other dims carries its content (the union of the
  // rects resolved so far, as guest RAM would hold it) into the new one.
  ResolvedTarget::FittedView carried;
  if (fv.texture) {
    id<MTLTexture> cur = (__bridge id<MTLTexture>)fv.texture;
    if (fv.width != fetch_w || fv.height != fetch_h ||
        cur.pixelFormat != src.pixelFormat) {
      if (cur.pixelFormat == src.pixelFormat) {
        carried = fv;
        fv = ResolvedTarget::FittedView{};
      } else {
        RetireFittedView(fv);
      }
    }
  }
  const uint32_t sx = draw_resolution_scale_x_, sy = draw_resolution_scale_y_;
  if (!fv.texture) {
    uint32_t slot = AllocateSlot();
    if (!slot) {
      if (carried.texture) {
        RetireFittedView(carried);
      }
      return false;
    }
    MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
    desc.textureType = MTLTextureType2DArray;
    desc.pixelFormat = src.pixelFormat;
    desc.width = fetch_w * sx;
    desc.height = fetch_h * sy;
    desc.arrayLength = 1;
    desc.mipmapLevelCount = 1;
    desc.usage = MTLTextureUsageShaderRead;
    // The texels the snapshot does not cover are zeroed once at creation
    // (deterministic stand-in for guest RAM the game does not sample); the
    // GPU blit below writes the covered rect. Discrete GPUs (texture heaps):
    // a Private heap texture zeroed by a GPU blit -- the Shared texture's
    // CPU zero fill was 2.5% of Red Dead's command-processor thread on the
    // Mac Pro, and its texels lived in system memory. Otherwise Shared,
    // zeroed from the CPU.
    id<MTLTexture> tex = nil;
    if (heaps_enabled_ && snapshot_zero_) {
      desc.storageMode = MTLStorageModePrivate;
      tex = (__bridge_transfer id<MTLTexture>)NewHeapTexture((__bridge void*)desc);
      if (tex && !snapshot_zero_((__bridge void*)tex, fetch_w * sx, fetch_h * sy)) {
        tex = nil;
      }
    }
    const bool cpu_zero = !tex;
    if (cpu_zero) {
      desc.storageMode = MTLStorageModeShared;
      tex = [src.device newTextureWithDescriptor:desc];
    }
    if (!tex) {
      free_slots_.push_back(slot);
      if (carried.texture) {
        RetireFittedView(carried);
      }
      return false;
    }
    if (cpu_zero) {
      // 16 bytes/texel covers every snapshot pixel format; replaceRegion reads
      // width*bpp bytes from each 16*width-byte row.
      const size_t row_bytes = size_t(fetch_w) * sx * 16;
      std::vector<uint8_t> zeros(row_bytes * size_t(fetch_h) * sy, 0);
      [tex replaceRegion:MTLRegionMake2D(0, 0, fetch_w * sx, fetch_h * sy)
             mipmapLevel:0
                   slice:0
               withBytes:zeros.data()
             bytesPerRow:row_bytes
           bytesPerImage:0];
    }
    WriteSlotDescriptor(slot, (__bridge void*)tex);
    fv.texture = (__bridge_retained void*)tex;
    fv.slot = slot;
    fv.width = fetch_w;
    fv.height = fetch_h;
    fv.source = nullptr;
    fv.source_generation = 0;
    if (carried.texture) {
      snapshot_fit_blit_(carried.texture, fv.texture,
                         std::min(carried.width, fetch_w) * sx,
                         std::min(carried.height, fetch_h) * sy);
      RetireFittedView(carried);
    }
    static uint32_t s_fit_logs = 0;
    if (s_fit_logs < 16) {
      ++s_fit_logs;
      REXLOG_INFO(
          "MetalTex: snapshot fit base view {}x{} (snapshot object {}x{}, "
          "registered {}x{}, depth={}) -> slot {}",
          fetch_w, fetch_h, uint32_t(src.width) / sx, uint32_t(src.height) / sy,
          rt.width, rt.height, rt.is_depth, slot);
    }
  }
  if (fv.source != rt.texture || fv.source_generation != rt.generation) {
    uint32_t snap_w = 0, snap_h = 0;
    SnapshotObjectDims(rt, &snap_w, &snap_h);
    const uint32_t copy_w = std::min(snap_w, fetch_w) * sx;
    const uint32_t copy_h = std::min(snap_h, fetch_h) * sy;
    if (!snapshot_fit_blit_(rt.texture, fv.texture, copy_w, copy_h)) {
      return false;
    }
    fv.source = rt.texture;
    fv.source_generation = rt.generation;
  }
  return true;
}

uint32_t MetalTextureCache::ServeFittedSnapshot(ResolvedTarget& rt,
                                                uint32_t fetch_w,
                                                uint32_t fetch_h,
                                                uint32_t guest_swizzle,
                                                bool identity) {
  ResolvedTarget::FittedView& fv = rt.fitted;
  // Bind time never blits (the draw's encoder is open): serve only a view
  // prepared for exactly this snapshot generation and these dims.
  if (!fv.texture || fv.width != fetch_w || fv.height != fetch_h ||
      fv.source != rt.texture || fv.source_generation != rt.generation) {
    return 0;
  }
  if (identity) {
    active_textures_.push_back(fv.texture);
    return fv.slot;
  }
  return GetSwizzledSnapshotSlot(fv.texture, guest_swizzle, fv.swizzle_views);
}

void MetalTextureCache::PrepareSnapshotFit(
    const xenos::TextureFetch& fetch, bool is_signed) {
  if (!SnapshotFitEnabled() || !snapshot_fit_blit_) {
    return;
  }
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked ||
      fetch.stacked) {
    return;
  }
  auto rit = resolved_targets_.find(fetch.base_address << 12);
  if (rit == resolved_targets_.end() || !rit->second.texture) {
    return;
  }
  const uint32_t fetch_w = fetch.size_2d.width + 1;
  const uint32_t fetch_h = fetch.size_2d.height + 1;
  uint32_t snap_w = 0, snap_h = 0;
  SnapshotObjectDims(rit->second, &snap_w, &snap_h);
  if (fetch_w == snap_w && fetch_h == snap_h) {
    return;  // the common case: nothing to prepare
  }
  ResolvedFetchServiceInputs in;
  BuildResolvedServiceInputs(fetch, is_signed, &in);
  if (ClassifyResolvedFetch(in) != ResolvedFetchService::kBridgeSnapshot) {
    return;
  }
  if (!SnapshotFitApplies(rit->second, fetch_w, fetch_h, in.covered_r32f)) {
    return;
  }
  EnsureFittedSnapshot(rit->second, fetch_w, fetch_h);
}

uint32_t MetalTextureCache::GetTextureSlot(
    const xenos::TextureFetch& fetch, bool is_signed) {
  uint32_t slot = GetTextureSlotUnswizzledNull(fetch, is_signed);
  if (slot || binding_allocation_failed_ || !null_texture_ ||
      !IsTextureFetchConstantServable(
          fetch.type, ::rex::graphics::metal::GetMetalSettings().gpu_allow_invalid_fetch_constants.Get())) {
    return slot;
  }
  // The fetch swizzle's constant channels (4/6 = 0, 5/7 = 1) do not read
  // memory, so they hold for a fetch nothing could be loaded for. Halo 4's
  // Bink shader takes alpha from a base-0 1x1 fetch swizzled to 1111 and
  // multiplies its output by it; the bare null texture (0, 0, 0, 0) made
  // every video frame black. The null texture is zero in all channels, so
  // only the constant-one channels need a view.
  uint32_t one_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t comp = (fetch.swizzle >> (i * 3)) & 0b111;
    if (comp == 5 || comp == 7) {
      one_mask |= 1u << i;
    }
  }
  if (!one_mask) {
    return 0;
  }
  uint32_t null_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    null_swizzle |= ((one_mask >> i) & 1 ? 5u : 4u) << (i * 3);
  }
  size_t null_views_before = null_swizzle_views_.size();
  slot = GetSwizzledSnapshotSlot(null_texture_, null_swizzle,
                                 null_swizzle_views_);
  if (null_swizzle_views_.size() != null_views_before) {
    REXLOG_INFO(
        "MetalTex: constant-swizzle null view {:03X} for unservable fetch "
        "base={:08X} fmt={} swz={:03X}",
        null_swizzle, fetch.base_address << 12, uint32_t(fetch.format),
        fetch.swizzle & 0xFFF);
  }
  // Out of slots or views: the plain null texture, as before.
  binding_allocation_failed_ = false;
  return slot;
}

uint32_t MetalTextureCache::GetTextureSlotUnswizzledNull(
    const xenos::TextureFetch& fetch, bool is_signed) {
  if (!trace_playback_) {
    DrainPendingInvalidations();
  }
  EnforceSlotHeadroom();
  // Diagnostic: log why a fetch resolves to the null (gray) texture. The
  // gameplay gray screen is a composite pass sampling slot 0; this shows the
  // fetch (base/format/dimension) so we know whether it's an unsupported format,
  // a non-2D dimension, or a resolve-backed base with no data.
  static uint32_t s_null_log = 0;
  auto log_null = [&](const char* reason) {
    if (!std::strcmp(reason, "dim")) {
      ++null_census_dim_;
    } else if (!std::strcmp(reason, "fmt")) {
      ++null_census_fmt_;
    } else if (!std::strcmp(reason, "volume-decoded")) {
      ++null_census_volume_decoded_;
    } else {
      ++null_census_other_;
    }
    if (s_null_log < 300) {
      ++s_null_log;
      REXLOG_INFO(
          "MetalTex NULL ({}): type={} dim={} fmt={} base={:08X} {}x{} "
          "tiled={} swz={:03X}",
          reason, uint32_t(fetch.type), uint32_t(fetch.dimension),
          uint32_t(fetch.format), fetch.base_address << 12,
          fetch.size_2d.width + 1, fetch.size_2d.height + 1,
          uint32_t(fetch.tiled), fetch.swizzle & 0xFFF);
    }
  };
  if (!IsTextureFetchConstantServable(
          fetch.type, ::rex::graphics::metal::GetMetalSettings().gpu_allow_invalid_fetch_constants.Get())) {
    log_null("fetch-type");
    return 0;
  }
  if (xe_lineage_base() && (fetch.base_address << 12) == xe_lineage_base()) {
    REXLOG_WARN("MetalLineage ENTER draw={} base={:08X} signed={} fmt={} filters={}/{} aniso={}",
           lineage_draw_, fetch.base_address << 12, is_signed,
           uint32_t(fetch.format), uint32_t(fetch.mag_filter),
           uint32_t(fetch.min_filter), uint32_t(fetch.aniso_filter));
  }
  // Shader-executor harness (XE_METAL_SYNTH_FETCH=hexbase,R,G,B,A): serve a
  // constant-color texture for that base — a known input pushed through the
  // REAL pipeline (VS, interpolators, sampler, translated PS). Reading the
  // consuming pass's output then fingerprints any channel permutation/scale
  // the translated shader introduces.
  {
    struct SynthFetch {
      uint32_t base = 0;
      uint8_t rgba[4] = {0, 0, 0, 0};
    };
    static const SynthFetch s_synth = [] {
      SynthFetch s;
      if (const char* v = std::getenv("XE_METAL_SYNTH_FETCH")) {
        unsigned b = 0, r = 0, g = 0, bl = 0, a = 255;
        if (sscanf(v, "%x,%u,%u,%u,%u", &b, &r, &g, &bl, &a) >= 4) {
          s.base = b;
          s.rgba[0] = uint8_t(r);
          s.rgba[1] = uint8_t(g);
          s.rgba[2] = uint8_t(bl);
          s.rgba[3] = uint8_t(a);
        }
      }
      return s;
    }();
    if (s_synth.base && (fetch.base_address << 12) == s_synth.base) {
      static void* s_synth_tex = nullptr;
      static uint32_t s_synth_slot = 0;
      if (!s_synth_tex) {
        // Match the fetch's DECLARED dimensions: unnormalized-coordinate
        // fetches read at pixel coords sized for the real texture — an
        // undersized stand-in reads out of bounds (undefined garbage).
        uint32_t sw = fetch.size_2d.width + 1;
        uint32_t sh = fetch.size_2d.height + 1;
        id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
        MTLTextureDescriptor* d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:sw
                                        height:sh
                                     mipmapped:NO];
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = 1;
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [device newTextureWithDescriptor:d];
        if (!tex) {
          binding_allocation_failed_ = true;
          return 0;
        }
        std::vector<uint8_t> px(size_t(sw) * sh * 4);
        for (size_t i = 0; i < size_t(sw) * sh; ++i) {
          std::memcpy(&px[i * 4], s_synth.rgba, 4);
        }
        [tex replaceRegion:MTLRegionMake2D(0, 0, sw, sh)
               mipmapLevel:0
                     slice:0
                 withBytes:px.data()
               bytesPerRow:sw * 4
             bytesPerImage:size_t(sw) * sh * 4];
        s_synth_slot = AllocateSlot();
        if (!s_synth_slot) {
          return 0;
        }
        s_synth_tex = (__bridge_retained void*)tex;
        auto* entries =
            static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
        WriteSlotDescriptor(s_synth_slot, s_synth_tex);
        REXLOG_WARN("MetalSynthFetch: base {:08X} -> constant ({},{},{},{}) slot {}",
               s_synth.base, s_synth.rgba[0], s_synth.rgba[1], s_synth.rgba[2],
               s_synth.rgba[3], s_synth_slot);
      }
      active_textures_.push_back(s_synth_tex);
      return s_synth_slot;
    }
  }
  // RT-as-texture bridge — STAGE 3 hybrid. Served for:
  //   - DEPTH (no guest-RAM encoding of host depth yet);
  //   - color bases whose resolves never enqueued a RAM writeback (dest
  //     formats the writer can't encode — 64bpp HDR etc.);
  //   - RAM-backed color bases whose registered snapshot FULLY COVERS the
  //     fetch (single-buffer dests: JC2's luminance-pyramid levels, SF4's
  //     per-column dests). The snapshot holds exactly what the writeback
  //     scatters, and sampling it is GPU-ordered — the RAM path for these
  //     forced a mid-frame commit+wait PER CONSUMER (JC2: 39 commits/frame,
  //     21.5 -> 12.7 fps when stage 3 went default-ON).
  // Everything else (multi-writeback assemblies, base reuse, strips — the
  // registered dims don't cover the fetch there) reads guest RAM: JC2's
  // final composite reads LAST frame's 1280x720 image while the base's
  // snapshot was re-registered 1280x384 by the mid-frame G-buffer dump —
  // coverage fails -> RAM, exactly the case the bridge can't represent.
  // XE_METAL_COLOR_BRIDGE=1 restores bridge-for-all-color for A/B.
  {
    auto rit = resolved_targets_.find(fetch.base_address << 12);
    // XE_METAL_DEPTH_BRIDGE=0 (diagnostic, 2026-09-19): serve depth resolve
    // destinations from guest RAM (the k_24_8 CPU decode) instead of the
    // depth snapshot, to attribute Gears of War 3's zero occlusion counts.
    const bool s_depth_bridge_off = DepthBridgeOff();
    if (rit != resolved_targets_.end() && rit->second.texture &&
        !(s_depth_bridge_off && rit->second.is_depth)) {
      const ResolvedTarget& rt = rit->second;
      uint32_t fetch_w = fetch.size_2d.width + 1;
      uint32_t fetch_h = fetch.size_2d.height + 1;
      // ONE shared decision: exact-cover and bridge conditions are
      // composed by ClassifyResolvedFetch in this serve order.
      ResolvedFetchServiceInputs service_in;
      BuildResolvedServiceInputs(fetch, is_signed, &service_in);
      ResolvedFetchService service = ClassifyResolvedFetch(service_in);
      if (service == ResolvedFetchService::kRamContent && xe_lineage_base() &&
          (fetch.base_address << 12) == xe_lineage_base() &&
          service_in.scaled.scaled_session) {
        const ScaledCoverInputs& sc = service_in.scaled;
        REXLOG_WARN(
            "MetalLineage SCALED-RAM draw={} base={:08X} fetch {}x{} pitch={} "
            "snapshot {}x{} rows={} fmt_match={} assembly={} endian {}/{}",
            lineage_draw_, fetch.base_address << 12, sc.fetch_width,
            sc.fetch_height, sc.fetch_pitch_texels, sc.snapshot_width,
            sc.snapshot_height, sc.rows_written,
            sc.snapshot_format_matches_fetch, sc.from_assembly,
            sc.fetch_endianness, sc.wb_endian);
      }
      bool covered_r32f = service_in.covered_r32f;
      if (service == ResolvedFetchService::kExactCoverSnapshot ||
          service == ResolvedFetchService::kScaledCoverSnapshot) {
        // Exact-cover RGBA8 fast path: the snapshot is provably the same
        // content the RAM upload would produce (per-condition rationale in
        // metal_exact_cover_predicate.h and the design doc). Scaled cover
        // (draw resolution scaling only) serves the same way: guest RAM holds
        // the snapshot's representative pixels (metal_scaled_cover_predicate.h).
        if (xe_lineage_base() &&
            (fetch.base_address << 12) == xe_lineage_base()) {
          REXLOG_WARN(
              "MetalLineage EXACT-COVER draw={} base={:08X} served snapshot "
              "{}x{} endian={} (RAM upload avoided)",
              lineage_draw_, fetch.base_address << 12, rt.width, rt.height,
              rt.wb_endian);
        }
        // The raw snapshot slot carries no swizzle; the ordinary RAM upload
        // bakes the fetch swizzle into a texture view (FinalizeTexture). Serve
        // the identity case from the raw slot, and every nonidentity guest
        // swizzle from a per-swizzle cached view so the exact-cover fast path
        // reads the same channels the RAM path would.
        uint32_t guest_swizzle = fetch.swizzle & 0xFFF;
        if (service == ResolvedFetchService::kScaledCoverSnapshot &&
            service_in.scaled.from_assembly) {
          // The column assembly at this base, same swizzle handling. On a view
          // failure, fall through to the RAM path (it holds the columns).
          auto assembly_it =
              scaled_assemblies_.find(fetch.base_address << 12);
          if (assembly_it != scaled_assemblies_.end() &&
              assembly_it->second.texture) {
            ScaledAssembly& assembly = assembly_it->second;
            if (xe_lineage_base() &&
                (fetch.base_address << 12) == xe_lineage_base()) {
              REXLOG_WARN(
                  "MetalLineage SCALED-ASSEMBLY draw={} base={:08X} served "
                  "{}x{} assembly (covered {})",
                  lineage_draw_, fetch.base_address << 12, assembly.width,
                  assembly.height, assembly.covered_width);
            }
            if (!rt.requires_fetch_swizzle ||
                GuestSwizzleIsIdentity(guest_swizzle)) {
              active_textures_.push_back(assembly.texture);
              return assembly.slot;
            }
            uint32_t assembly_swz_slot = GetSwizzledSnapshotSlot(
                assembly.texture, guest_swizzle, assembly.swizzle_views);
            if (assembly_swz_slot) {
              return assembly_swz_slot;
            }
            ClearBindingAllocationFailure();
          }
        } else {
          if (!rt.requires_fetch_swizzle ||
              GuestSwizzleIsIdentity(guest_swizzle)) {
            active_textures_.push_back(rt.texture);
            return rt.slot;
          }
          ResolvedTarget& rt_mut = rit->second;
          uint32_t swz_slot = GetSwizzledSnapshotSlot(
              rt_mut.texture, guest_swizzle, rt_mut.swizzle_views);
          if (swz_slot) {
            return swz_slot;
          }
          // View creation or heap allocation failed: fall through to the
          // guest RAM path below (which bakes the swizzle into its own view)
          // rather than serve the unswizzled raw snapshot (wrong channels) or
          // null. Exact-cover is RAM-backed by definition (RAM holds
          // scatter(snapshot)), so the untile reproduces the content.
          ClearBindingAllocationFailure();
        }
      }
      if (service == ResolvedFetchService::kBridgeSnapshot) {
        // The raw snapshot / tip slots carry no swizzle, exactly like the
        // exact-cover path; the ordinary RAM upload would bake the guest fetch
        // swizzle into a view. Honor a non-identity swizzle here too so the
        // bridge reads the same channels the RAM path would.
        uint32_t guest_swizzle = fetch.swizzle & 0xFFF;
        bool identity = !rt.requires_fetch_swizzle ||
                        GuestSwizzleIsIdentity(guest_swizzle);
        // RAM-backed color is the ONLY bridge source with a safe fallback when
        // a swizzled view can't be built (its guest RAM holds the content, so
        // the untile below reproduces it). Depth and non-RAM-backed color have
        // no RAM decode — they must fail (null) rather than bind the raw slot
        // with the wrong channels.
        bool ram_fallback = rt.ram_backed && !rt.is_depth;
        // XE_METAL_TIP_SNAP_VIEW: a 1x1 guest fetch of a larger snapshot must
        // read guest texel (0,0); binding the full WxH snapshot lands the
        // shader's UV mid-texture (8x8 exposure tip -> texel 4,4 — the JC2
        // steady-dark mechanism). Serve the derived 1x1 blit instead;
        // GPU-ordered like the snapshot itself.
        if (covered_r32f && fetch_w == 1 && fetch_h == 1 &&
            (rt.width > 1 || rt.height > 1) && rt.tip_texture) {
          if (xe_lineage_base() &&
              (fetch.base_address << 12) == xe_lineage_base()) {
            REXLOG_WARN(
                "MetalLineage TIP-VIEW draw={} base={:08X} served derived 1x1 "
                "(snapshot texel 0,0; rt={}x{})",
                lineage_draw_, fetch.base_address << 12, rt.width, rt.height);
          }
          if (identity) {
            active_textures_.push_back(rt.tip_texture);
            return rt.tip_slot;
          }
          // Swizzle a view of the TIP texture (not the full snapshot), cached
          // separately so it retires with the tip's own generation.
          uint32_t tip_swz_slot = GetSwizzledSnapshotSlot(
              rit->second.tip_texture, guest_swizzle,
              rit->second.tip_swizzle_views);
          if (tip_swz_slot) {
            return tip_swz_slot;
          }
          // covered_r32f implies !ram_backed: the tip has no RAM fallback.
          log_null("tip-swizzle-view");
          return 0;
        }
        if (xe_lineage_base() && (fetch.base_address << 12) == xe_lineage_base()) {
          REXLOG_WARN(
              "MetalLineage BRIDGE draw={} base={:08X} served SNAPSHOT rt={}x{} "
              "fetch={}x{} is_depth={} ram_backed={} covered_r32f={} "
              "(RAM bypassed)",
              lineage_draw_, fetch.base_address << 12, rt.width, rt.height,
              fetch_w, fetch_h, rt.is_depth, rt.ram_backed, covered_r32f);
        }
        if (SnapshotFitApplies(rt, fetch_w, fetch_h, covered_r32f)) {
          uint32_t fit_slot = ServeFittedSnapshot(rit->second, fetch_w,
                                                  fetch_h, guest_swizzle,
                                                  identity);
          if (fit_slot) {
            return fit_slot;
          }
          // Not prepared for this draw: the raw snapshot, as before.
        }
        if (identity) {
          active_textures_.push_back(rt.texture);
          return rt.slot;
        }
        uint32_t swz_slot = GetSwizzledSnapshotSlot(
            rit->second.texture, guest_swizzle, rit->second.swizzle_views);
        if (swz_slot) {
          return swz_slot;
        }
        if (!ram_fallback) {
          log_null("bridge-swizzle-view");
          return 0;
        }
        // RAM-backed color: fall through to the guest RAM path below, which
        // bakes the swizzle into its own view.
        ClearBindingAllocationFailure();
      }
      // kRamContent: RAM authoritative — fall through to the cache/untile
      // path below.
    }
  }
  // 2D (or stacked), 3D and CUBE are handled. 3D uploads every depth slice
  // from the guest 32x32x4-block tiled layout (texture_address::Tiled3D) into
  // an MTLTextureType3D — the translated shader samples a texture3d, and
  // serving slice 0 as a 2D texture (the pre-2026-09-17 behavior) fed DOA5's
  // 24x24x24 color-grading LUT garbage: every composite came out blue. Cubes
  // upload all 6 faces (guest layout: faces sequential, each a
  // 32x32-block-aligned tiled slice) into an MTLTextureTypeCubeArray — MSC's
  // ForceTextureArray turns the translated TextureCube SRV into
  // texturecube_array. 1D is skipped.
  // Stacked 2D textures (fetch.stacked: a 2D array of stack_depth+1 layers,
  // sampled by tfetch3D through the shader's 2D-array binding with an
  // integer layer) upload every layer at its array-slice stride. DOA5's
  // 24-layer color-grading LUT was served as layer 0 only, so every lookup
  // clamped to one layer (blue 255 across the whole composite).
  // XE_METAL_VOLUME_TEXTURES=0 restores the single-layer behavior for both
  // 3D and stacked textures (exact rollback).
  static const bool s_volume_textures = [] {
    const char* v = std::getenv("XE_METAL_VOLUME_TEXTURES");
    return !v || v[0] != '0';
  }();
  bool is_3d = fetch.dimension == xenos::DataDimension::k3D;
  bool is_cube = fetch.dimension == xenos::DataDimension::kCube;
  const bool is_stacked =
      fetch.dimension == xenos::DataDimension::k2DOrStacked &&
      fetch.stacked != 0;
  const uint32_t depth =
      !s_volume_textures ? 1u
      : is_3d            ? uint32_t(fetch.size_3d.depth) + 1u
      : is_stacked       ? uint32_t(fetch.size_2d.stack_depth) + 1u
                         : 1u;
  if (fetch.dimension == xenos::DataDimension::k1D) {
    if (fetch.type == xenos::FetchConstantType::kTexture &&
        (fetch.base_address || fetch.mip_address)) {
      if (census_1d_real_++ < 16) {
        REXLOG_WARN("MetalTex1D fmt={} base={:08X} width={} tiled={} mips={}..{} "
               "mip_addr={:08X} signed={}",
               uint32_t(fetch.format), fetch.base_address << 12,
               fetch.size_1d.width + 1, uint32_t(fetch.tiled),
               uint32_t(fetch.mip_min_level), uint32_t(fetch.mip_max_level),
               fetch.mip_address << 12, is_signed ? 1 : 0);
      }
    } else {
      ++census_1d_empty_;
    }
  }
  // 1D: the translator binds every 1D fetch as a 2D texture (DXBC
  // FindOrAddTextureBinding) and the guest layout is one linear row, so a
  // 1D texture is served as a width x 1 linear 2D upload. Until 2026-10-17
  // this cache skipped 1D and served the null slot: Rise of the Tomb Raider's
  // tonemap multiplies the scene by a 256x1 k_8 curve, so the whole world
  // composited to black while the HUD (no curve) stayed intact.
  // XE_METAL_TEX_1D=0 restores the null serve.
  static const bool s_tex_1d = [] {
    const char* v = std::getenv("XE_METAL_TEX_1D");
    return !(v && v[0] == '0' && !v[1]);
  }();
  const bool is_1d = fetch.dimension == xenos::DataDimension::k1D &&
                     s_tex_1d && !fetch.tiled &&
                     fetch.size_1d.width < xenos::kTexture2DCubeMaxWidthHeight;
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked && !is_3d &&
      !is_cube && !is_1d) {
    log_null("dim");
    return 0;
  }
  FormatPlan plan =
      PlanForFormat(fetch.format, bc_supported_, packed16_supported_);
  // Diagnostic (XE_METAL_DISABLE_FMT=<n>): serve the null texture for one
  // guest format, to attribute a defect to that format's content.
  static const int s_disable_fmt = [] {
    const char* v = std::getenv("XE_METAL_DISABLE_FMT");
    return v ? atoi(v) : -1;
  }();
  if (s_disable_fmt >= 0 && int(fetch.format) == s_disable_fmt) {
    plan.supported = false;
  }
  if (!plan.supported) {
    log_null("fmt");
    return 0;
  }
  if (is_signed && plan.host_format_signed != MTLPixelFormatInvalid) {
    plan.host_format = plan.host_format_signed;
  }
  if ((is_3d || depth > 1) &&
      (plan.decode_4444 || plan.decode_565 || plan.decode_1555 ||
       plan.decode_r16 ||
       plan.decode_depth_unorm ||
       plan.decode_depth_float || plan.decode_ctx1 || plan.decode_dxt3a)) {
    // The CPU-decoded plans are written per 2D level; no 3D/stacked consumer
    // of them has been measured. Fail closed to the null slot, observably.
    log_null("volume-decoded");
    return 0;
  }

  uint32_t width = (is_3d   ? fetch.size_3d.width
                    : is_1d ? fetch.size_1d.width
                            : fetch.size_2d.width) +
                   1;
  uint32_t height =
      is_1d ? 1u : (is_3d ? fetch.size_3d.height : fetch.size_2d.height) + 1;
  uint32_t base = fetch.base_address << 12;
  uint32_t pitch_texels = fetch.pitch << 5;  // guest pitch is texels/32.
  if (!base || !width || !height || width > 8192 || height > 8192 ||
      base >= kPhysicalSize) {
    log_null("bounds");
    return 0;
  }
  if (!pitch_texels) {
    pitch_texels = (width + 31) & ~31u;
  }

  // The key normalizes internally and hands the range back; the upload below
  // uses THIS value, so identity and content cannot diverge. Normalization is
  // also what makes the excluded fields safe: an absent mip page collapses the
  // range to the base level, and both ends are bounded by the size-derived
  // maximum, so fetches whose raw fields differ but whose ranges are equivalent
  // still share one entry.
  uint32_t normalized_mip_min = 0;
  uint32_t normalized_mip_max = 0;
  uint64_t key =
      MakeTextureKey(fetch, is_signed, base, width, height, depth,
                     pitch_texels, &normalized_mip_min, &normalized_mip_max);
  auto it = cache_.find(key);

  // Lineage probe: this fetch of the watched base is the composite's temporal
  // read. A cache HIT means the untile is SKIPPED and the STALE cached upload is
  // served -- the freshly-resolved guest RAM never reaches the composite (the
  // invalidation/ordering break). ramHash is a strided hash of the guest RAM the
  // untile WOULD read; compare across frames vs the DRAIN ramHash to see whether
  // this reads last frame's resolve (correct temporal) or something stale.
  if (xe_lineage_base() && base == xe_lineage_base()) {
    const uint8_t* g =
        static_cast<const uint8_t*>(static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(base));
    uint32_t rh = 0;
    if (g) {
      size_t region = std::min<size_t>(size_t(pitch_texels) * height * 4u,
                                       size_t(kPhysicalSize - base));
      uint32_t h = 2166136261u;
      for (size_t i = 0; i < region; i += 37) h = (h ^ g[i]) * 16777619u;
      rh = h;
    }
    bool ram_backed = false;
    auto rit = resolved_targets_.find(base);
    if (rit != resolved_targets_.end()) ram_backed = rit->second.ram_backed;
    REXLOG_WARN(
        "MetalLineage FETCH draw={} base={:08X} {}x{} pitch={} fmt={} tiled={} "
        "endian={} cache={} ram_backed={} ramHash={:08x}",
        lineage_draw_, base, width, height, pitch_texels,
        uint32_t(fetch.format), uint32_t(fetch.tiled),
        uint32_t(fetch.endianness), it != cache_.end() ? "HIT" : "MISS",
        ram_backed, rh);
  }
  // Census (XE_TEXTURE_KEY_CENSUS=1, diagnostic): shadow the fetch fields the
  // key deliberately omits (mip address, raw packed_mips flag, stacked flag)
  // per key and report ONLY when a cache HIT arrives carrying different ones
  // -- an in-flight mis-serve, not a hypothetical collision. "mips" marks the
  // cases where the entry actually holds levels sourced from mip_address.
  static const bool s_key_census = std::getenv("XE_TEXTURE_KEY_CENSUS") != nullptr;
  if (s_key_census) {
    struct KeyShadow {
      uint32_t mip_address;
      uint8_t packed_mips;
      uint8_t stacked;
    };
    static std::unordered_map<uint64_t, KeyShadow> s_shadow;
    static uint32_t s_lines = 0;
    KeyShadow now{uint32_t(fetch.mip_address) << 12,
                  uint8_t(fetch.packed_mips), uint8_t(fetch.stacked)};
    auto sh = s_shadow.find(key);
    if (it != cache_.end() && sh != s_shadow.end()) {
      const KeyShadow& was = sh->second;
      bool mips_held = normalized_mip_max > 0 || normalized_mip_min > 0;
      if ((was.mip_address != now.mip_address || was.packed_mips != now.packed_mips ||
           was.stacked != now.stacked) && s_lines < 4000) {
        ++s_lines;
        REXLOG_WARN(
            "MetalKeyCensus MISSERVE base={:08X} fmt={} {}x{} depth={} "
            "lv={}..{} mips_held={} stored(mipaddr={:08X} packed={} stacked={}) "
            "incoming(mipaddr={:08X} packed={} stacked={}) fields={}{}{}",
            base, uint32_t(fetch.format), width, height, depth,
            normalized_mip_min, normalized_mip_max, mips_held ? 1 : 0,
            was.mip_address, was.packed_mips, was.stacked, now.mip_address,
            now.packed_mips, now.stacked,
            was.mip_address != now.mip_address ? "M" : "",
            was.packed_mips != now.packed_mips ? "P" : "",
            was.stacked != now.stacked ? "S" : "");
      }
    } else if (it == cache_.end()) {
      // XE_TEXTURE_KEY_CENSUS=selftest: perturb the recorded mip address so
      // every later hit reports "M" -- proves the report path is live.
      static const bool s_selftest =
          std::strcmp(std::getenv("XE_TEXTURE_KEY_CENSUS"), "selftest") == 0;
      if (s_selftest) {
        now.mip_address ^= 0x1000u;
      }
      s_shadow[key] = now;
    }
  }
  if (it != cache_.end()) {
    it->second.last_use = ++use_clock_;
    active_textures_.push_back(it->second.texture);
    return it->second.slot;
  }
  if (RevalidateEnabled()) {
    auto pk = parked_.find(key);
    if (pk != parked_.end()) {
      ParkedTexture parked = pk->second;
      parked_.erase(pk);
      // The write that parked it unwatched its region. Watch again BEFORE
      // hashing: a store landing between the hash and the re-arm would match
      // the old hash, fault nothing, and leave the stale texture adopted
      // (Tiger Woods 14 / FIFA 16 glyph atlases lost single glyphs this way).
      // A store after the re-arm queues an invalidation that parks it again.
      if (!trace_playback_ && shared_memory_) {
        /* watch armed through the global watch above */;
      }
      const auto t0 = std::chrono::steady_clock::now();
      const uint64_t now_hash =
          HashGuestRegions(parked.guest_base, parked.guest_bytes,
                           parked.mip_base, parked.mip_bytes);
      census_revalidate_ns_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t0)
              .count());
      if (now_hash == parked.guest_hash) {
        uint32_t slot = AllocateSlot();
        if (slot) {
          WriteSlotDescriptor(slot, parked.texture);
          EmplaceEntry(key, Entry{parked.texture, slot, parked.guest_base, parked.guest_bytes,
                                  parked.mip_base, parked.mip_bytes, parked.guest_hash});
          active_textures_.push_back(parked.texture);
          ++census_revalidated_;
          return slot;
        }
      } else {
        ++census_revalidate_changed_;
      }
      retired_textures_.emplace_back(RetireSubmission(), parked.texture);
    }
  }
  // Signed twin. A shader's signed and unsigned bindings of one texture key
  // apart, and the legacy path untiled the same guest bytes once per key
  // (Rage 2026-10-15: every streamed texture twice, 19 of 39 ms/frame). Serve
  // the second binding from the first's host texture: the same pixel format
  // shares it, a unorm/snorm pair takes a format view (the shared route already
  // serves signed bindings as views). The entry carries the twin's guest range
  // and hash, so a guest write evicts both. XE_METAL_TEX_SIGNED_TWIN=0 disables.
  static const bool s_signed_twin = [] {
    const char* v = std::getenv("XE_METAL_TEX_SIGNED_TWIN");
    return !(v && v[0] == '0' && !v[1]);
  }();
  if (s_signed_twin) {
    uint32_t twin_mip_min = 0, twin_mip_max = 0;
    const uint64_t twin_key =
        MakeTextureKey(fetch, !is_signed, base, width, height, depth,
                       pitch_texels, &twin_mip_min, &twin_mip_max);
    auto twin_it = cache_.find(twin_key);
    if (twin_it != cache_.end() && twin_it->second.texture) {
      // Copy: AllocateSlot may evict entries and invalidate the iterator.
      const Entry twin = twin_it->second;
      id<MTLTexture> twin_tex = (__bridge id<MTLTexture>)twin.texture;
      id<MTLTexture> alias = twin_tex.pixelFormat == plan.host_format
                                 ? twin_tex
                                 : [twin_tex newTextureViewWithPixelFormat:
                                                 plan.host_format];
      if (alias) {
        uint32_t slot = AllocateSlot();
        if (slot) {
          void* alias_ptr = (__bridge_retained void*)alias;
          WriteSlotDescriptor(slot, alias_ptr);
          EmplaceEntry(key, Entry{alias_ptr, slot, twin.guest_base,
                                  twin.guest_bytes, twin.mip_base,
                                  twin.mip_bytes, twin.guest_hash});
          active_textures_.push_back(alias_ptr);
          ++census_signed_twin_;
          return slot;
        }
      }
    }
  }
  const uint8_t* guest =
      static_cast<const uint8_t*>(static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(base));
  if (!guest) {
    return 0;
  }
  // XE_METAL_UNTILE_TIMING=1: the first 4:4:4:4 misses with the fields the
  // shared-route eligibility reads, so a table entry that does not engage
  // can be attributed (RDR 2026-10-04).
  {
    static const bool s_untile_diag =
        std::getenv("XE_METAL_UNTILE_TIMING") != nullptr;
    // Unique by (base, level range, signedness), bounded.
    static std::set<uint64_t> s_4444_shapes;
    // XE_METAL_UNTILE_TIMING=1: the first resolve-backed (or 4:4:4:4)
    // legacy misses with every eligibility field, unique by shape.
    const bool diag_candidate =
        fetch.format == xenos::TextureFormat::k_4_4_4_4 ||
        resolved_targets_.count(base) != 0;
    if (s_untile_diag && diag_candidate && s_4444_shapes.size() < 64 &&
        s_4444_shapes
            .insert((uint64_t(base) << 16) |
                    (uint64_t(fetch.mip_min_level) << 8) |
                    (uint64_t(fetch.mip_max_level) << 4) |
                    (is_signed ? 1u : 0u))
            .second) {
      REXLOG_WARN(
          "MetalTexMiss fmt={} base={:08X} {}x{} pitch={} tiled={} dim={} "
          "stacked={} mips={}..{} mip_addr={:08X} packed={} signs={}{}{}{} "
          "endian={} signed={} resolve_backed={} elig_base={} elig_mip={}",
          uint32_t(fetch.format), base, width, height, pitch_texels,
          uint32_t(fetch.tiled), uint32_t(fetch.dimension),
          uint32_t(fetch.stacked), uint32_t(fetch.mip_min_level),
          uint32_t(fetch.mip_max_level), uint32_t(fetch.mip_address) << 12,
          uint32_t(fetch.packed_mips),
          uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
          uint32_t(fetch.sign_z), uint32_t(fetch.sign_w),
          uint32_t(fetch.endianness), is_signed ? 1 : 0,
          resolved_targets_.count(base) ? 1 : 0,
          metal::IsSharedRouteEligible(fetch, is_signed, width, height,
                                       pitch_texels)
              ? 1
              : 0,
          metal::IsSharedRouteMipEligible(fetch, is_signed, width, height,
                                          pitch_texels)
              ? 1
              : 0);
    }
  }
  // Diagnostic scope timer for the whole miss path (untile + upload +
  // finalize); see ConsumeUntileTimingReport.
  struct UntileTimingScope {
    MetalTextureCache* cache;
    uint32_t key;
    uint64_t bytes;
    std::chrono::steady_clock::time_point t0;
    ~UntileTimingScope() {
      const uint64_t ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t0)
              .count());
      auto& b = cache->untile_timing_[key];
      b.ns += ns;
      b.count += 1;
      b.bytes += bytes;
      cache->untile_total_ns_ += ns;
      cache->untile_total_count_ += 1;
    }
  };
  static const bool s_untile_timing =
      std::getenv("XE_METAL_UNTILE_TIMING") != nullptr;
  std::optional<UntileTimingScope> untile_timing_scope;
  if (s_untile_timing) {
    const bool resolve_backed =
        resolved_targets_.find(base) != resolved_targets_.end();
    const uint32_t timing_key = (uint32_t(fetch.format) << 2) |
                                (is_signed ? 2u : 0u) |
                                (resolve_backed ? 1u : 0u);
    untile_timing_scope.emplace(UntileTimingScope{
        this, timing_key, 0, std::chrono::steady_clock::now()});
    // Why is this fetch on the legacy path? Bounded per key: the fields the
    // shared-route eligibility predicates look at.
    static std::unordered_map<uint32_t, uint32_t> s_miss_logs;
    if (s_miss_logs[timing_key]++ < 2) {
      REXLOG_INFO(
          "MetalUntileMiss fmt={} signed={} resolve_backed={} base={:08X} "
          "{}x{} pitch={} tiled={} dim={} stacked={} mips={}-{} mip_addr={:08X} "
          "signs={}{}{}{} endian={}",
          uint32_t(fetch.format), is_signed ? 1 : 0, resolve_backed ? 1 : 0,
          base, fetch.size_2d.width + 1, fetch.size_2d.height + 1,
          uint32_t(fetch.pitch), uint32_t(fetch.tiled),
          uint32_t(fetch.dimension), uint32_t(fetch.stacked),
          fetch.mip_min_level, fetch.mip_max_level, fetch.mip_address << 12,
          uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
          uint32_t(fetch.sign_z), uint32_t(fetch.sign_w),
          uint32_t(fetch.endianness));
    }
  }

  bool tiled = fetch.tiled != 0;
  xenos::Endian endian = fetch.endianness;
  // Calibration override (XE_METAL_HISTORY_ENDIAN=0..3): endian applied when
  // untiling fetches of RESOLVE-WRITTEN bases — hardware's resolve write
  // order vs artist-texture order is being determined empirically.
  static const int s_history_endian = [] {
    const char* v = std::getenv("XE_METAL_HISTORY_ENDIAN");
    return v ? atoi(v) : -1;
  }();
  if (s_history_endian >= 0 &&
      resolved_targets_.find(base) != resolved_targets_.end()) {
    endian = xenos::Endian(s_history_endian & 3);
  }
  uint32_t max_src_bytes = kPhysicalSize - base;

  // Block grid (1x1 for uncompressed). The tiled addresser works in block
  // coordinates; the pitch it wants is in blocks, 32-block aligned.
  uint32_t block_texels = plan.block_texels;
  uint32_t bpb = plan.bytes_per_block;
  uint32_t blocks_w = (width + block_texels - 1) / block_texels;
  uint32_t blocks_h = (height + block_texels - 1) / block_texels;
  uint32_t pitch_blocks = pitch_texels / block_texels;
  uint32_t pitch_blocks_aligned = (pitch_blocks + 31u) & ~31u;

  // Packed base level: the base's blocks start at (base_xb, base_yb) inside
  // the base tail, not at (0, 0) — see metal_packed_base.h. Every base read
  // below (2D and cube faces, tiled and linear) and the watched/recorded
  // base extent use the same offsets.
  const MetalPackedBaseOffset packed_base = ComputeMetalPackedBaseOffset(
      fetch.packed_mips != 0, fetch.dimension, width, height, fetch.format);
  const uint32_t base_xb = packed_base.x_blocks;
  const uint32_t base_yb = packed_base.y_blocks;
  // Rows of blocks the base read actually touches (offset rows included).
  const uint32_t blocks_h_read = blocks_h + base_yb;

  uint32_t blocks_h_aligned = (blocks_h_read + 31u) & ~31u;
  // 3D: the guest layout's per-slice stride (32-aligned block rows; the
  // tiled addresser's height_aligned) and the base level's full extent —
  // the same rules the shared layout computes for every backend.
  uint32_t z_layer_stride_block_rows = blocks_h_aligned;
  uint32_t z_slice_stride_bytes_linear = 0;
  uint32_t array_layer_stride_bytes = 0;
  uint32_t volume_extent_bytes = 0;
  if (is_3d || depth > 1) {
    ::rex::xenos::GuestTextureLayout layout_v =
        texture_util::GetGuestTextureLayout(
            fetch.dimension, fetch.pitch, width, height, depth, tiled,
            fetch.format, fetch.packed_mips != 0, /*has_base=*/true,
            normalized_mip_max);
    z_layer_stride_block_rows = layout_v.base.z_layer_stride_block_rows;
    z_slice_stride_bytes_linear =
        layout_v.base.row_pitch_bytes * z_layer_stride_block_rows;
    array_layer_stride_bytes = layout_v.base.array_layer_stride_bytes;
    volume_extent_bytes = layout_v.base.level_data_extent_bytes;
  }
  // Non-cube extent shared with the routed serve's trace records
  // (metal_texture_load_eligibility.h) — recorded ranges must be identical
  // by construction across producers.
  uint32_t watched_base_bytes =
      is_cube ? uint32_t(std::min<size_t>(
                    size_t(pitch_blocks_aligned) * blocks_h_aligned * bpb * 6,
                    size_t(kPhysicalSize - base)))
      : (is_3d || depth > 1)
          ? uint32_t(std::min<size_t>(volume_extent_bytes,
                                      size_t(kPhysicalSize - base)))
              : ComputeWatchedBaseBytes(pitch_blocks_aligned, blocks_h_read,
                                        bpb, base, kPhysicalSize);
  // WATCH-ONLY admission for the pages this untile is about to read directly.
  // This cache still does NOT call ::rex::gpu::SharedMemory::RequestRange: that path
  // uploads into the shared buffer and marks pages valid, which this cache
  // never reads, and its whole-range rejection of extents containing unmapped
  // pages dropped entire textures to the null slot (measured: 300 null serves
  // of one 1280x720 base in a live SF4 session). RequestWatchOnlyRange instead
  // enables CPU-write notifications for the admissible mapped pages only,
  // uploading nothing and validating nothing, so an in-place guest rewrite
  // (FMV/video decoders reusing one buffer, font atlases) reaches
  // QueueInvalidation/InvalidateRange and the next fetch re-untiles fresh
  // bytes. Without it those rewrites were served stale forever (measured: 418
  // stale hits in one live SF4 session; corrupted menu glyphs).
  //
  // Admission is advisory BY CONTRACT: a partially or wholly unadmitted extent
  // is not an error and never changes what this fetch serves — the direct-RAM
  // untile below already has the bytes. Only watching is lost, which is the
  // pre-existing behavior.
  if (!trace_playback_ && shared_memory_) {
    /* watched through the global watch; the range is informational */;
  }
  // Revalidation hash of the bytes this upload is about to read, taken after
  // the watch is armed and BEFORE the untile. Hashing after the untile let a
  // CPU store that faulted mid-untile land in the hash but not the texture:
  // the queued invalidation parked the entry, the re-hash matched, and the
  // stale texture was re-adopted for good.
  bool base_hash_ok = false;
  const uint64_t base_hash =
      RevalidateEnabled()
          ? HashGuestBase(base, watched_base_bytes, &base_hash_ok)
          : 0;

  // Untile / copy into a linear block-major staging buffer (host byte order).
  // Rows are independent — run them concurrently: stage-3 re-uploads big
  // resolve dests every frame (SF4: 1280x720 each frame on the GPU thread),
  // and the single-threaded per-block loop was a measurable slice of the
  // live frame.
  // Slice-major staging: slice z occupies [z * blocks_h * blocks_w * bpb, ...)
  // (Metal's bytesPerImage for the 3D upload); depth is 1 for everything
  // but 3D, so 2D and cube keep their exact bytes.
  const size_t slice_bytes = size_t(blocks_w) * blocks_h * bpb;
  std::vector<uint8_t> linear(slice_bytes * depth);
  auto untile_row = [&](size_t row) {
    const uint32_t z = uint32_t(row / blocks_h);
    const uint32_t by = uint32_t(row % blocks_h);
    for (uint32_t bx = 0; bx < blocks_w; ++bx) {
      uint32_t src_off;
      if (is_3d) {
        if (tiled) {
          src_off = uint32_t(texture_address::Tiled3D(
              int32_t(bx + base_xb), int32_t(by + base_yb), int32_t(z),
              pitch_blocks_aligned, z_layer_stride_block_rows,
              plan.block_log2));
        } else {
          src_off = z * z_slice_stride_bytes_linear +
                    uint32_t((by + base_yb) * pitch_blocks + bx + base_xb) *
                        bpb;
        }
      } else if (tiled) {
        src_off = z * array_layer_stride_bytes +
                  uint32_t(texture_address::Tiled2D(
                      int32_t(bx + base_xb), int32_t(by + base_yb),
                      pitch_blocks_aligned, plan.block_log2));
      } else {
        src_off = z * array_layer_stride_bytes +
                  uint32_t((by + base_yb) * pitch_blocks + bx + base_xb) * bpb;
      }
      uint8_t* dst = &linear[(row * blocks_w + bx) * bpb];
      if (src_off + bpb <= max_src_bytes) {
        std::memcpy(dst, guest + src_off, bpb);
        SwapEndian(dst, bpb, endian);
      }
    }
  };
  const size_t total_rows = size_t(blocks_h) * depth;
  if (linear.size() >= 512 * 1024) {
    dispatch_apply(total_rows, DISPATCH_APPLY_AUTO,
                   ^(size_t row) { untile_row(row); });
  } else {
    for (size_t row = 0; row < total_rows; ++row) {
      untile_row(row);
    }
  }

  // Data + row stride handed to Metal. k_4_4_4_4 is CPU-expanded here from its
  // 16bpp linear form to RGBA8 (each 4-bit channel -> 8 bits via *17), in the
  // guest's channel order (ascending nibble -> ascending byte), so the same
  // fetch-swizzle-view path as k_8_8_8_8 makes channels correct.
  const uint8_t* upload_data = linear.data();
  uint32_t upload_bytes_per_row = blocks_w * bpb;
  // Calibration (XE_METAL_DUMP_FETCH=<hex base>): print center texels of the
  // untiled+endian-swapped upload for that base — channel-permutation ground
  // truth against a reference render of the same content.
  static const uint32_t s_dump_fetch = [] {
    const char* v = std::getenv("XE_METAL_DUMP_FETCH");
    return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
  }();
  if (s_dump_fetch && base == s_dump_fetch) {
    if (bpb == 4) {
      auto texel = [&](uint32_t x, uint32_t y) {
        const uint8_t* t = &linear[(size_t(y) * blocks_w + x) * 4];
        return fmt::format("({},{},{},{})", t[0], t[1], t[2], t[3]);
      };
      REXLOG_WARN("MetalFetchDump {:08X} fmt={} {}x{} swz={:03X}: c={} q1={} q3={}",
             base, uint32_t(fetch.format), width, height, fetch.swizzle & 0xFFF,
             texel(width / 2, height / 2), texel(width / 4, height / 4),
             texel(3 * width / 4, 3 * height / 4));
    } else if (bpb == 2) {
      // 16bpp (k_16 LUTs etc.): print a ramp of raw 16-bit values.
      std::string vals;
      for (uint32_t x = 0; x < width; x += std::max(1u, width / 16)) {
        const uint8_t* t = &linear[size_t(x) * 2];
        vals += fmt::format(" {:04X}", uint32_t(t[0]) | (uint32_t(t[1]) << 8));
      }
      REXLOG_WARN("MetalFetchDump {:08X} fmt={} {}x{} swz={:03X} endian={}:{}",
             base, uint32_t(fetch.format), width, height, fetch.swizzle & 0xFFF,
             uint32_t(endian), vals);
      static uint32_t s_n = 0;
      std::string path = fmt::format("/tmp/xe_fetch_{:08X}_{}.bin", base, s_n++);
      if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(linear.data(), 1, size_t(blocks_w) * blocks_h * bpb, f);
        fclose(f);
        REXLOG_WARN("MetalFetchDump wrote {} rows={} row_bytes={}", path, blocks_h, blocks_w * bpb);
      }
    }
  }
  // XE_METAL_DUMP_FETCH: also compare guest RAM with the shared buffer over
  // the fetched region (RAM is what this legacy path untiles; the buffer is
  // what the GPU load route untiles) -- identical bytes attribute a routed
  // divergence to interpretation, different bytes to content/ordering.
  if (s_dump_fetch && base == s_dump_fetch && shared_memory_) {
    auto* msm = static_cast<MetalSharedMemory*>(shared_memory_);
    id<MTLBuffer> buf = (__bridge id<MTLBuffer>)msm->GetBuffer();
    const uint8_t* ram = static_cast<const uint8_t*>(static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(base));
    const uint8_t* gpu = buf ? static_cast<const uint8_t*>([buf contents]) + base : nullptr;
    const size_t bytes = size_t(blocks_h) * pitch_texels * bpb;
    if (ram && gpu) {
      size_t diff = 0, first = SIZE_MAX;
      for (size_t i = 0; i < bytes; ++i) {
        if (ram[i] != gpu[i]) { if (first == SIZE_MAX) first = i; ++diff; }
      }
      REXLOG_WARN("MetalFetchDump RAM-vs-BUFFER {:08X} bytes={} differing={} first=+{:X} ram={:02X}{:02X} buf={:02X}{:02X}",
             base, bytes, diff, first == SIZE_MAX ? 0 : first,
             first == SIZE_MAX ? 0 : ram[first], first == SIZE_MAX ? 0 : ram[first + 1],
             first == SIZE_MAX ? 0 : gpu[first], first == SIZE_MAX ? 0 : gpu[first + 1]);
    }
  }
  // XE_METAL_DUMP_FETCH_PNG=<hex base>: dump the whole untiled+swapped upload
  // as /tmp/xe_fetch_<base>.ppm (bytes 0,1,2 as RGB). Run once reader-only
  // (trace-restored RAM = hardware content) and once with stage-3 writebacks
  // (our content) — the visual diff attributes divergence to the producer.
  static const uint32_t s_dump_fetch_png = [] {
    const char* v = std::getenv("XE_METAL_DUMP_FETCH_PNG");
    return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
  }();
  if (s_dump_fetch_png && base == s_dump_fetch_png && bpb == 4) {
    // Named by the lineage draw stamp (set when XE_METAL_LINEAGE is active)
    // so successive fetches of the same base in one frame do not clobber.
    std::string path =
        fmt::format("/tmp/xe_fetch_{:08X}_d{:06}.ppm", base, lineage_draw_);
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
      fprintf(f, "P6\n%u %u\n255\n", blocks_w, blocks_h);
      for (size_t i = 0; i < size_t(blocks_w) * blocks_h; ++i) {
        fwrite(&linear[i * 4], 1, 3, f);
      }
      fclose(f);
      REXLOG_WARN("MetalFetchPNG: wrote {} ({}x{})", path, blocks_w, blocks_h);
    }
  }
  // XE_METAL_DUMP_FETCH_RAW=<hex base>[,<hex base>...]: write the
  // untiled+endian-swapped upload BLOCK bytes (any format, compressed
  // included) to /tmp/xe_fetch_<base>.bin for offline decode — the
  // DUMP_FETCH_PNG lever above is 32bpp-only. Sonic black-sky task 3.2.
  static const char* s_dump_fetch_raw = std::getenv("XE_METAL_DUMP_FETCH_RAW");
  if (s_dump_fetch_raw) {
    bool wanted = false;
    for (const char* p = s_dump_fetch_raw; *p;) {
      char* end;
      uint32_t v = uint32_t(strtoul(p, &end, 16));
      if (end == p) break;
      if (v == base) { wanted = true; break; }
      p = (*end == ',') ? end + 1 : end;
    }
    if (wanted) {
      std::string path = fmt::format("/tmp/xe_fetch_{:08X}_d{:06}.bin", base,
                                     lineage_draw_);
      FILE* f = fopen(path.c_str(), "wb");
      if (f) {
        fwrite(linear.data(), 1, linear.size(), f);
        fclose(f);
        REXLOG_WARN(
            "MetalFetchRaw: wrote {} ({} bytes, blocks {}x{}, bpb={}, fmt={})",
            path, linear.size(), blocks_w, blocks_h, bpb,
            uint32_t(fetch.format));
      }
    }
  }
  // The 16bpp CPU expansions (4:4:4:4, 5:6:5, k_16) to RGBA8, from one
  // untiled level/face in `src` (blocks_w texels per row) into `out`.
  const bool plan_decode16 =
      plan.decode_4444 || plan.decode_565 || plan.decode_1555 ||
      plan.decode_r16;
  auto decode16 = [&](const uint8_t* src, std::vector<uint8_t>& out) {
    out.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        size_t si = (size_t(y) * blocks_w + x) * bpb;
        uint16_t v = uint16_t(src[si] | (src[si + 1] << 8));
        uint8_t* o = &out[(size_t(y) * width + x) * 4];
        if (plan.decode_r16) {
          uint8_t r = uint8_t(v >> 8);  // high byte, unorm
          o[0] = o[1] = o[2] = r;
          o[3] = 255;
        } else if (plan.decode_565) {
          // R5 (bits 0-4) G6 (5-10) B5 (11-15), rounded to the nearest
          // 8-bit unorm, as the native format's float conversion would be.
          o[0] = uint8_t((((v >> 0) & 0x1F) * 255 + 15) / 31);
          o[1] = uint8_t((((v >> 5) & 0x3F) * 255 + 31) / 63);
          o[2] = uint8_t((((v >> 11) & 0x1F) * 255 + 15) / 31);
          o[3] = 255;
        } else if (plan.decode_1555) {
          // R5 (bits 0-4) G5 (5-9) B5 (10-14) A1 (15), as the GPU expansion
          // (XeR5G5B5A1ToR8G8B8A8) rounds them.
          o[0] = uint8_t((((v >> 0) & 0x1F) * 255 + 15) / 31);
          o[1] = uint8_t((((v >> 5) & 0x1F) * 255 + 15) / 31);
          o[2] = uint8_t((((v >> 10) & 0x1F) * 255 + 15) / 31);
          o[3] = (v & 0x8000) ? 255 : 0;
        } else {  // decode_4444
          o[0] = uint8_t(((v >> 0) & 0xF) * 17);
          o[1] = uint8_t(((v >> 4) & 0xF) * 17);
          o[2] = uint8_t(((v >> 8) & 0xF) * 17);
          o[3] = uint8_t(((v >> 12) & 0xF) * 17);
        }
      }
    }
  };
  std::vector<uint8_t> decoded;
  if (plan.decode_depth_unorm || plan.decode_depth_float) {
    // k_24_8 / k_24_8_FLOAT: one float per texel, the shared load-shader rule.
    decoded.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        size_t si = (size_t(y) * blocks_w + x) * bpb;
        uint32_t word;
        std::memcpy(&word, &linear[si], sizeof(word));
        float f = plan.decode_depth_unorm ? DecodeDepthUnormTexel(word)
                                          : DecodeDepthFloatTexel(word);
        std::memcpy(&decoded[(size_t(y) * width + x) * 4], &f, sizeof(f));
      }
    }
    upload_data = decoded.data();
    upload_bytes_per_row = width * 4;
  } else if (plan_decode16) {
    decode16(linear.data(), decoded);
    upload_data = decoded.data();
    upload_bytes_per_row = width * 4;
  } else if (plan.decode_ctx1) {
    // Mirrors texture_load_ctx1.cs.xesl / XeCTX1TwoBlocksRowToR8G8: after the
    // fetch-endian swap, block word 0 holds the endpoints as bytes
    // [g0, r0, g1, r1] and word 1 the 2-bit codes (texel (row r, col c) at
    // bit 8*r + 2*c). Code -> weight of endpoint 1: 0->0, 1->3, 2->1, 3->2;
    // endpoint 0 takes the remaining thirds. Result = sum / 3.
    decoded.resize(size_t(width) * height * 2);
    for (uint32_t by = 0; by < blocks_h; ++by) {
      for (uint32_t bx = 0; bx < blocks_w; ++bx) {
        uint8_t texels[16 * 2];
        DecodeCtx1Block(&linear[(size_t(by) * blocks_w + bx) * 8], texels);
        for (uint32_t r = 0; r < 4 && by * 4 + r < height; ++r) {
          for (uint32_t c = 0; c < 4 && bx * 4 + c < width; ++c) {
            std::memcpy(&decoded[(size_t(by * 4 + r) * width + bx * 4 + c) * 2],
                        &texels[(r * 4 + c) * 2], 2);
          }
        }
      }
    }
    upload_data = decoded.data();
    upload_bytes_per_row = width * 2;
  } else if (plan.decode_dxt3a) {
    // After the fetch-endian swap, block word 0 holds rows 0-1 and word 1
    // rows 2-3; a row is 16 bits with texel c's 4-bit alpha at bit 4*c.
    // Expanded to 8 bits as a*17 (XeDXT3FourBlocksRowToA8).
    decoded.resize(size_t(width) * height);
    for (uint32_t by = 0; by < blocks_h; ++by) {
      for (uint32_t bx = 0; bx < blocks_w; ++bx) {
        uint8_t texels[16];
        DecodeDxt3aBlock(&linear[(size_t(by) * blocks_w + bx) * 8], texels);
        for (uint32_t r = 0; r < 4 && by * 4 + r < height; ++r) {
          for (uint32_t c = 0; c < 4 && bx * 4 + c < width; ++c) {
            decoded[size_t(by * 4 + r) * width + bx * 4 + c] = texels[r * 4 + c];
          }
        }
      }
    }
    upload_data = decoded.data();
    upload_bytes_per_row = width;
  }

  // XE_METAL_DUMP_FETCH: the CPU-decoded upload (CTX1, DXT3A, depth, 16bpp
  // expansions) to /tmp/xe_fetchdec_<base>_<n>.bin, the byte oracle for the
  // GPU route's XE_METAL_DUMP_ROUTE level dumps.
  if (s_dump_fetch && base == s_dump_fetch && !decoded.empty()) {
    static uint32_t s_dec_n = 0;
    std::string path =
        fmt::format("/tmp/xe_fetchdec_{:08X}_{}.bin", base, s_dec_n++);
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(decoded.data(), 1, decoded.size(), f);
      fclose(f);
      REXLOG_WARN("MetalFetchDump wrote {} {}x{} row_bytes={}", path, width, height,
             upload_bytes_per_row);
    }
  }
  // Bring-up diagnostic: a texture that decodes to all-zero bytes renders pure
  // black. The most likely source is a render-target-backed surface (e.g. the
  // resolved scene) whose guest memory we never populated because IssueCopy is
  // still a stub. Flag it so we can tell "black texture" from a shader bug.
  static uint32_t s_tex_log = 0;
  bool all_zero = true;
  for (uint8_t b : linear) {
    if (b) {
      all_zero = false;
      break;
    }
  }
  {
    const uint32_t census_index =
        (uint32_t(fetch.dimension) << 2) | (is_stacked ? 1u : 0u) |
        (fetch.mip_max_level > fetch.mip_min_level ? 2u : 0u);
    ++dim_census_[census_index & 15u];
    if (is_signed) {
      ++dim_census_signed_[census_index & 15u];
    }
  }
  if (s_tex_log < 24) {
    ++s_tex_log;
    REXLOG_INFO(
        "MetalTex: fmt={} {}x{}x{} base={:08X} pitch_t={} tiled={} endian={} "
        "swz={:03X} slot={} {}",
        uint32_t(fetch.format), width, height, depth, base, pitch_texels, tiled,
        uint32_t(endian), fetch.swizzle & 0xFFF, next_slot_,
        all_zero ? "ALL-ZERO(black)" : "hasdata");
  }

  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  // MUST be a 2D ARRAY texture: our DXBC translator emits texture2d_array reads
  // and MSC is compiled with IRCompatibilityFlagForceTextureArray, so a plain
  // 2D texture sampled through an array read returns garbage (flat sampling).
  // Cubes likewise must be CUBE ARRAYS (texturecube_array in the MSL).
  const bool plan_cpu_decoded = plan.decode_4444 || plan.decode_565 ||
                                plan.decode_1555 ||
                                plan.decode_r16 ||
                                plan.decode_depth_unorm ||
                                plan.decode_depth_float || plan.decode_ctx1 ||
                                plan.decode_dxt3a;
  // The 16bpp expansions decode each cube face below; the other CPU decodes
  // have no cube consumer and stay refused.
  if (is_cube && (width != height || (plan_cpu_decoded && !plan_decode16))) {
    log_null("cube-shape");
    return 0;
  }
  // Mip chain: mips live at fetch.mip_address with their own layout
  // (pow2-rounded, 32-block-aligned, small levels packed into a tail —
  // texture_util::GetGuestTextureLayout). Serving mip 0 only point/linear
  // aliases every minified texture (JC2 cloth moire). 2D only for now.
  // The page comes from the same normalization the key used: when level 0 is
  // already the packed tail, D3D leaves mip_address at 0 and the mips share
  // the tail at the base address (texture_util, canary ace153cb8).
  uint32_t mip_page = 0;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, nullptr, nullptr, nullptr, nullptr, &mip_page, nullptr, nullptr);
  uint32_t mip_addr = mip_page << 12;
  uint32_t host_levels = 1;
  // A/B: XE_METAL_NO_MIPS=1 disables mip upload; XE_METAL_NO_PACKED_MIPS=1
  // uploads only the levels before the packed tail.
  static const bool s_no_mips = std::getenv("XE_METAL_NO_MIPS") != nullptr;
  static const bool s_no_packed_mips =
      std::getenv("XE_METAL_NO_PACKED_MIPS") != nullptr;
  static const bool s_mips_dxt_only =
      std::getenv("XE_METAL_MIPS_DXT_ONLY") != nullptr;
  static const bool s_mips_uncomp_only =
      std::getenv("XE_METAL_MIPS_UNCOMP_ONLY") != nullptr;
  bool mip_format_allowed = !(s_mips_dxt_only && !plan.compressed) &&
                            !(s_mips_uncomp_only && plan.compressed);
  // Cube maps: every level holds six faces at the level's array-slice stride
  // (texture_util layout with array size 6, the same walk D3D12 uses). Found
  // engaged by the 2026-10-03 dimension census (Batman Arkham Asylum 872,
  // Skate 2 28 mipped cube uploads); level-0-only serving sharpened every
  // roughness-driven reflection. XE_METAL_CUBE_MIPS=0 restores level 0 only.
  static const bool s_cube_mips = [] {
    const char* v = std::getenv("XE_METAL_CUBE_MIPS");
    return !(v && v[0] == '0' && !v[1]);
  }();
  if (plan_cpu_decoded && mip_addr && normalized_mip_max > 0) {
    ++mips_dropped_census_[uint32_t(fetch.format) & 63];
  }
  if (mip_addr && normalized_mip_max > 0) {
    if (is_3d) {
      ++census_3d_mips_dropped_;
    } else if (depth > 1) {
      ++census_stacked_mips_dropped_;
    }
  }
  if (!s_no_mips && mip_format_allowed && (!is_cube || s_cube_mips) &&
      !is_3d && depth == 1 && mip_addr &&
      mip_addr < kPhysicalSize && normalized_mip_max > 0 &&
      !plan_cpu_decoded) {
    // Same normalized value the key was built from — see the normalization
    // above. The size-derived clamp is retained; normalization already applies
    // an equivalent bound, so this is belt-and-braces rather than a second
    // opinion.
    host_levels = std::min<uint32_t>(
        normalized_mip_max + 1,
        rex::log2_floor(std::max(width, height)) + 1);
    if (s_no_packed_mips && fetch.packed_mips) {
      host_levels = std::min(
          host_levels, std::max(1u, texture_util::GetPackedMipLevel(width, height)));
    }
    // Capture-completeness guard, TRACE PLAYBACK ONLY: single-frame captures
    // never recorded mip memory, so the restored pages are ZEROS and uploading
    // them would black out every minified surface in a replay. If the first
    // 4KB at mip_address is all-zero, serve the base level only.
    //
    // Live, guest RAM is authoritative (2026-09-18): a black mip tail is data
    // — an explicit-LOD or minified fetch of it must read black, as it does on
    // hardware and on D3D12/Vulkan, which upload whatever is there. The
    // content test must not change valid texture semantics, so it is confined
    // to playback. XE_METAL_MIP_ZERO_GUARD_LIVE=1 restores the previous
    // content-based demotion everywhere (exact rollback).
    static const bool s_zero_guard_live =
        std::getenv("XE_METAL_MIP_ZERO_GUARD_LIVE") != nullptr;
    const uint32_t* probe = reinterpret_cast<const uint32_t*>(
        static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(mip_addr));
    if (!probe) {
      host_levels = 1;
    } else if (trace_playback_ || s_zero_guard_live) {
      uint32_t probe_words =
          std::min<uint32_t>(1024, (kPhysicalSize - mip_addr) / 4);
      bool any_nonzero = false;
      for (uint32_t i = 0; i < probe_words; ++i) {
        if (probe[i]) {
          any_nonzero = true;
          break;
        }
      }
      if (!any_nonzero) {
        host_levels = 1;
      }
    } else {
      // Live census (bounded): how often the old guard WOULD have demoted a
      // texture whose mip tail starts with 4KB of zeros.
      static uint32_t s_zero_tail_log = 0;
      if (s_zero_tail_log < 64) {
        uint32_t probe_words =
            std::min<uint32_t>(1024, (kPhysicalSize - mip_addr) / 4);
        bool any_nonzero = false;
        for (uint32_t i = 0; i < probe_words; ++i) {
          if (probe[i]) {
            any_nonzero = true;
            break;
          }
        }
        if (!any_nonzero) {
          ++s_zero_tail_log;
          REXLOG_INFO(
              "MetalTexMipZeroTail: fmt={} {}x{} base={:08X} mip={:08X} "
              "levels={} kept (live RAM authoritative; old guard would serve "
              "base only)",
              uint32_t(fetch.format), width, height, base, mip_addr,
              host_levels);
        }
      }
    }
  }
  MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
  desc.textureType = is_3d     ? MTLTextureType3D
                     : is_cube ? MTLTextureTypeCubeArray
                               : MTLTextureType2DArray;
  desc.pixelFormat = plan.host_format;
  desc.width = width;
  desc.height = height;
  desc.depth = is_3d ? depth : 1;
  // For cube arrays this is the CUBE count (6 slices); stacked textures are
  // an array of their layers.
  desc.arrayLength = (!is_3d && !is_cube) ? depth : 1;
  desc.mipmapLevelCount = host_levels;
  desc.usage = MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  if (!texture) {
    binding_allocation_failed_ = true;
    return 0;
  }
  // For BC formats bytesPerRow is a row of blocks; MTLRegion is still in texels
  // and Metal handles partial edge blocks. Slice 0 of the single array layer;
  // a 3D texture takes the whole slice-major volume in one region.
  if (is_3d) {
    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, width, height, depth)
               mipmapLevel:0
                     slice:0
                 withBytes:upload_data
               bytesPerRow:upload_bytes_per_row
             bytesPerImage:size_t(upload_bytes_per_row) * blocks_h];
  } else {
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                     slice:0
                 withBytes:upload_data
               bytesPerRow:upload_bytes_per_row
             bytesPerImage:0];
    // Stacked layers 1..depth-1 (slice-major in the staging buffer; the
    // decoded plans are excluded above, so upload_data is `linear`).
    for (uint32_t z = 1; z < depth; ++z) {
      [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                 mipmapLevel:0
                       slice:z
                   withBytes:linear.data() + size_t(z) * slice_bytes
                 bytesPerRow:upload_bytes_per_row
               bytesPerImage:0];
    }
  }
  // Cube faces 1..5: guest layout is face-major — each face is a full tiled
  // (or linear) slice whose block grid is padded to 32x32 blocks
  // (texture_extent.cc guest rules); re-untile each face at its offset.
  uint32_t face_stride = 0;
  if (is_cube) {
    face_stride = pitch_blocks_aligned * blocks_h_aligned * bpb;
    for (uint32_t face = 1; face < 6; ++face) {
      uint32_t face_base = face * face_stride;
      for (uint32_t by = 0; by < blocks_h; ++by) {
        for (uint32_t bx = 0; bx < blocks_w; ++bx) {
          uint32_t src_off;
          if (tiled) {
            src_off = face_base + uint32_t(texture_address::Tiled2D(
                                      int32_t(bx + base_xb),
                                      int32_t(by + base_yb),
                                      pitch_blocks_aligned, plan.block_log2));
          } else {
            src_off = face_base +
                      ((by + base_yb) * pitch_blocks + bx + base_xb) * bpb;
          }
          uint8_t* dst = &linear[(size_t(by) * blocks_w + bx) * bpb];
          if (src_off + bpb <= max_src_bytes) {
            std::memcpy(dst, guest + src_off, bpb);
            SwapEndian(dst, bpb, endian);
          }
        }
      }
      const uint8_t* face_data = linear.data();
      if (plan_decode16) {
        decode16(linear.data(), decoded);
        face_data = decoded.data();
      }
      [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                 mipmapLevel:0
                       slice:face
                   withBytes:face_data
                 bytesPerRow:upload_bytes_per_row
               bytesPerImage:0];
    }
  }
  uint32_t entry_mip_bytes = 0;
  if (host_levels > 1) {
    ::rex::xenos::GuestTextureLayout layout = texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, width, height, is_cube ? 6u : 1u, tiled,
        fetch.format, fetch.packed_mips != 0, /*has_base=*/true,
        normalized_mip_max);
    entry_mip_bytes = std::min<uint32_t>(layout.mips_total_extent_bytes,
                                         kPhysicalSize - mip_addr);
    pending_guest_hash_ =
        base_hash_ok ? HashGuestMips(base_hash, mip_addr, entry_mip_bytes) : 0;
    // Mips live at mip_address, NOT relative to the base level — read through
    // their own translation with their own bounds.
    const uint8_t* mip_guest =
        static_cast<const uint8_t*>(static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(mip_addr));
    uint32_t max_mip_bytes = kPhysicalSize - mip_addr;
    std::vector<uint8_t> mip_linear;
    for (uint32_t level = 1; mip_guest && level < host_levels; ++level) {
      uint32_t lw = std::max(1u, width >> level);
      uint32_t lh = std::max(1u, height >> level);
      uint32_t lbw = (lw + block_texels - 1) / block_texels;
      uint32_t lbh = (lh + block_texels - 1) / block_texels;
      uint32_t storage_level = level;
      uint32_t xb = 0, yb = 0, zb = 0;
      if (layout.packed_level != UINT32_MAX && level >= layout.packed_level) {
        storage_level = layout.packed_level;
        ::rex::xenos::GetPackedMipOffset(width, height, 1, fetch.format, level,
                                         xb, yb, zb);
      }
      if (storage_level >= xenos::kTextureMaxMips) {
        break;
      }
      const auto& lvl = layout.mips[storage_level];
      if (!lvl.row_pitch_bytes) {
        break;
      }
      const uint32_t level_faces = is_cube ? 6u : 1u;
      for (uint32_t face = 0; face < level_faces; ++face) {
      uint32_t level_base = layout.mip_offsets_bytes[storage_level] +
                            face * lvl.array_layer_stride_bytes;
      uint32_t lpitch_blocks = lvl.row_pitch_bytes / bpb;
      mip_linear.assign(size_t(lbw) * lbh * bpb, 0);
      for (uint32_t by = 0; by < lbh; ++by) {
        for (uint32_t bx = 0; bx < lbw; ++bx) {
          uint32_t src_off;
          if (tiled) {
            src_off = level_base +
                      uint32_t(texture_address::Tiled2D(
                          int32_t(bx + xb), int32_t(by + yb), lpitch_blocks,
                          plan.block_log2));
          } else {
            src_off = level_base + (by + yb) * lvl.row_pitch_bytes +
                      (bx + xb) * bpb;
          }
          uint8_t* dst = &mip_linear[(size_t(by) * lbw + bx) * bpb];
          if (src_off + bpb <= max_mip_bytes) {
            std::memcpy(dst, mip_guest + src_off, bpb);
            SwapEndian(dst, bpb, endian);
          }
        }
      }
      // XE_METAL_DUMP_FETCH_RAW also dumps every uploaded mip level of the
      // watched base as /tmp/xe_fetch_<base>_L<level>.bin (block bytes),
      // with the layout the level was read through.
      if (s_dump_fetch_raw && std::strstr(s_dump_fetch_raw,
                                          fmt::format("{:08X}", base).c_str())) {
        std::string lpath = fmt::format("/tmp/xe_fetch_{:08X}_L{}.bin", base, level);
        FILE* lf = fopen(lpath.c_str(), "wb");
        if (lf) {
          fwrite(mip_linear.data(), 1, mip_linear.size(), lf);
          fclose(lf);
          REXLOG_WARN("MetalFetchRaw: level {} -> {} ({} bytes, blocks {}x{}, "
                 "storage_level={} level_base={:#x} pitch_blocks={} xb={} yb={} "
                 "mip_addr={:08X} host_levels={} packed_level={})",
                 level, lpath, mip_linear.size(), lbw, lbh, storage_level,
                 level_base, lpitch_blocks, xb, yb, mip_addr, host_levels,
                 layout.packed_level);
        }
      }
      // XE_METAL_MIP_TRACE=1: sample the untiled level's bytes (mip
      // corruption forensics — plausible-DXT vs noise vs zeros).
      static const bool s_mip_trace = std::getenv("XE_METAL_MIP_TRACE");
      static uint32_t s_mip_trace_count = 0;
      if (s_mip_trace && level == 1 && s_mip_trace_count < 8) {
        ++s_mip_trace_count;
        const uint8_t* l0 = linear.data();
        const uint8_t* l1 = mip_linear.data();
        size_t mid1 = (mip_linear.size() / 2) & ~15ull;
        // First nonzero byte offset in the raw guest mip region — reveals
        // where the data actually lives vs where the layout math looks.
        uint32_t first_nz = 0xFFFFFFFFu;
        uint32_t scan_len = std::min<uint32_t>(0x100000, kPhysicalSize - mip_addr);
        const uint8_t* raw = mip_guest;
        for (uint32_t i = 0; i < scan_len; ++i) {
          if (raw[i]) {
            first_nz = i;
            break;
          }
        }
        REXLOG_WARN("MetalMipTrace   first_nonzero at mip_addr+{:#x}", first_nz);
        REXLOG_WARN(
            "MetalMipTrace base={:08X} fmt={} {}x{} mip_addr={:08X} "
            "packed_lvl={} off1={} pitch1={} L0[0..8]={:02X}{:02X}{:02X}{:02X}"
            "{:02X}{:02X}{:02X}{:02X} L1[0..8]={:02X}{:02X}{:02X}{:02X}{:02X}"
            "{:02X}{:02X}{:02X} L1[mid]={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
            "{:02X}{:02X}",
            base, uint32_t(fetch.format), width, height, mip_addr,
            layout.packed_level, layout.mip_offsets_bytes[storage_level],
            lpitch_blocks, l0[0], l0[1], l0[2], l0[3], l0[4], l0[5], l0[6],
            l0[7], l1[0], l1[1], l1[2], l1[3], l1[4], l1[5], l1[6], l1[7],
            l1[mid1], l1[mid1 + 1], l1[mid1 + 2], l1[mid1 + 3], l1[mid1 + 4],
            l1[mid1 + 5], l1[mid1 + 6], l1[mid1 + 7]);
      }
      [texture replaceRegion:MTLRegionMake2D(0, 0, lw, lh)
                 mipmapLevel:level
                       slice:face
                   withBytes:mip_linear.data()
                 bytesPerRow:lbw * bpb
               bytesPerImage:0];
      }  // face
    }
  }

  if (!entry_mip_bytes) {
    pending_guest_hash_ = base_hash_ok ? HashGuestMips(base_hash, 0, 0) : 0;
  }
  return FinalizeTexture(key, fetch, (__bridge void*)texture,
                         uint64_t(plan.host_format), plan.host_swizzle,
                         is_cube, host_levels, base, watched_base_bytes,
                         entry_mip_bytes ? mip_addr : 0, entry_mip_bytes);
}

// The SINGLE finalize tail for every producer of texture bytes (today the
// CPU untile; the proven slice-4/5R GPU loads consumed it too): swizzle
// view, bindless slot, trace-read record and cache entry are identical
// regardless of which path produced the bytes — so trace ranges and
// cache/lifetime semantics cannot diverge by producer.
// texture_bridge is an id<MTLTexture> at +0 (caller holds a strong ref
// through the call).
std::string MetalTextureCache::ConsumeUntileTimingReport(uint32_t frames) {
  const float f = float(std::max(frames, 1u));
  std::vector<std::pair<uint32_t, UntileTimingBucket>> rows(
      untile_timing_.begin(), untile_timing_.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second.ns > b.second.ns;
  });
  std::string top;
  size_t shown = 0;
  for (const auto& [key, b] : rows) {
    if (shown++ >= 6) {
      break;
    }
    top += fmt::format(" fmt{}{}{}={:.2f}ms/{:.1f}", key >> 2,
                       (key & 2) ? "s" : "u", (key & 1) ? "R" : "",
                       double(b.ns) / 1e6 / f, float(b.count) / f);
  }
  std::string inval;
  for (const auto& [site, b] : inval_census_) {
    inval += fmt::format(" {}={:.1f}calls/{:.1f}evicted/{:.0f}KB", site,
                         float(b.calls) / f, float(b.evicted) / f,
                         double(b.bytes) / 1024.0 / f);
  }
  inval_census_.clear();
  std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> ranges(
      inval_ranges_.begin(), inval_ranges_.end());
  std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
    return a.second.second > b.second.second;
  });
  std::string top_ranges;
  size_t shown_ranges = 0;
  for (const auto& [k, v] : ranges) {
    if (shown_ranges++ >= 12) {
      break;
    }
    top_ranges += fmt::format(" {:08X}+{:X}={:.1f}/{:.1f}", uint32_t(k >> 32),
                              uint32_t(k), float(v.first) / f,
                              float(v.second) / f);
  }
  inval_ranges_.clear();
  std::string report = fmt::format(
      "untile {:.2f} ms/frame over {:.1f} misses/frame; top:{}; inval/frame:{}; "
      "revalidated {:.1f}/frame in {:.2f} ms (changed {:.1f}, parked {}); "
      "signed-twin {:.1f}/frame; "
      "evicting ranges (calls/evicted per frame):{}",
      double(untile_total_ns_) / 1e6 / f, float(untile_total_count_) / f,
      top, inval, float(census_revalidated_) / f,
      double(census_revalidate_ns_) / 1e6 / f,
      float(census_revalidate_changed_) / f, parked_.size(),
      float(census_signed_twin_) / f, top_ranges);
  census_signed_twin_ = 0;
  census_revalidated_ = 0;
  census_revalidate_changed_ = 0;
  census_revalidate_ns_ = 0;
  untile_timing_.clear();
  untile_total_ns_ = 0;
  untile_total_count_ = 0;
  return report;
}

uint32_t MetalTextureCache::FinalizeTexture(
    uint64_t key, const xenos::TextureFetch& fetch,
    void* texture_bridge, uint64_t host_format, uint32_t host_swizzle,
    bool is_cube, uint32_t host_levels, uint32_t base, uint32_t guest_bytes,
    uint32_t mip_base, uint32_t mip_bytes) {
  id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_bridge;
  // The texture unit applies the fetch-constant swizzle (X/Y/Z/W/0/1 per output
  // channel) when sampling. Bake it into a swizzled texture VIEW so the shader
  // reads the correct channels (matches the Vulkan backend's host_swizzle). A
  // wrong swizzle here reads alpha from the wrong channel, and alpha-test then
  // discards the whole draw.
  // The host format's own channel mapping (RRRR depth, RGGG two-channel)
  // composes underneath the guest swizzle; identity host swizzle leaves the
  // historical behaviour bit-for-bit.
  uint32_t guest_swizzle = ComposeGuestHostSwizzle(fetch.swizzle & 0xFFF,
                                                   host_swizzle & 0xFFF);
  id<MTLTexture> sampled = texture;
  if (!GuestSwizzleIsIdentity(guest_swizzle)) {
    MTLTextureSwizzleChannels channels = MTLTextureSwizzleChannelsMake(
        GuestSwizzleComponentToMetal((guest_swizzle >> 0) & 0b111),
        GuestSwizzleComponentToMetal((guest_swizzle >> 3) & 0b111),
        GuestSwizzleComponentToMetal((guest_swizzle >> 6) & 0b111),
        GuestSwizzleComponentToMetal((guest_swizzle >> 9) & 0b111));
    // The view keeps the base texture's type (2D array, cube array or 3D):
    // the translated shader's resource type must match what it samples.
    id<MTLTexture> view = [texture
        newTextureViewWithPixelFormat:MTLPixelFormat(host_format)
                          textureType:texture.textureType
                               levels:NSMakeRange(0, host_levels)
                               slices:NSMakeRange(0, is_cube ? 6
                                                              : texture.arrayLength)
                              swizzle:channels];
    if (!view) {
      binding_allocation_failed_ = true;
      return 0;
    }
    sampled = view;
  }

  uint32_t slot = AllocateSlot();
  if (!slot) {
    return 0;
  }
  auto* entries = static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
  WriteSlotDescriptor(slot, (__bridge void*)sampled);
  // Retain the sampled view (or the base texture if identity) for the descriptor
  // and residency. A view keeps its base alive, so this covers both.
  void* texture_ptr = (__bridge_retained void*)sampled;
  ++upload_count_;
  // Frame-trace recording: a replay must restore this texture's guest bytes
  // before the fetch that triggered this upload. No-op unless tracing.
  if (trace_writer_) {
    // No trace writer in this tree.
    if (mip_bytes) {
      // No trace writer in this tree.
    }
  }
  EmplaceEntry(
      key, Entry{texture_ptr, slot, base, guest_bytes, mip_base, mip_bytes, pending_guest_hash_});
  pending_guest_hash_ = 0;
  active_textures_.push_back(texture_ptr);
  return slot;
}

void MetalTextureCache::WriteAllCachedTexturesToTrace(
    TraceWriter& trace_writer) const {
  for (const auto& pair : cache_) {
    const Entry& entry = pair.second;
    if (entry.guest_bytes) {
      (void)entry;  // No trace writer in this tree.
    }
    if (entry.mip_bytes) {
      // No trace writer in this tree.
    }
  }
}

void MetalTextureCache::RegisterResolvedTarget(uint32_t base, void* texture,
                                               uint32_t width, uint32_t height,
                                               bool is_depth,
                                               bool requires_fetch_swizzle,
                                               uint32_t guest_extent_start,
                                               uint32_t guest_extent_length) {
  ++registry_epoch_;
  if (!texture) {
    return;
  }
  auto* entries = static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
  auto it = resolved_targets_.find(base);
  if (it != resolved_targets_.end()) {
    auto guest_extent = metal::GuestRange::FromBaseLength(
        guest_extent_start, guest_extent_length);
    uint32_t normalized_extent_start = guest_extent ? guest_extent->start() : 0;
    uint32_t normalized_extent_length = guest_extent ? guest_extent->length() : 0;
    bool texture_changed = it->second.texture != texture;
    bool extent_changed =
        it->second.width != width || it->second.height != height ||
        it->second.guest_extent_start != normalized_extent_start ||
        it->second.guest_extent_length != normalized_extent_length;

    if (texture_changed) {
      // Allocate the replacement before mutating metadata or retiring any
      // descriptor/view. On exhaustion, the old entry remains one internally
      // consistent generation and the caller can fail closed.
      static const bool s_fault_inject =
          std::getenv("XE_METAL_FAULT_INJECT") != nullptr;
      uint32_t new_slot = it->second.slot;
      if (!s_fault_inject) {
        new_slot = AllocateSlot();
        if (!new_slot) {
          return;
        }
      }

      // Every per-swizzle exact-cover view aliases the old snapshot texture.
      // Retire it through the same in-flight-safe pools as the base slot.
      for (auto& sv : it->second.swizzle_views) {
        RetireSlot(RetireSubmission(), sv.second.slot);
        retired_textures_.emplace_back(RetireSubmission(), sv.second.view);
      }
      it->second.swizzle_views.clear();
      if (!s_fault_inject) {
        // Never repoint a descriptor used by an in-flight draw. The new
        // generation gets a fresh slot while the old slot drains.
        RetireSlot(RetireSubmission(), it->second.slot);
      }
      WriteSlotDescriptor(new_slot, texture);
      it->second.texture = texture;
      CacheSnapshotProperties(it->second);
      it->second.slot = new_slot;
    }

    // A new snapshot generation or extent invalidates the exact-cover
    // state: RAM equivalence must be re-established by the next writeback.
    if (texture_changed || extent_changed) {
      it->second.full_rect = false;
    }
    // Cache-clear classification, on a DIFFERENT condition from full_rect
    // above: EVERY successful registration is a new content generation, not
    // just one that changed object identity or extent. A resolve reuses the
    // existing snapshot object whenever it is large enough
    // (EnsureResolveSnapshot returns it), so ResolveColorToSnapshot overwrites
    // the content in place with the pointer and dimensions unchanged, and this
    // call happens before that resolve's writeback decision. Treating "same
    // texture, same extent" as the same generation would let a skipped or
    // failed writeback inherit the previous generation's RAM-backed status,
    // and a clear would then release a snapshot holding content guest RAM
    // never received. Reset unconditionally here -- after the allocation
    // failure return above, so a registration that bailed out leaves the entry
    // as one consistent generation. ram_backed itself stays sticky, so no
    // serving decision changes.
    it->second.generation_ram_backed = false;
    ++it->second.generation;
    it->second.width = width;
    it->second.height = height;
    it->second.is_depth = is_depth;
    it->second.requires_fetch_swizzle = requires_fetch_swizzle;
    it->second.guest_extent_start = normalized_extent_start;
    it->second.guest_extent_length = normalized_extent_length;
    if (IsDrawResolutionScaled()) {
      // A new texture starts with only this resolve's rows. Reusing the same
      // texture keeps the rows earlier resolves wrote, which still match
      // guest RAM: any writer overlapping them since would have retired this
      // registration through the union extent.
      if (texture_changed || !it->second.scaled_extent_length) {
        it->second.scaled_rows_written = height;
        it->second.scaled_extent_start = normalized_extent_start;
        it->second.scaled_extent_length = normalized_extent_length;
      } else {
        it->second.scaled_rows_written =
            std::max(it->second.scaled_rows_written, height);
        uint32_t union_start =
            std::min(it->second.scaled_extent_start, normalized_extent_start);
        uint32_t union_end = std::max(
            it->second.scaled_extent_start + it->second.scaled_extent_length,
            normalized_extent_start + normalized_extent_length);
        it->second.scaled_extent_start = union_start;
        it->second.scaled_extent_length = union_end - union_start;
      }
    }
    return;
  }
  uint32_t slot = AllocateSlot();
  if (!slot) {
    return;
  }
  id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
  WriteSlotDescriptor(slot, (__bridge void*)tex);
  auto guest_extent = metal::GuestRange::FromBaseLength(
      guest_extent_start, guest_extent_length);
  resolved_targets_[base] = ResolvedTarget{
      texture,
      width,
      height,
      slot,
      guest_extent ? guest_extent->start() : 0,
      guest_extent ? guest_extent->length() : 0,
      is_depth,
      requires_fetch_swizzle};
  CacheSnapshotProperties(resolved_targets_[base]);
  if (IsDrawResolutionScaled()) {
    ResolvedTarget& registered = resolved_targets_[base];
    registered.scaled_rows_written = height;
    registered.scaled_extent_start = registered.guest_extent_start;
    registered.scaled_extent_length = registered.guest_extent_length;
  }
  REXLOG_INFO("MetalTex: resolved RT registered base={:08X} {}x{} -> slot {}", base,
         width, height, slot);
}

void MetalTextureCache::RegisterResolvedTipView(uint32_t base, void* texture) {
  ++registry_epoch_;
  if (!texture) {
    return;
  }
  auto it = resolved_targets_.find(base);
  if (it == resolved_targets_.end() || it->second.tip_texture == texture) {
    return;  // no registered target yet, or stable object already slotted.
  }
  uint32_t new_slot = AllocateSlot();
  if (!new_slot) {
    return;  // heap exhausted; keep serving the full snapshot.
  }
  if (it->second.tip_slot) {
    // Same page-fault rule as RegisterResolvedTarget: in-flight draws hold the
    // old texture by gpuAddress — retire the slot, never repoint in place.
    RetireSlot(RetireSubmission(), it->second.tip_slot);
  }
  // Per-tip swizzled views alias the OLD tip texture — retire them (slot +
  // retained view) through the same in-flight-safe pool so in-flight draws keep
  // sampling them until their submissions drain.
  for (auto& sv : it->second.tip_swizzle_views) {
    RetireSlot(RetireSubmission(), sv.second.slot);
    retired_textures_.emplace_back(RetireSubmission(), sv.second.view);
  }
  it->second.tip_swizzle_views.clear();
  auto* entries = static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
  WriteSlotDescriptor(new_slot, texture);
  it->second.tip_texture = texture;
  it->second.tip_slot = new_slot;
  REXLOG_INFO("MetalTex: tip view registered base={:08X} -> slot {}", base,
         new_slot);
}

bool MetalTextureCache::BridgeServes(uint32_t base, uint32_t width,
                                     uint32_t height, uint32_t pitch) const {
  auto it = resolved_targets_.find(base);
  if (it == resolved_targets_.end() || !it->second.texture) {
    return false;
  }
  const ResolvedTarget& rt = it->second;
  if (rt.is_depth) {
    return !DepthFetchIsComposite(rt, base, width, height, pitch);
  }
  return !rt.ram_backed;
}

void MetalTextureCache::ExtendResolvedTargetCoverage(void* texture,
                                                     uint32_t strip_y,
                                                     uint32_t rows,
                                                     uint32_t extent_start,
                                                     uint32_t extent_length,
                                                     uint32_t wb_endian) {
  ++registry_epoch_;
  if (!IsDrawResolutionScaled() || !texture) {
    return;
  }
  auto extent = metal::GuestRange::FromBaseLength(extent_start, extent_length);
  for (auto& pair : resolved_targets_) {
    ResolvedTarget& rt = pair.second;
    if (rt.texture != texture) {
      continue;
    }
    // Only a strip contiguous with the written rows, written back like the
    // base resolve, extends them; a gap would leave rows holding content
    // guest RAM never received.
    if (strip_y <= rt.scaled_rows_written && wb_endian == rt.wb_endian) {
      rt.scaled_rows_written =
          std::max(rt.scaled_rows_written, strip_y + rows);
    }
    if (extent) {
      uint32_t union_start = rt.scaled_extent_length
                                 ? std::min(rt.scaled_extent_start,
                                            extent->start())
                                 : extent->start();
      uint32_t union_end = std::max(
          rt.scaled_extent_start + rt.scaled_extent_length, extent->end());
      rt.scaled_extent_start = union_start;
      rt.scaled_extent_length = union_end - union_start;
    }
    return;
  }
}

void MetalTextureCache::MarkResolvedTargetRamBacked(uint32_t base,
                                                    uint32_t wb_width,
                                                    uint32_t wb_height,
                                                    bool full_rect_geometry,
                                                    uint32_t wb_endian) {
  ++registry_epoch_;
  auto it = resolved_targets_.find(base);
  if (it == resolved_targets_.end()) {
    return;
  }
  if (!it->second.ram_backed) {
    it->second.ram_backed = true;
    REXLOG_INFO("MetalTex: resolved RT base={:08X} now RAM-backed (bridge retired)",
           base);
  }
  // This writeback belongs to the CURRENT generation, so its content is now in
  // guest RAM (cache-clear classification; see the field comment).
  it->second.generation_ram_backed = true;
  // The newest writeback governs: a full-extent write makes RAM equal to the
  // snapshot regardless of history; anything partial/offset means RAM is a
  // composition the snapshot cannot represent.
  it->second.full_rect = full_rect_geometry &&
                         wb_width == it->second.width &&
                         wb_height == it->second.height;
  it->second.wb_endian = wb_endian;
}

MetalTextureCache::ByteCensus MetalTextureCache::CensusBytes() const {
  ByteCensus c;
  auto bytes = [](void* bridge) -> uint64_t {
    return bridge ? uint64_t([(__bridge id<MTLTexture>)bridge allocatedSize])
                  : 0;
  };
  for (const auto& pair : cache_) {
    c.cache_bytes += bytes(pair.second.texture);
    ++c.cache_count;
  }
  for (const auto& parked : parked_) {
    c.parked_bytes += bytes(parked.second.texture);
    ++c.parked_count;
  }
  for (const auto& pair : resolved_targets_) {
    c.resolved_bytes += bytes(pair.second.texture) +
                        bytes(pair.second.tip_texture) +
                        bytes(pair.second.fitted.texture);
    ++c.resolved_count;
  }
  for (const auto& entry : retired_textures_) {
    c.retired_bytes += bytes(entry.second);
    ++c.retired_count;
  }
  return c;
}

void MetalTextureCache::AppendAllTextures(std::vector<void*>& out) const {
  for (const auto& pair : cache_) {
    if (pair.second.texture) {
      out.push_back(pair.second.texture);
    }
  }
  for (const auto& pair : resolved_targets_) {
    if (pair.second.texture) {
      out.push_back(pair.second.texture);
    }
    if (pair.second.tip_texture) {
      out.push_back(pair.second.tip_texture);
    }
    for (const auto& sv : pair.second.swizzle_views) {
      if (sv.second.view) {
        out.push_back(sv.second.view);
      }
    }
    for (const auto& sv : pair.second.tip_swizzle_views) {
      if (sv.second.view) {
        out.push_back(sv.second.view);
      }
    }
    if (pair.second.fitted.texture) {
      out.push_back(pair.second.fitted.texture);
    }
    for (const auto& sv : pair.second.fitted.swizzle_views) {
      if (sv.second.view) {
        out.push_back(sv.second.view);
      }
    }
  }
}

void MetalTextureCache::BindExternalSlot(uint32_t slot, void* texture_bridge) {
  if (!slot || slot >= view_heap_slots_) {
    return;
  }
  auto* entries = static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
  WriteSlotDescriptor(slot, texture_bridge);
}

uint32_t MetalTextureCache::AllocateSlot() {
  if (!free_slots_.empty()) {
    uint32_t slot = free_slots_.back();
    free_slots_.pop_back();
    return slot;
  }
  if (next_slot_ >= view_heap_slots_) {
    // Exhaustion was SILENT here while consumers degraded to stale/null
    // textures (the zero-exposure black flicker) — scream, throttled.
    static uint32_t s_exhausted_logs = 0;
    if (s_exhausted_logs++ < 8 || (s_exhausted_logs & 1023) == 0) {
      REXLOG_ERROR("MetalTextureCache: BINDLESS HEAP EXHAUSTED ({} slots, {} stale)",
             view_heap_slots_, uint32_t(stale_slots_.size()));
    }
    binding_allocation_failed_ = true;
    return 0;  // exhausted (0 == null texture).
  }
  return next_slot_++;
}



void MetalTextureCache::WriteSlotDescriptor(uint32_t slot,
                                            void* texture_bridge) {
  ++binding_generation_;
  auto* entries = static_cast<IRDescriptorTableEntry*>(view_heap_contents_);
  if (!entries || !slot || slot >= view_heap_slots_) {
    return;
  }
  if (SlotAuditEnabled()) {
    if (audit_slot_texture_.size() < view_heap_slots_) {
      audit_slot_texture_.resize(view_heap_slots_, nullptr);
      audit_slot_last_ref_.resize(view_heap_slots_, 0);
    }
    void* old_texture = audit_slot_texture_[slot];
    if (old_texture && old_texture != texture_bridge &&
        audit_slot_last_ref_[slot] > audit_completed_) {
      ++audit_live_rewrites_;
      if (audit_live_rewrites_ <= 64 || !(audit_live_rewrites_ & 1023)) {
        REXLOG_ERROR("MetalSlotAudit: LIVE SLOT REWRITE #{} slot={} last_ref={} "
               "completed={} old={} new={} caller={}",
               audit_live_rewrites_, slot, audit_slot_last_ref_[slot],
               audit_completed_, DescribeAuditTexture(old_texture),
               DescribeAuditTexture(texture_bridge),
               DescribeAuditCaller(__builtin_return_address(0)));
      }
    }
    audit_slot_texture_[slot] = texture_bridge;
  }
  IRDescriptorTableSetTexture(&entries[slot],
                              (__bridge id<MTLTexture>)texture_bridge, 0.0f, 0);
  if (residency_add_ && texture_bridge) {
    residency_add_(texture_bridge);
  }
}

bool MetalTextureCache::SlotAuditEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_SLOT_AUDIT");
    return v && v[0] == '1';
  }();
  return s_enabled;
}

void MetalTextureCache::AuditDrawSlots(const uint32_t* slots, size_t count,
                                       uint64_t submission) {
  if (audit_slot_texture_.size() < view_heap_slots_) {
    audit_slot_texture_.resize(view_heap_slots_, nullptr);
    audit_slot_last_ref_.resize(view_heap_slots_, 0);
  }
  static uint64_t s_audited = 0;
  if (!(s_audited & 0xFFFF) && count) {
    REXLOG_INFO("MetalSlotAudit: {} slots audited, {} undeclared, {} live "
           "rewrites, completed={}",
           s_audited, audit_unresident_, audit_live_rewrites_,
           audit_completed_);
  }
  s_audited += count;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t slot = slots[i];
    if (!slot || slot >= view_heap_slots_) {
      continue;
    }
    audit_slot_last_ref_[slot] =
        std::max(audit_slot_last_ref_[slot], submission);
    void* texture = audit_slot_texture_[slot];
    if (!texture ||
        std::find(active_textures_.begin(), active_textures_.end(), texture) !=
            active_textures_.end()) {
      continue;
    }
    __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
    bool heap_backed = false;
    for (uint32_t depth = 0; t && depth < 4 && !heap_backed; ++depth) {
      heap_backed = t.heap != nil;
      t = t.parentTexture;
    }
    if (heap_backed && heaps_enabled_) {
      continue;
    }
    ++audit_unresident_;
    if (audit_unresident_ <= 64 || !(audit_unresident_ & 1023)) {
      REXLOG_ERROR("MetalSlotAudit: UNDECLARED #{} slot={} submission={} {} "
             "(heap={}, {} active)",
             audit_unresident_, slot, submission,
             DescribeAuditTexture(texture), heap_backed,
             active_textures_.size());
    }
  }
}

std::string MetalTextureCache::DescribeAuditTexture(void* texture_bridge) {
  if (!texture_bridge) {
    return "null";
  }
  __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)texture_bridge;
  __unsafe_unretained id<MTLTexture> root = t;
  while (root.parentTexture) {
    root = root.parentTexture;
  }
  return fmt::format("{:p} {}x{}x{} L{} fmt={} type={} storage={} view={} "
                     "heap={}",
                     texture_bridge, uint32_t(t.width), uint32_t(t.height),
                     uint32_t(t.depth > 1 ? t.depth : t.arrayLength),
                     uint32_t(t.mipmapLevelCount), uint32_t(t.pixelFormat),
                     uint32_t(t.textureType), uint32_t(root.storageMode),
                     t.parentTexture != nil, root.heap != nil);
}

std::string MetalTextureCache::DescribeAuditCaller(void* address) {
  Dl_info info;
  if (dladdr(address, &info) && info.dli_fbase) {
    return fmt::format("+0x{:x}", uintptr_t(address) -
                                      uintptr_t(info.dli_fbase));
  }
  return fmt::format("{:p}", address);
}

void MetalTextureCache::ReclaimStaleSlots(uint64_t completed_submission) {
  audit_completed_ = std::max(audit_completed_, completed_submission);
  // Free ONLY entries whose last-referencing submission has completed —
  // later in-flight submissions still read the rest (freeing them was the
  // async-S8 page-fault flicker). Descriptors are overwritten on the next
  // AllocateSlot that hands a freed slot out.
  //
  // NOTE (port-xenios-tessellation 5.3): leaving the descriptor naming the
  // released texture until reallocation was investigated as a page-fault
  // cause and MEASURED NOT TO BE ONE — closing the window entirely changed
  // nothing (4.88 -> 5.37 per 10k over five matched runs each). Rebinding
  // freed slots to the null texture, as ClearCache already does, remains
  // defensible HARDENING for a separate bounded change; it is deliberately
  // not carried here, where it would be unjustified as a fault fix.
  auto slot_it = std::remove_if(
      stale_slots_.begin(), stale_slots_.end(),
      [&](const std::pair<uint64_t, uint32_t>& entry) {
        if (entry.first > completed_submission) {
          return false;
        }
        free_slots_.push_back(entry.second);
        return true;
      });
  stale_slots_.erase(slot_it, stale_slots_.end());
  auto tex_it = std::remove_if(
      retired_textures_.begin(), retired_textures_.end(),
      [&](const std::pair<uint64_t, void*>& entry) {
        if (entry.first > completed_submission) {
          return false;
        }
        if (residency_release_) {
          residency_release_(entry.second);
        }
        id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)entry.second;
        (void)released;
        return true;
      });
  retired_textures_.erase(tex_it, retired_textures_.end());
}

void MetalTextureCache::RetireScaledAssembly(ScaledAssembly& assembly) {
  uint64_t retire_submission = RetireSubmission();
  if (assembly.slot) {
    RetireSlot(retire_submission, assembly.slot);
  }
  for (auto& swizzle_view : assembly.swizzle_views) {
    RetireSlot(retire_submission, swizzle_view.second.slot);
    retired_textures_.emplace_back(retire_submission,
                                   swizzle_view.second.view);
  }
  assembly.swizzle_views.clear();
}

void MetalTextureCache::RegisterScaledAssembly(
    uint32_t base0, void* texture, uint32_t width, uint32_t height, uint32_t x,
    uint32_t w, uint32_t extent_start, uint32_t extent_length,
    uint32_t wb_endian) {
  ++registry_epoch_;
  if (!IsDrawResolutionScaled() || !texture) {
    return;
  }
  ScaledAssembly& assembly = scaled_assemblies_[base0];
  if (assembly.texture != texture) {
    // A new texture (or first use) holds only this column. Allocate before
    // retiring so exhaustion leaves the previous state consistent.
    uint32_t slot = AllocateSlot();
    if (!slot) {
      return;
    }
    RetireScaledAssembly(assembly);
    WriteSlotDescriptor(slot, texture);
    assembly.texture = texture;
    assembly.slot = slot;
    assembly.width = width;
    assembly.height = height;
    assembly.covered_width = 0;
    assembly.extent_start = 0;
    assembly.extent_length = 0;
    assembly.wb_endian = wb_endian;
  }
  // Only a column contiguous with the covered columns, written back like
  // them, extends the coverage; a gap would leave columns holding content
  // guest RAM never received.
  if (x <= assembly.covered_width && wb_endian == assembly.wb_endian) {
    assembly.covered_width = std::max(assembly.covered_width, x + w);
  }
  auto extent = metal::GuestRange::FromBaseLength(extent_start, extent_length);
  if (extent) {
    uint32_t union_start = assembly.extent_length
                               ? std::min(assembly.extent_start,
                                          extent->start())
                               : extent->start();
    uint32_t union_end = std::max(
        assembly.extent_start + assembly.extent_length, extent->end());
    assembly.extent_start = union_start;
    assembly.extent_length = union_end - union_start;
  }
}

void MetalTextureCache::RetireResolvedTarget(ResolvedTarget& target) {
  uint64_t retire_submission = RetireSubmission();
  if (target.slot) {
    RetireSlot(retire_submission, target.slot);
  }
  if (target.tip_slot) {
    RetireSlot(retire_submission, target.tip_slot);
  }
  for (auto& swizzle_view : target.swizzle_views) {
    RetireSlot(retire_submission, swizzle_view.second.slot);
    retired_textures_.emplace_back(retire_submission,
                                   swizzle_view.second.view);
  }
  target.swizzle_views.clear();
  for (auto& swizzle_view : target.tip_swizzle_views) {
    RetireSlot(retire_submission, swizzle_view.second.slot);
    retired_textures_.emplace_back(retire_submission,
                                   swizzle_view.second.view);
  }
  target.tip_swizzle_views.clear();
  RetireFittedView(target.fitted);
}

uint32_t MetalTextureCache::InvalidateRange(uint32_t base, uint32_t length,
                                            InvalidationScope scope,
                                            const char* site) {
  auto invalidation = metal::GuestRange::FromBaseLength(base, length);
  if (!invalidation) {
    return 0;
  }
  uint32_t evicted = 0;
  static const bool s_census =
      std::getenv("XE_METAL_UNTILE_TIMING") != nullptr;
  InvalidationSiteBucket* census = nullptr;
  if (s_census) {
    census = &inval_census_[site ? site : "other"];
    ++census->calls;
    census->bytes += length;
  }
  auto overlaps_entry = [&](const Entry& e) {
    auto guest_range =
        metal::GuestRange::FromBaseLength(e.guest_base, e.guest_bytes);
    auto mip_range =
        metal::GuestRange::FromBaseLength(e.mip_base, e.mip_bytes);
    return (guest_range && invalidation->Overlaps(*guest_range)) ||
           (mip_range && invalidation->Overlaps(*mip_range));
  };
  auto evict = [&](std::unordered_map<uint64_t, Entry>::iterator it) {
    const Entry& e = it->second;
    // Retire, don't free: in-flight draws may still reference the texture
    // (bindless) and the slot; both drain once the submissions encoded
    // against them complete.
    if (RevalidateEnabled() && e.guest_hash && e.texture) {
      ParkEvictedEntry(it->first, e);
    } else {
      retired_textures_.emplace_back(RetireSubmission(), e.texture);
    }
    RetireSlot(RetireSubmission(), e.slot);
    UnindexEntry(it->first, e);
    ++evicted;
    ++binding_generation_;
    return cache_.erase(it);
  };
  if (InvalidationIndexEnabled()) {
    // Candidates: every key filed under a bucket the invalidation touches.
    std::vector<uint64_t> candidates;
    if (!entry_buckets_.empty()) {
      const uint32_t bucket_first = uint32_t(invalidation->start() >> kEntryBucketShift);
      const uint32_t bucket_last = uint32_t((invalidation->end() - 1) >> kEntryBucketShift);
      for (uint32_t b = bucket_first; b <= bucket_last; ++b) {
        const std::vector<uint64_t>& keys = entry_buckets_[b];
        candidates.insert(candidates.end(), keys.begin(), keys.end());
      }
      if (bucket_first != bucket_last) {
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
      }
    }
    static const bool s_verify = std::getenv("XE_METAL_TEX_INVAL_INDEX_VERIFY") != nullptr;
    if (s_verify) {
      std::vector<uint64_t> scanned, indexed;
      for (const auto& [key, e] : cache_) {
        if (overlaps_entry(e)) {
          scanned.push_back(key);
        }
      }
      for (uint64_t key : candidates) {
        auto it = cache_.find(key);
        if (it != cache_.end() && overlaps_entry(it->second)) {
          indexed.push_back(key);
        }
      }
      std::sort(scanned.begin(), scanned.end());
      std::sort(indexed.begin(), indexed.end());
      ++inval_index_verify_calls_;
      if (scanned != indexed) {
        if (inval_index_verify_mismatches_++ < 16) {
          REXLOG_WARN("MetalTexInvalIndexVerify MISMATCH {:08X}+{:X}: scan={} "
                 "index={}",
                 base, length, scanned.size(), indexed.size());
        }
      }
      if ((inval_index_verify_calls_ & 4095) == 0) {
        REXLOG_INFO("MetalTexInvalIndexVerify: calls={} mismatches={}", inval_index_verify_calls_,
               inval_index_verify_mismatches_);
      }
    }
    for (uint64_t key : candidates) {
      auto it = cache_.find(key);
      if (it != cache_.end() && overlaps_entry(it->second)) {
        evict(it);
      }
    }
  } else {
    for (auto it = cache_.begin(); it != cache_.end();) {
      if (overlaps_entry(it->second)) {
        it = evict(it);
      } else {
        ++it;
      }
    }
  }
  if (scope == InvalidationScope::kAllRepresentations) {
    for (auto it = resolved_targets_.begin();
         it != resolved_targets_.end();) {
      auto guest_extent = metal::GuestRange::FromBaseLength(
          it->second.guest_extent_start, it->second.guest_extent_length);
      auto scaled_extent = metal::GuestRange::FromBaseLength(
          it->second.scaled_extent_start, it->second.scaled_extent_length);
      if ((guest_extent && invalidation->Overlaps(*guest_extent)) ||
          (scaled_extent && invalidation->Overlaps(*scaled_extent))) {
        RetireResolvedTarget(it->second);
        ++registry_epoch_;
        it = resolved_targets_.erase(it);
        ++evicted;
      } else {
        ++it;
      }
    }
    for (auto it = scaled_assemblies_.begin();
         it != scaled_assemblies_.end();) {
      auto extent = metal::GuestRange::FromBaseLength(
          it->second.extent_start, it->second.extent_length);
      if (extent && invalidation->Overlaps(*extent)) {
        RetireScaledAssembly(it->second);
        it = scaled_assemblies_.erase(it);
        ++evicted;
      } else {
        ++it;
      }
    }
  }
  if (census) {
    census->evicted += evicted;
  }
  return evicted;
}

bool MetalTextureCache::InvalidationIndexEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_INVAL_INDEX");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

namespace {
// Calls fn(bucket) once for each 1 MiB bucket an entry's base or mip range
// covers (a bucket shared by both ranges is visited once).
template <typename Fn>
void ForEachEntryBucket(uint32_t guest_base, uint32_t guest_bytes, uint32_t mip_base,
                        uint32_t mip_bytes, uint32_t shift, Fn&& fn) {
  auto base_range = metal::GuestRange::FromBaseLength(guest_base, guest_bytes);
  auto mip_range = metal::GuestRange::FromBaseLength(mip_base, mip_bytes);
  uint32_t first0 = 1, last0 = 0;
  if (base_range) {
    first0 = uint32_t(base_range->start() >> shift);
    last0 = uint32_t((base_range->end() - 1) >> shift);
    for (uint32_t b = first0; b <= last0; ++b) {
      fn(b);
    }
  }
  if (mip_range) {
    const uint32_t first1 = uint32_t(mip_range->start() >> shift);
    const uint32_t last1 = uint32_t((mip_range->end() - 1) >> shift);
    for (uint32_t b = first1; b <= last1; ++b) {
      if (!(base_range && b >= first0 && b <= last0)) {
        fn(b);
      }
    }
  }
}
}  // namespace

void MetalTextureCache::IndexEntry(uint64_t key, const Entry& entry) {
  if (!InvalidationIndexEnabled()) {
    return;
  }
  if (entry_buckets_.empty()) {
    entry_buckets_.resize(kEntryBucketCount);
  }
  ForEachEntryBucket(entry.guest_base, entry.guest_bytes, entry.mip_base, entry.mip_bytes,
                     kEntryBucketShift, [&](uint32_t b) { entry_buckets_[b].push_back(key); });
}

void MetalTextureCache::UnindexEntry(uint64_t key, const Entry& entry) {
  if (!InvalidationIndexEnabled() || entry_buckets_.empty()) {
    return;
  }
  ForEachEntryBucket(entry.guest_base, entry.guest_bytes, entry.mip_base, entry.mip_bytes,
                     kEntryBucketShift, [&](uint32_t b) {
                       std::vector<uint64_t>& keys = entry_buckets_[b];
                       auto it = std::find(keys.begin(), keys.end(), key);
                       if (it != keys.end()) {
                         *it = keys.back();
                         keys.pop_back();
                       }
                     });
}

void MetalTextureCache::ClearEntryIndex() {
  for (std::vector<uint64_t>& keys : entry_buckets_) {
    keys.clear();
  }
}

void MetalTextureCache::EmplaceEntry(uint64_t key, const Entry& entry) {
  auto [it, inserted] = cache_.emplace(key, entry);
  if (inserted) {
    it->second.last_use = ++use_clock_;
    IndexEntry(key, entry);
  }
}

bool MetalTextureCache::SlotLruEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_SLOT_LRU");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

void MetalTextureCache::EnforceSlotHeadroom() {
  if (!SlotLruEnabled()) {
    return;
  }
  const size_t never_allocated =
      view_heap_slots_ > next_slot_ ? size_t(view_heap_slots_ - next_slot_) : 0;
  const size_t available = never_allocated + free_slots_.size();
  const size_t headroom_low = size_t(view_heap_slots_) / 16;
  const size_t headroom_target = size_t(view_heap_slots_) * 3 / 16;
  if (available >= headroom_low) {
    return;
  }
  // Slots retired under in-flight submissions come back at reclaim.
  const size_t pending = stale_slots_.size();
  if (available + pending >= headroom_target) {
    return;
  }
  const size_t want = std::min(headroom_target - available - pending, cache_.size());
  if (!want) {
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> ages;  // (last_use, key)
  ages.reserve(cache_.size());
  for (const auto& [key, entry] : cache_) {
    ages.emplace_back(entry.last_use, key);
  }
  std::nth_element(ages.begin(), ages.begin() + (want - 1), ages.end());
  for (size_t i = 0; i < want; ++i) {
    auto it = cache_.find(ages[i].second);
    if (it == cache_.end()) {
      continue;
    }
    const Entry& e = it->second;
    // Same retirement as an invalidation: in-flight draws may still read the
    // texture and the slot until the encoding submission completes.
    if (RevalidateEnabled() && e.guest_hash && e.texture) {
      ParkEvictedEntry(it->first, e);
    } else {
      retired_textures_.emplace_back(RetireSubmission(), e.texture);
    }
    RetireSlot(RetireSubmission(), e.slot);
    UnindexEntry(it->first, e);
    cache_.erase(it);
    ++binding_generation_;
    ++lru_evictions_;
  }
  static uint32_t s_lru_logs = 0;
  if (s_lru_logs++ < 8 || (s_lru_logs & 255) == 0) {
    REXLOG_INFO("MetalTextureCache: slot headroom {} (pending {}), evicted {} LRU "
           "entries (total {}, cache {})",
           available, pending, want, lru_evictions_, cache_.size());
  }
}

bool MetalTextureCache::RevalidateEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_REVALIDATE");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

void MetalTextureCache::VerifyCachedAgainstGuest(uint64_t swap) {
  if (!shared_memory_ || trace_playback_) {
    return;
  }
  constexpr uint32_t kPage = 16384;
  // Writes the watch DID report evict their entries here, before the scan.
  DrainPendingInvalidations();
  struct Suspect {
    uint64_t key;
    void* texture;
    uint32_t page_address;
    uint32_t changed_pages;
  };
  std::vector<Suspect> suspects;
  std::unordered_map<uint64_t, StaleVerifyRecord> next;
  next.reserve(cache_.size());
  auto hash_pages = [&](uint32_t base, uint32_t bytes,
                        std::vector<uint64_t>& out) {
    if (!base || !bytes || base >= kPhysicalSize) {
      return;
    }
    const uint8_t* g =
        static_cast<const uint8_t*>(static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(base));
    if (!g) {
      return;
    }
    const uint32_t end = uint32_t(
        std::min<uint64_t>(uint64_t(base) + bytes, kPhysicalSize));
    for (uint32_t a = base; a < end;) {
      const uint32_t next_page = (a & ~(kPage - 1)) + kPage;
      const uint32_t stop = std::min(end, next_page);
      out.push_back(XXH3_64bits(g + (a - base), stop - a));
      a = stop;
    }
  };
  for (const auto& [key, e] : cache_) {
    StaleVerifyRecord rec{e.texture, {}};
    hash_pages(e.guest_base, e.guest_bytes, rec.page_hashes);
    hash_pages(e.mip_base, e.mip_bytes, rec.page_hashes);
    auto prev = stale_verify_.find(key);
    if (prev != stale_verify_.end() && prev->second.texture == e.texture &&
        prev->second.page_hashes.size() == rec.page_hashes.size()) {
      uint32_t changed = 0, first = UINT32_MAX;
      for (size_t i = 0; i < rec.page_hashes.size(); ++i) {
        if (rec.page_hashes[i] != prev->second.page_hashes[i]) {
          if (first == UINT32_MAX) {
            first = uint32_t(i);
          }
          ++changed;
        }
      }
      if (changed) {
        // Page index -> address (base pages first, then mip pages).
        uint32_t base_pages = 0;
        if (e.guest_base && e.guest_bytes) {
          base_pages = ((e.guest_base + e.guest_bytes - 1) / kPage) -
                       (e.guest_base / kPage) + 1;
        }
        uint32_t address =
            first < base_pages
                ? std::max(e.guest_base, (e.guest_base & ~(kPage - 1)) +
                                             first * kPage)
                : std::max(e.mip_base, (e.mip_base & ~(kPage - 1)) +
                                           (first - base_pages) * kPage);
        suspects.push_back({key, e.texture, address, changed});
      }
    }
    next.emplace(key, std::move(rec));
  }
  stale_verify_.swap(next);
  if (suspects.empty()) {
    return;
  }
  // A write that faulted during the scan queued an invalidation that has not
  // been drained yet; only entries that survive this drain are real misses.
  DrainPendingInvalidations();
  for (const Suspect& s : suspects) {
    auto it = cache_.find(s.key);
    if (it == cache_.end() || it->second.texture != s.texture) {
      continue;
    }
    const Entry& e = it->second;
    ++stale_verify_hits_;
    if (stale_verify_hits_ > 64 && (stale_verify_hits_ & 255) != 0) {
      continue;
    }
    id<MTLTexture> t = (__bridge id<MTLTexture>)e.texture;
    REXLOG_WARN("MetalTexStaleVerify swap#{}: key {:016X} base {:08X}+{:X} mip "
           "{:08X}+{:X} {}x{} fmt {} changed {} page(s), first {:08X} "
           "UNREPORTED (hit {}) watch {}",
           swap, s.key, e.guest_base, e.guest_bytes, e.mip_base, e.mip_bytes,
           t ? uint32_t(t.width) : 0, t ? uint32_t(t.height) : 0,
           t ? uint32_t(t.pixelFormat) : 0, s.changed_pages, s.page_address,
           stale_verify_hits_, s.page_address);
  }
}

uint64_t MetalTextureCache::HashGuestRegions(uint32_t base, uint32_t bytes,
                                             uint32_t mip_base,
                                             uint32_t mip_bytes) const {
  bool ok = false;
  const uint64_t base_hash = HashGuestBase(base, bytes, &ok);
  return ok ? HashGuestMips(base_hash, mip_base, mip_bytes) : 0;
}

uint64_t MetalTextureCache::HashGuestBase(uint32_t base, uint32_t bytes,
                                          bool* ok) const {
  *ok = shared_memory_ != nullptr;
  if (!shared_memory_ || !base || !bytes || base >= kPhysicalSize) {
    return 0;
  }
  const uint8_t* g =
      static_cast<const uint8_t*>(static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(base));
  if (!g) {
    *ok = false;
    return 0;
  }
  return XXH3_64bits(g, std::min<size_t>(bytes, kPhysicalSize - base));
}

uint64_t MetalTextureCache::HashGuestMips(uint64_t base_hash,
                                          uint32_t mip_base,
                                          uint32_t mip_bytes) const {
  uint64_t hash = base_hash;
  if (mip_base && mip_bytes && mip_base < kPhysicalSize) {
    const uint8_t* g =
        static_cast<const uint8_t*>(static_cast<metal::MetalSharedMemory*>(shared_memory_)->TranslateGuestAddress<const void*>(mip_base));
    if (!g) {
      return 0;
    }
    hash = XXH3_64bits_withSeed(
        g, std::min<size_t>(mip_bytes, kPhysicalSize - mip_base), hash);
  }
  // 0 is the "no hash" sentinel.
  return hash ? hash : 1;
}

void MetalTextureCache::ParkEvictedEntry(uint64_t key, const Entry& entry) {
  DropParked(key);
  if (parked_.size() >= kMaxParkedTextures) {
    // Drop the oldest parked texture.
    auto oldest = parked_.begin();
    for (auto it = parked_.begin(); it != parked_.end(); ++it) {
      if (it->second.stamp < oldest->second.stamp) {
        oldest = it;
      }
    }
    retired_textures_.emplace_back(RetireSubmission(), oldest->second.texture);
    parked_.erase(oldest);
  }
  parked_.emplace(key, ParkedTexture{entry.texture, entry.guest_hash,
                                     entry.guest_base, entry.guest_bytes,
                                     entry.mip_base, entry.mip_bytes,
                                     ++parked_stamp_});
}

void MetalTextureCache::DropParked(uint64_t key) {
  auto it = parked_.find(key);
  if (it == parked_.end()) {
    return;
  }
  retired_textures_.emplace_back(RetireSubmission(), it->second.texture);
  parked_.erase(it);
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
