/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <fmt/format.h>
#include "rex/graphics/metal/metal_shared_texture_cache.h"

#import <Metal/Metal.h>

// The C++-visible format table stores MTLPixelFormat raw values; pin them.
static_assert(MTLPixelFormatR8Unorm == rex::graphics::metal::kMtlPixelFormatR8Unorm, "");
static_assert(MTLPixelFormatABGR4Unorm == rex::graphics::metal::kMtlPixelFormatABGR4Unorm, "");
static_assert(MTLPixelFormatB5G6R5Unorm == rex::graphics::metal::kMtlPixelFormatB5G6R5Unorm, "");
static_assert(MTLPixelFormatBC1_RGBA == rex::graphics::metal::kMtlPixelFormatBC1_RGBA, "");
static_assert(MTLPixelFormatBC2_RGBA == rex::graphics::metal::kMtlPixelFormatBC2_RGBA, "");
static_assert(MTLPixelFormatBC3_RGBA == rex::graphics::metal::kMtlPixelFormatBC3_RGBA, "");
static_assert(MTLPixelFormatBC4_RUnorm == rex::graphics::metal::kMtlPixelFormatBC4_RUnorm, "");
static_assert(MTLPixelFormatBC4_RSnorm == rex::graphics::metal::kMtlPixelFormatBC4_RSnorm, "");
static_assert(MTLPixelFormatBC5_RGUnorm == rex::graphics::metal::kMtlPixelFormatBC5_RGUnorm, "");
static_assert(MTLPixelFormatBC5_RGSnorm == rex::graphics::metal::kMtlPixelFormatBC5_RGSnorm, "");
static_assert(MTLPixelFormatR8Snorm == rex::graphics::metal::kMtlPixelFormatR8Snorm, "");
static_assert(MTLPixelFormatR16Unorm == rex::graphics::metal::kMtlPixelFormatR16Unorm, "");
static_assert(MTLPixelFormatR16Snorm == rex::graphics::metal::kMtlPixelFormatR16Snorm, "");
static_assert(MTLPixelFormatR16Float == rex::graphics::metal::kMtlPixelFormatR16Float, "");
static_assert(MTLPixelFormatRG8Unorm == rex::graphics::metal::kMtlPixelFormatRG8Unorm, "");
static_assert(MTLPixelFormatRG8Snorm == rex::graphics::metal::kMtlPixelFormatRG8Snorm, "");
static_assert(MTLPixelFormatR32Float == rex::graphics::metal::kMtlPixelFormatR32Float, "");
static_assert(MTLPixelFormatRG16Unorm == rex::graphics::metal::kMtlPixelFormatRG16Unorm, "");
static_assert(MTLPixelFormatRG16Snorm == rex::graphics::metal::kMtlPixelFormatRG16Snorm, "");
static_assert(MTLPixelFormatRG16Float == rex::graphics::metal::kMtlPixelFormatRG16Float, "");
static_assert(MTLPixelFormatRGBA8Unorm == rex::graphics::metal::kMtlPixelFormatRGBA8Unorm, "");
static_assert(MTLPixelFormatRGBA8Snorm == rex::graphics::metal::kMtlPixelFormatRGBA8Snorm, "");
static_assert(MTLPixelFormatRGB10A2Unorm == rex::graphics::metal::kMtlPixelFormatRGB10A2Unorm, "");
static_assert(MTLPixelFormatRGBA16Unorm == rex::graphics::metal::kMtlPixelFormatRGBA16Unorm, "");
static_assert(MTLPixelFormatRGBA16Snorm == rex::graphics::metal::kMtlPixelFormatRGBA16Snorm, "");
static_assert(MTLPixelFormatRGBA16Float == rex::graphics::metal::kMtlPixelFormatRGBA16Float, "");

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <rex/graphics/metal/metal_settings.h>
#include <rex/logging/log_category.h>
#include "rex/math.h"
#include "rex/graphics/metal/metal_load_shader_pipelines.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/graphics/metal/metal_mip_level_plan.h"
#include "rex/graphics/metal/metal_texture_load_eligibility.h"
#include "rex/graphics/trace_writer.h"
#include "rex/memory.h"
#include "xxhash.h"

// Declared by this SDK in rex/graphics/flags.h as int32; upstream declares them here as uint32, and the second
// declaration is what collides. The SDK's is the one in force.

namespace rex {
namespace graphics {
namespace metal {

// Store convention: the direct load writes the 2D base texture; consuming
// guest shaders sample texture2d_array (MSC ForceTextureArray), so the
// descriptor slot binds a 2DArray view with the binding's host swizzle —
// the same view mechanism the legacy FinalizeTexture uses.
static MTLTextureSwizzle HostSwizzleComponentToMetal(uint32_t component) {
  switch (component) {
    case xenos::REXGPU_SWIZZLE_R:
      return MTLTextureSwizzleRed;
    case xenos::REXGPU_SWIZZLE_G:
      return MTLTextureSwizzleGreen;
    case xenos::REXGPU_SWIZZLE_B:
      return MTLTextureSwizzleBlue;
    case xenos::REXGPU_SWIZZLE_A:
      return MTLTextureSwizzleAlpha;
    case xenos::REXGPU_SWIZZLE_0:
      return MTLTextureSwizzleZero;
    case xenos::REXGPU_SWIZZLE_1:
    default:
      return MTLTextureSwizzleOne;
  }
}

// Per-texture view cache (default). XE_METAL_ROUTE_VIEW_CACHE=0 returns to a
// view and slot per binding, re-created whenever a fetch constant changes
// texture.
static bool RouteViewCacheEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_ROUTE_VIEW_CACHE");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

// The xe_resolve_forward_store kind a resolve can write this texture with:
// single-level tiled 2D textures whose load is a plain copy of the guest
// dwords (after the fetch endianness), else 0.
uint32_t MetalSharedTextureCache::ResolveForwardKind(const TextureKey& key) {
  if (key.dimension != xenos::DataDimension::k2DOrStacked ||
      key.depth_or_array_size_minus_1 != 0 || key.mip_max_level != 0 ||
      !key.tiled || key.scaled_resolve || key.signed_separate) {
    return 0;
  }
  switch (key.format) {
    case xenos::TextureFormat::k_8_8_8_8:
      return 1;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return 2;
    case xenos::TextureFormat::k_32_FLOAT:
      return 3;
    default:
      return 0;
  }
}

namespace {
// XE_METAL_RESOLVE_FORWARD_TRACE=<hex address>: every forwarding decision,
// forward and load of the textures and destinations in the 8 MB from it.
bool ResolveForwardTraced(uint32_t address) {
  static const int64_t traced = [] {
    const char* v = std::getenv("XE_METAL_RESOLVE_FORWARD_TRACE");
    return v ? int64_t(strtoul(v, nullptr, 16)) : int64_t(-1);
  }();
  return traced >= 0 && address >= uint64_t(traced) &&
         address < uint64_t(traced) + (8u << 20);
}

bool ResolveForwardEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("XE_METAL_RESOLVE_FORWARD");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return enabled;
}
}  // namespace

class MetalSharedTextureCache::MetalTexture final
    : public TextureCache::Texture {
 public:
  MetalTexture(MetalSharedTextureCache& cache, const TextureKey& key,
               void* mtl_texture)
      : Texture(cache, key), mtl_texture_(mtl_texture) {
    // Full chain, not just the base level: LRU eviction and the memory limits
    // are driven by this number, so a mipped texture must not under-report.
    uint64_t bytes = 0;
    uint32_t levels = key.mip_max_level + 1;
    const SharedTextureFormatEntry* entry = FindSharedTextureFormat(key.format);
    const uint32_t bpb = entry ? SharedTextureHostBytesPerBlock(*entry) : 4u;
    // Host-BC textures hold one bpb-byte block per 4x4 texels; counting them
    // per texel over-reported a 1024x1024 DXT5 as 16 MB instead of 1 MB and
    // tripped the cache's 768 MB hard limit after a few dozen textures.
    const uint32_t host_block = entry && SharedTextureHostIsBc(*entry) ? 4u : 1u;
    for (uint32_t level = 0; level < levels; ++level) {
      uint64_t w = (std::max(key.GetWidth() >> level, 1u) + host_block - 1u) /
                   host_block;
      uint64_t h = (std::max(key.GetHeight() >> level, 1u) + host_block - 1u) /
                   host_block;
      bytes += w * h * bpb;
    }
    SetHostMemoryUsage(bytes * (key.depth_or_array_size_minus_1 + 1));
    // TODO(migration): GetHostMemoryUsage() came from the old base; upstream tracks host usage inside TextureCache now.
    ++cache.live_textures_;
    if (ResolveForwardKind(key)) {
      cache.forward_textures_.emplace(uint32_t(key.base_page) << 12, this);
    }
    if (ResolveForwardTraced(uint32_t(key.base_page) << 12)) {
      REXLOG_INFO("MetalResolveForward CREATE base={:08X} fmt={} {}x{} pitch={} "
             "mips={} tiled={} forwardable={}",
             uint32_t(key.base_page) << 12, uint32_t(key.format),
             key.GetWidth(), key.GetHeight(), key.pitch, key.mip_max_level,
             uint32_t(key.tiled), ResolveForwardKind(key));
    }
  }
  ~MetalTexture() override {
    auto& cache = static_cast<MetalSharedTextureCache&>(GetTextureCache());
    if (ResolveForwardTraced(uint32_t(GetKey().base_page) << 12)) {
      REXLOG_INFO("MetalResolveForward DESTROY base={:08X} fmt={} {}x{}",
             uint32_t(GetKey().base_page) << 12, uint32_t(GetKey().format),
             GetKey().GetWidth(), GetKey().GetHeight());
    }
    if (ResolveForwardKind(GetKey())) {
      auto range =
          cache.forward_textures_.equal_range(uint32_t(GetKey().base_page) << 12);
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second == this) {
          cache.forward_textures_.erase(it);
          break;
        }
      }
    }
    ++cache.census_destroyed_;
    // TODO(migration): see above.
    if (cache.live_textures_) {
      --cache.live_textures_;
    }
    cache.ForgetTextureBindings(this);
    for (const CachedView& cached : views) {
      if (cache.legacy_) {
        cache.legacy_->RetireExternalSlot(cached.slot);
        if (cache.external_slots_held_) {
          --cache.external_slots_held_;
        }
        cache.legacy_->RetireExternalTexture(cached.view);
      } else {
        id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)cached.view;
        (void)released;
      }
    }
    // Submission-gated release through the single legacy retirement pool:
    // the load encoder referenced this texture directly and views bound in
    // descriptor slots retain it; destruction is LRU/completion-gated, but
    // the retire pool is the uniformly safe release point.
    if (forward_view) {
      if (cache.legacy_) {
        cache.legacy_->RetireExternalTexture(forward_view);
      } else {
        id<MTLTexture> released =
            (__bridge_transfer id<MTLTexture>)forward_view;
        (void)released;
      }
    }
    if (cache.legacy_) {
      cache.legacy_->RetireExternalTexture(mtl_texture_);
    } else {
      id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)mtl_texture_;
      (void)released;
    }
  }
  void* mtl_texture() const { return mtl_texture_; }
  // Resolve forwarding: an unsigned integer view of the texture (retained),
  // created on the first forward.
  void* forward_view = nullptr;

  // Base and mip load provenance are tracked SEPARATELY so a later partial
  // reload of one region cannot hide an earlier faulted load of the other.
  uint64_t last_load_submission = 0;      // base region
  uint64_t last_mip_load_submission = 0;  // mip chain

  // Hash revalidation state (see MetalSharedTextureCache::RevalidateRegions).
  uint32_t loads = 0;            // successful loads of any region
  uint64_t base_hash = 0;        // XXH3 of the guest base bytes at last load
  uint64_t mips_hash = 0;        // XXH3 of the guest mip bytes at last load
  uint32_t hash_changed_streak = 0;  // consecutive reloads whose hash moved

  // Views of this texture in bindless slots, one per (host swizzle,
  // signedness), kept for the texture's lifetime. Re-creating a view and slot
  // each time a fetch constant switched textures cost Skate 2 ~17 ms of CP
  // time a frame on the AMD driver (view init/dealloc, residency, retires).
  struct CachedView {
    uint32_t host_swizzle;
    bool is_signed;
    void* view;     // id<MTLTexture>, retained.
    uint32_t slot;  // Legacy bindless slot holding `view`.
  };
  std::vector<CachedView> views;

 private:
  void* mtl_texture_;  // id<MTLTexture>, 2D base, retained.
};

MetalSharedTextureCache::MetalSharedTextureCache(
    const RegisterFile& register_file, const ::rex::gpu::XenosSettings& settings,
    MetalSharedMemory& shared_memory_, void* device, MetalTextureCache* legacy_adapter,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y)
    : TextureCache(register_file, settings, shared_memory_, draw_resolution_scale_x,
                   draw_resolution_scale_y),
      device_(device),
      legacy_(legacy_adapter) {}

std::string MetalSharedTextureCache::DescribeRecentLoads() const {
  std::string out;
  const uint32_t count = std::min<uint32_t>(recent_load_next_, 8u);
  for (uint32_t i = 0; i < count; ++i) {
    const RecentLoad& load =
        recent_loads_[(recent_load_next_ - count + i) % 8];
    out += fmt::format("{}{}{} {:08X} {}x{} L{} s{}", i ? ", " : "",
                       load.direct ? "direct" : "family",
                       load.side ? "/side" : "", load.base_page << 12,
                       load.width, load.height, load.levels, load.submission);
  }
  return out.empty() ? std::string("(none)") : out;
}

MetalSharedTextureCache::~MetalSharedTextureCache() {
  for (BindingSlot& binding_slot : binding_slots_) {
    ReleaseBindingSlot(binding_slot);
  }
  // Destroy all textures HERE, while the derived object is still alive:
  // ~MetalTexture casts texture_cache() back to MetalSharedTextureCache and
  // reads legacy_, which is undefined once the base destructor runs (the
  // base header documents exactly this pattern). The base destructor then
  // clears an empty map.
  DestroyAllTextures(true);
  family_pipelines_.Shutdown();
  for (ScratchBuffer& sb : scratch_) {
    if (sb.buffer) {
      id<MTLBuffer> released = (__bridge_transfer id<MTLBuffer>)sb.buffer;
      (void)released;
    }
  }
  scratch_.clear();
  if (direct_pipeline_) {
    id<MTLComputePipelineState> released =
        (__bridge_transfer id<MTLComputePipelineState>)direct_pipeline_;
    (void)released;
    direct_pipeline_ = nullptr;
  }
}

bool MetalSharedTextureCache::Initialize() {
  // XE_METAL_PACKED16_FALLBACK=1 takes the non-Apple routes on Apple GPUs
  // too (A/B of the expansions against the packed formats).
  SharedTexturePacked16Supported().store(
      [(__bridge id<MTLDevice>)device_ supportsFamily:MTLGPUFamilyApple1] &&
          !std::getenv("XE_METAL_PACKED16_FALLBACK"),
      std::memory_order_relaxed);
  SharedTextureBcSupported().store(
      [(__bridge id<MTLDevice>)device_ supportsBCTextureCompression],
      std::memory_order_relaxed);
  SizeHostMemoryLimitsForDevice();
  direct_pipeline_ = MetalLoadShaderPipelines::CreateDirectRgba8Pipeline(device_);
  if (!direct_pipeline_) {
    return false;
  }
  if (SharedTextureRouteFormatsWidened()) {
    if (!family_pipelines_.Initialize(device_)) {
      REXLOG_WARN(
          "MetalSharedTextureCache: texture-load family pipelines failed to "
          "build; only the RGBA8 direct class is routed");
    }
  }
  return true;
}

void MetalSharedTextureCache::SizeHostMemoryLimitsForDevice() {
  // The GetTextureCache()memory_limit_* defaults (384 / 768 MB) are sized for
  // the smallest GPUs the D3D12 and Vulkan backends run on. Dedicated-VRAM
  // Macs have at least 4 GB (the floor is an RX 580 class card), so there
  // the limits follow the device's working set: an eighth soft and a
  // quarter hard, never below the defaults, at most 2 / 4 GB. 4 GB card:
  // 512 / 1024 MB; 8 GB (RX 580, Radeon Pro 580X): 1 / 2 GB; 16 GB (RX 6900
  // XT): 2 / 4 GB. Settings changed from the defaults are kept, and Apple
  // GPUs, which share system memory, keep the defaults.
  // XE_METAL_TEXTURE_LIMITS_DEFAULT=1 keeps the defaults on any GPU.
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  if (!device || device.hasUnifiedMemory ||
      [device supportsFamily:MTLGPUFamilyApple7] ||
      std::getenv("XE_METAL_TEXTURE_LIMITS_DEFAULT")) {
    return;
  }
  constexpr uint32_t kDefaultSoftMB = 384, kDefaultHardMB = 768;
  const uint64_t working_set_mb =
      uint64_t(device.recommendedMaxWorkingSetSize) >> 20;
  const uint32_t soft_mb = uint32_t(std::clamp<uint64_t>(
      working_set_mb / 8, kDefaultSoftMB, 2048));
  const uint32_t hard_mb = uint32_t(std::clamp<uint64_t>(
      working_set_mb / 4, kDefaultHardMB, 4096));
  const bool soft_default =
      GetMetalSettings().texture_cache_memory_limit_soft.Get() == kDefaultSoftMB;
  const bool hard_default =
      GetMetalSettings().texture_cache_memory_limit_hard.Get() == kDefaultHardMB;
  SetHostMemoryLimitsMB(soft_default ? soft_mb : 0,
                        hard_default ? hard_mb : 0);
  REXLOG_INFO("MetalSharedTextureCache: dedicated VRAM ({} MiB working set), "
         "texture memory limits {} / {} MB",
         working_set_mb,
         soft_default ? soft_mb : GetMetalSettings().texture_cache_memory_limit_soft.Get(),
         hard_default ? hard_mb : GetMetalSettings().texture_cache_memory_limit_hard.Get());
}

bool MetalSharedTextureCache::PlanLevels(const Texture& texture, bool load_base,
                                         bool load_mips,
                                         std::vector<MipLevelPlan>* out) {
  TextureKey key = texture.GetKey();
  MipLevelPlanInputs in;
  in.base_address = uint32_t(key.base_page) << 12;
  in.mip_address = uint32_t(key.mip_page) << 12;
  in.width = key.GetWidth();
  in.height = key.GetHeight();
  in.mip_max_level = key.mip_max_level;
  in.array_size = key.depth_or_array_size_minus_1 + 1;
  in.is_3d = key.dimension == xenos::DataDimension::k3D;
  in.is_1d = key.dimension == xenos::DataDimension::k1D;
  in.guest_buffer_size = ::rex::gpu::SharedMemory::kBufferSize;
  in.load_base = load_base;
  in.load_mips = load_mips;
  if (const SharedTextureFormatEntry* entry =
          FindSharedTextureFormat(key.format)) {
    in.bytes_per_block = entry->bytes_per_block;
    in.block_dim = SharedTextureBlockDim(*entry);
    in.block_width = SharedTextureBlockWidth(*entry);
  }
  // The single planning authority (metal_mip_level_plan.h), shared with
  // conformance so production and tests can never diverge. The memo caches
  // only the per-shape range scan, so it changes cost, never the plan.
  if (!BuildMipLevelPlan(in, texture.GetGuestLayout(), out, &range_memo_)) {
    return false;
  }
  if (!key.tiled && key.dimension != xenos::DataDimension::k3D) {
    // Linear layout: the planner's envelope is the tiled walk's; a linear
    // level reads exactly block rows x pitch from its offset (the packed
    // origin's rows included). Same conservative role: a surplus only adds a
    // flush.
    const SharedTextureFormatEntry* entry = FindSharedTextureFormat(key.format);
    const uint32_t block_dim = entry ? SharedTextureBlockHeight(*entry) : 1u;
    for (MipLevelPlan& p : *out) {
      const uint64_t rows =
          (uint64_t(p.py) + p.height + block_dim - 1u) / block_dim;
      const uint64_t bytes = rows * uint64_t(p.pitch_blocks) * 4u;
      const uint64_t end = std::min<uint64_t>(uint64_t(p.guest_offset) + bytes,
                                              ::rex::gpu::SharedMemory::kBufferSize);
      p.consumed_begin = p.guest_offset;
      p.consumed_end = uint32_t(std::max<uint64_t>(end, p.guest_offset));
    }
  }
  return true;
}

std::unique_ptr<TextureCache::Texture> MetalSharedTextureCache::CreateTexture(
    TextureKey key) {
  // Strict routed-class guard (C1T-R): anything else is unsupported here and
  // falls back to the legacy cache. Mirrors IsStrictRgba8Eligible at key
  // granularity, plus pitch != 0 (the key carries the raw fetch pitch; the
  // legacy align32(width) default is not representable) and the mip_page
  // agreement checked below.
  const SharedTextureFormatEntry* entry = FindSharedTextureFormat(key.format);
  if (!entry || (!entry->direct && !family_pipelines_.initialized())) {
    return nullptr;
  }
  const bool cube = key.dimension == xenos::DataDimension::kCube &&
                    SharedRouteCubeEnabled() &&
                    key.GetWidth() == key.GetHeight() &&
                    key.depth_or_array_size_minus_1 == 5;
  // Volume textures (XE_METAL_TEX_ROUTE_3D): a 3D host texture loaded by the
  // load shaders' 3D path (Tomb Raider's per-frame 32x32x32 colour LUT, whose
  // draw was a RAM consumer of its 32 slice resolves: a GPU wait a frame).
  const bool volume = key.dimension == xenos::DataDimension::k3D &&
                      SharedRouteVolumeEnabled() && key.tiled &&
                      key.depth_or_array_size_minus_1 + 1 <= 2048;
  // 1D (XE_METAL_TEX_ROUTE_1D): one linear row per level, created as a
  // width x 1 2D texture and served through the same 2D-array view as 2D
  // textures (the translator samples 1D fetches as 2D arrays). The row pitch
  // addresses nothing for a single row, so a zero fetch pitch is accepted.
  const bool one_d = key.dimension == xenos::DataDimension::k1D &&
                     SharedRoute1DEnabled() && !key.tiled &&
                     !key.packed_mips;
  if ((key.dimension != xenos::DataDimension::k2DOrStacked && !cube &&
       !volume && !one_d) ||
      (!key.tiled && !SharedRouteLinearEnabled()) ||
      key.signed_separate || key.scaled_resolve ||
      (key.pitch == 0 && !one_d) ||
      (key.depth_or_array_size_minus_1 != 0 && !SharedRouteStackedEnabled())) {
    return nullptr;
  }
  const uint32_t array_size = key.depth_or_array_size_minus_1 + 1;
  // The strict MIPPED sibling is served whenever the chain has its own guest
  // pointer; the single-mip class stays exactly base-only. Each class refuses
  // the other's mip_page shape, so they remain mutually exclusive.
  //
  // A mips-only key (base_page 0: the fetch's minimum level is above 0, so
  // the base cache zeroes the base page and the guest layout has no base
  // extent) is the D3D12/Vulkan contract for such fetches: levels 1..max load
  // from the mip chain, level 0 of the host texture is never written and
  // never sampled (the sampler's lodMinClamp is the fetch's minimum level,
  // and base-map pins to it). Red Dead Redemption fetches mips 1 and 2 of
  // its resolve-backed 4:4:4:4 pyramids alone (2026-10-04;
  // XE_METAL_TEX_ROUTE_MINMIP=0 keeps such fetches off the route).
  bool mipped = key.mip_max_level != 0;
  if (mipped) {
    if (key.mip_page == 0) {
      return nullptr;
    }
    if (key.base_page == 0 && !SharedRouteMinMipEnabled()) {
      return nullptr;
    }
  } else if (key.mip_page != 0 || key.base_page == 0) {
    return nullptr;
  }
  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormat(entry->host_format)
                                   width:width
                                  height:height
                               mipmapped:mipped ? YES : NO];
  if (cube) {
    // Six faces as the slices of a one-element cube array (the loader blits
    // per slice, exactly as for stacked textures).
    desc.textureType = MTLTextureTypeCubeArray;
    desc.arrayLength = 1;
  } else if (volume) {
    desc.textureType = MTLTextureType3D;
    desc.depth = array_size;
  } else if (array_size > 1) {
    desc.textureType = MTLTextureType2DArray;
    desc.arrayLength = array_size;
  }
  desc.storageMode = MTLStorageModePrivate;
  // PixelFormatView: the signed serve creates an RGBA8Snorm view of this
  // Unorm texture — a FORMAT-CASTING view, which Metal only defines when the
  // base texture declares this usage. Without it the cast view's sampling is
  // formally undefined (descriptor A/B for the 10395 corner artifact).
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
  if (!SharedTextureHostIsBc(*entry)) {
    // Compressed formats cannot be shader-written (they are blit-filled from
    // the loader's scratch rows); the direct pipeline writes RGBA8 levels.
    desc.usage |= MTLTextureUsageShaderWrite;
  }
  if (mipped) {
    // Level count from the shared layout's CLAMPED max_level, and the key must
    // agree with it -- the planner cross-checks the same pair, so a descriptor
    // and a plan can never disagree about how many levels exist.
    ::rex::xenos::GuestTextureLayout layout = key.GetGuestLayout();
    if (layout.max_level != key.mip_max_level) {
      return nullptr;
    }
    desc.mipmapLevelCount = layout.max_level + 1;
    // The load writes through level-scoped views; the G0 foundation fixture
    // establishes that this usage bit is part of that contract.
    desc.usage |= MTLTextureUsagePixelFormatView;
  }
  // Discrete AMD GPUs: from the legacy cache's texture heaps (one useHeaps
  // per pass instead of a per-draw declaration). Release is through the
  // legacy retire pool (~MetalTexture), which waits for the submissions.
  id<MTLTexture> texture = nil;
  if (legacy_ && legacy_->texture_heaps_enabled()) {
    texture = (__bridge_transfer id<MTLTexture>)legacy_->NewHeapTexture(
        (__bridge void*)desc);
  }
  if (!texture) {
    texture = [device newTextureWithDescriptor:desc];
  }
  if (!texture) {
    return nullptr;
  }
  return std::unique_ptr<Texture>(
      new MetalTexture(*this, key, (__bridge_retained void*)texture));
}

namespace {
bool SideLoadEnabled();  // defined below (XE_METAL_TEX_SIDE_LOAD)
// Bounded per-reason log of shared-route load declines (the prepass then
// reports the fetch as UNSERVED and drains it for the legacy path).
template <typename Key>  // TextureCache::TextureKey (protected; deduced)
void NoteDecline(const char* reason, const Key& key) {
  static std::unordered_map<std::string, uint32_t> s_counts;
  static std::mutex s_mutex;
  std::lock_guard<std::mutex> lock(s_mutex);
  uint32_t n = ++s_counts[reason];
  if (n <= 4 || (n & 1023) == 0) {
    REXLOG_WARN("MetalSharedTex DECLINE {} #{} fmt={} base={:08X} {}x{} mips={} "
           "mip_base={:08X}",
           reason, n, uint32_t(key.format), uint32_t(key.base_page) << 12,
           key.GetWidth(), key.GetHeight(), key.mip_max_level,
           uint32_t(key.mip_page) << 12);
  }
}
}  // namespace

bool MetalSharedTextureCache::LoadTextureDataFromResidentMemoryBackend(
    Texture& texture, bool load_base, bool load_mips) {
  ++serve_generation_;
  MetalTexture& metal_texture = static_cast<MetalTexture&>(texture);
  TextureKey key = texture.GetKey();
  bool mipped = key.mip_max_level != 0;
  if (!mipped) {
    // The promoted single-mip class: base-only by CreateTexture's guard.
    if (!load_base || load_mips) {
      NoteDecline("single-mip-load-shape", key);
      return false;
    }
  }
  if (!load_base && !load_mips) {
    NoteDecline("no-levels", key);
    return false;
  }

  // ---- Content guard, the equivalent of the legacy path's: captures never
  // record mip memory, so a trace replay restores all-zero mip pages, and
  // loading those blacks out every minified surface. The legacy cache probes
  // the first 4 KiB at mip_address and serves base-only when it is entirely
  // zero; the routed path must apply the SAME rule or the two diverge on
  // replays and on any live all-zero mip region. Direct replay non-engagement
  // telemetry is evidence, not a substitute -- older or future traces could
  // engage. New-capture mip recording is unaffected (that is the warm sweep,
  // which only records regions already up to date).
  static const bool s_zero_guard_live =
      std::getenv("XE_METAL_MIP_ZERO_GUARD_LIVE") != nullptr;
  if (mipped && load_mips && (trace_playback_ || s_zero_guard_live)) {
    uint32_t mip_addr = uint32_t(key.mip_page) << 12;
    // Bound FIRST: the subtraction below underflows for an out-of-range
    // address, which would produce an enormous probe length.
    if (mip_addr >= ::rex::gpu::SharedMemory::kBufferSize) {
      NoteDecline("mip-address-out-of-range", key);
      return false;
    }
    const uint32_t* probe = static_cast<MetalSharedMemory&>(GetSharedMemory())
                                .TranslateGuestAddress<const uint32_t*>(mip_addr);
    if (!probe) {
      NoteDecline("mip-probe-untranslatable", key);
      return false;
    }
    uint32_t probe_words = std::min<uint32_t>(
        1024, (::rex::gpu::SharedMemory::kBufferSize - mip_addr) / 4);
    bool any_nonzero = false;
    for (uint32_t i = 0; i < probe_words; ++i) {
      if (probe[i]) {
        any_nonzero = true;
        break;
      }
    }
    if (!any_nonzero) {
      // Zero mip region: decline the whole load so the legacy path applies its
      // own guard and serves the base level only. Atomic fallback.
      NoteDecline("zero-mip-region", key);
      return false;
    }
  }

  if (ResolveForwardTraced(uint32_t(key.base_page) << 12)) {
    REXLOG_INFO("MetalResolveForward LOAD base={:08X} fmt={} {}x{} loads={} "
           "gpu_written={}",
           uint32_t(key.base_page) << 12, uint32_t(key.format), key.GetWidth(),
           key.GetHeight(), metal_texture.loads,
           static_cast<MetalSharedMemory&>(GetSharedMemory())
                   .AnyPageGpuWritten(uint32_t(key.base_page) << 12,
                                      texture.GetGuestBaseSize())
               ? 1
               : 0);
  }
  // ---- Reload of an already-loaded texture: when the guest bytes of every
  // region to load still hash to what the last load read, there is nothing
  // to load (and no render pass to end). First loads go straight to the load.
  pending_base_hash_ = 0;
  pending_mips_hash_ = 0;
  if (metal_texture.loads) {
    ++census_reloads_;
    if (RevalidateRegions(metal_texture, load_base,
                          uint32_t(key.base_page) << 12,
                          texture.GetGuestBaseSize(), load_mips,
                          uint32_t(key.mip_page) << 12,
                          texture.GetGuestMipsSize())) {
      return true;
    }
  }

  // ---- Plan EVERY requested level and validate EVERY consumed range BEFORE
  // any encoding, so an unsatisfiable plan falls back to the legacy cache
  // atomically instead of half-loading a texture. ----
  std::vector<MipLevelPlan> plan;
  if (!PlanLevels(texture, load_base, load_mips, &plan)) {
    NoteDecline("plan-failed", key);
    return false;
  }
  auto& metal_shared_memory = static_cast<MetalSharedMemory&>(GetSharedMemory());
  for (const MipLevelPlan& p : plan) {
    // Register each exact read range as CONSUMED by the open frame so a later
    // same-frame rewrite triggers the existing hazard flush.
    //
    // ATOMICITY, precisely: if a later request fails, earlier ones stay
    // registered and we return before ANY encoding, so no texture content is
    // half-written and the legacy path serves the whole texture. A surplus
    // consumed range is conservative by construction -- it can only cause an
    // extra hazard flush, never a missed one -- so the guarantee is "no partial
    // GPU work", not "no partial bookkeeping".
    if (!metal_shared_memory.RequestConsumedRange(
            p.consumed_begin, p.consumed_end - p.consumed_begin)) {
      NoteDecline("consumed-range-refused", key);
      return false;
    }
  }
  const SharedTextureFormatEntry* entry = FindSharedTextureFormat(key.format);
  if (!entry) {
    NoteDecline("format-not-in-table", key);
    return false;
  }
  if (!entry->direct) {
    // The planner's exact range scan is the 32bpp tiled walk; for the other
    // block sizes register the conservative whole tiled extent of each level
    // (32-row aligned) as consumed. A superset can only add a hazard flush.
    const uint32_t block_dim = SharedTextureBlockHeight(*entry);
    for (const MipLevelPlan& p : plan) {
      if (p.depth > 1) {
        continue;  // 3D: the plan's range is already the whole level
      }
      uint32_t pitch_blocks = (p.pitch_blocks * 4u) / entry->bytes_per_block;
      uint32_t block_rows =
          (p.py + p.height + block_dim - 1u) / block_dim;
      uint64_t rows = uint64_t((block_rows + 31u) & ~31u);
      uint64_t bytes = rows * pitch_blocks * entry->bytes_per_block;
      uint64_t end = std::min<uint64_t>(uint64_t(p.guest_offset) + bytes,
                                        ::rex::gpu::SharedMemory::kBufferSize);
      if (end <= p.guest_offset ||
          !metal_shared_memory.RequestConsumedRange(
              p.guest_offset, uint32_t(end - p.guest_offset))) {
        NoteDecline("family-consumed-range-refused", key);
        return false;
      }
    }
  }

  // ---- Side command buffer: a FIRST load of a texture whose source pages
  // are CPU-written (not GPU-written, no staged hazard blit pending) has no
  // ordering need against the open frame command buffer -- nothing in it
  // reads this texture yet, and the bytes are already in the shared buffer
  // -- so it is encoded into the submission's side command buffer, committed
  // just ahead of the frame one (tracked resources order the draws after it)
  // instead of ending the render pass (~40 MB of EDRAM-height attachment
  // traffic per resume; GTA IV paid 31 such ends a frame). Reloads stay in
  // the frame buffer: earlier draws of this submission sample the old content.
  // XE_METAL_TEX_SIDE_LOAD=0 disables.
  side_load_ = false;
  if (SideLoadEnabled() && render_pass_open_ && render_pass_open_()) {
    const uint64_t open_submission =
        submission_provider_ ? submission_provider_() : 0;
    // A reload qualifies too when no draw of the open submission has sampled
    // the texture yet (its last use is an earlier, already-committed
    // submission that the side buffer orders after): the common streaming
    // case, a texture invalidated by the guest's writes and reloaded before
    // its first draw of the frame.
    bool eligible =
        open_submission != 0 &&
        (metal_texture.loads == 0 ||
         metal_texture.GetLastUsageSubmissionIndex() < open_submission);
    auto clean = [&](uint32_t begin, uint32_t len) {
      return len != 0 &&
             !metal_shared_memory.AnyPageGpuWritten(begin, len) &&
             // A memexport is not marked GPU-written at encode (only at its
             // drain), so ask the pending-producer index as well.
             !metal_shared_memory.AnyPendingGpuProducerSpan(begin, len) &&
             !metal_shared_memory.AnyStagedUploadPending(begin, len,
                                                         open_submission);
    };
    const uint32_t block_dim = SharedTextureBlockHeight(*entry);
    for (const MipLevelPlan& p : plan) {
      if (!eligible) {
        break;
      }
      if (!clean(p.consumed_begin, p.consumed_end - p.consumed_begin)) {
        eligible = false;
        break;
      }
      if (!entry->direct) {
        // The family loaders read the whole tiled extent (see the consumed
        // registration above); check the same superset.
        uint32_t pitch_blocks = (p.pitch_blocks * 4u) / entry->bytes_per_block;
        uint32_t block_rows = (p.py + p.height + block_dim - 1u) / block_dim;
        uint64_t rows = uint64_t((block_rows + 31u) & ~31u);
        uint64_t bytes = rows * pitch_blocks * entry->bytes_per_block;
        uint64_t end = std::min<uint64_t>(uint64_t(p.guest_offset) + bytes,
                                          ::rex::gpu::SharedMemory::kBufferSize);
        if (end <= p.guest_offset ||
            !clean(p.guest_offset, uint32_t(end - p.guest_offset))) {
          eligible = false;
        }
      }
    }
    // XE_METAL_TEX_SIDE_LOAD=2 (diagnostic only; reloads of a texture an
    // earlier draw of this submission sampled then race it): every load takes
    // the side buffer, so no load ends a render pass.
    static const bool s_force_side = [] {
      const char* v = std::getenv("XE_METAL_TEX_SIDE_LOAD");
      return v && v[0] == '2' && !v[1];
    }();
    if (eligible || s_force_side) {
      side_load_ = true;
      ++census_side_loads_;
    }
  }

  // XE_METAL_RGBA4_EXPAND_MIPS=0: the RGBA4 -> RGBA8 expansion serves level 0
  // only and the legacy decode takes mipped keys, atomically (nothing is
  // encoded yet).
  if (entry->load_shader == SharedLoadFamily::k16bpbRgba4ToRgba8 &&
      plan.size() > 1 && !SharedRgba4ExpandMipsEnabled()) {
    NoteDecline("expanding-family-mips", key);
    return false;
  }
  // The direct pipeline writes single-slice level views; stacked textures
  // take the family loader of the same format (bit copy for RGBA8). So do
  // linear ones: the direct shader and its exact range scan are tiled-only.
  if (entry->direct && key.depth_or_array_size_minus_1 == 0 && key.tiled) {
    if (!LoadLevelsDirect(texture, plan, mipped)) {
      NoteDecline("direct-load-failed", key);
      return false;
    }
  } else if (!LoadLevelsFamily(texture, *entry, plan)) {
    NoteDecline("family-load-failed", key);
    return false;
  }

  uint64_t submission = submission_provider_ ? submission_provider_() : 0;
  // RevalidateRegions hashed RAM after the RequestRange that filled the
  // buffer this load read. A guest write in between invalidated pages and
  // landed in the hash but not in the texture; adopting that hash would let
  // the next reload match it and keep the old bytes for good. Adopt only
  // while every page is still valid; a hash of 0 makes the next reload load.
  if (pending_base_hash_ || pending_mips_hash_) {
    auto global_lock = rex::thread::global_critical_region::AcquireDirect();
    const uint32_t base = uint32_t(key.base_page) << 12;
    const uint32_t mips = uint32_t(key.mip_page) << 12;
    if (pending_base_hash_ &&
        !(uint64_t(base) + uint64_t(base + texture.GetGuestBaseSize() <= ::rex::gpu::SharedMemory::kBufferSize))) {
      pending_base_hash_ = 0;
    }
    if (pending_mips_hash_ &&
        !(uint64_t(mips) + uint64_t(mips + texture.GetGuestMipsSize() <= ::rex::gpu::SharedMemory::kBufferSize))) {
      pending_mips_hash_ = 0;
    }
  }
  if (load_base) {
    metal_texture.last_load_submission = submission;
    // The hash of what this load read, for the next reload's revalidation:
    // RevalidateRegions computed it from the same bytes just before the load
    // and parked it here; it is adopted only now that the load succeeded, so
    // a declined load can never leave a hash claiming content the texture
    // does not hold. A first load is never hashed.
    metal_texture.base_hash = pending_base_hash_;
  }
  if (load_mips) {
    metal_texture.last_mip_load_submission = submission;
    metal_texture.mips_hash = pending_mips_hash_;
  }
  pending_base_hash_ = 0;
  pending_mips_hash_ = 0;
  ++metal_texture.loads;
  // Load census (reported per swap by BeginFrame).
  if (load_base) {
    ++census_loads_base_;
    census_load_bytes_ += texture.GetGuestBaseSize();
  }
  if (load_mips) {
    ++census_loads_mips_;
    census_load_bytes_ += texture.GetGuestMipsSize();
  }
  if (census_loads_by_format_.size() < 64) {
    ++census_loads_by_format_[uint32_t(key.format) |
                             (key.tiled ? 0u : 0x100u)];
  }
  // Keep the texture out of LRU destruction while its load is in flight
  // (completion actions can pump mid-frame on upload-ring pressure).
  texture.MarkAsUsed();

  // No trace recording here (M5 revised by gate-C evidence): the route
  // disengages while a capture is open, so a load never encodes during a
  // captured frame and the legacy path records the CPU-oracle read-set.
  return true;
}

bool MetalSharedTextureCache::LoadLevelsDirect(
    Texture& texture, const std::vector<MipLevelPlan>& plan, bool mipped) {
  MetalTexture& metal_texture = static_cast<MetalTexture&>(texture);
  TextureKey key = texture.GetKey();
  auto& metal_shared_memory = static_cast<MetalSharedMemory&>(GetSharedMemory());
  // ---- Pre-create and validate EVERY level view BEFORE opening the encoder.
  // A late newTextureView failure must not leave earlier dispatches encoded, so
  // view creation cannot be interleaved with dispatching. ----
  id<MTLTexture> base_texture =
      (__bridge id<MTLTexture>)metal_texture.mtl_texture();
  if (!base_texture) {
    return false;
  }
  // RAII: every early exit after pre-creation releases the retained views
  // exactly once. `release_on_scope_exit` is cleared only after the views have
  // been handed to the submission-gated retirement pool, so a view is never
  // both retired and released.
  std::vector<void*> level_views;  // retained id<MTLTexture>, parallel to plan
  bool release_on_scope_exit = true;
  struct ViewGuard {
    std::vector<void*>* views;
    bool* armed;
    ~ViewGuard() {
      if (!*armed) {
        return;
      }
      for (void* v : *views) {
        id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)v;
        (void)released;
      }
      views->clear();
    }
  } view_guard{&level_views, &release_on_scope_exit};
  bool views_ok = true;
  if (mipped) {
    level_views.reserve(plan.size());
    for (const MipLevelPlan& p : plan) {
      id<MTLTexture> view = [base_texture
          newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                            textureType:MTLTextureType2D
                                 levels:NSMakeRange(p.host_level, 1)
                                 slices:NSMakeRange(0, 1)];
      if (!view) {
        views_ok = false;
        break;
      }
      level_views.push_back((__bridge_retained void*)view);
    }
    if (!views_ok) {
      // The guard releases what we made; nothing was encoded, so the legacy
      // path serves the whole texture.
      return false;
    }
  }

  // ---- ONE PrepareTransferCommandBuffer per texture load; ALL levels batched
  // as dispatches on that existing frame command buffer. ----
  void* command_buffer_ptr = prepare_transfer_ ? prepare_transfer_(side_load_) : nullptr;
  if (!command_buffer_ptr || !direct_pipeline_) {
    return false;
  }
  id<MTLCommandBuffer> command_buffer =
      (__bridge id<MTLCommandBuffer>)command_buffer_ptr;
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (!encoder) {
    return false;
  }
  encoder.label = mipped ? @"shared-texcache direct load (mip chain)"
                         : @"shared-texcache direct load";
  NoteRecentLoad(RecentLoad{uint32_t(key.base_page), key.GetWidth(),
                            key.GetHeight(), uint32_t(plan.size()),
                            submission_provider_ ? submission_provider_() : 0,
                            side_load_, true});
  // XE_METAL_ROUTE_LOAD_LOG=1: every direct load in encode order (bounded),
  // to interleave with the scatter / draw logs when bisecting staleness.
  static const bool s_load_log = std::getenv("XE_METAL_ROUTE_LOAD_LOG") != nullptr;
  if (s_load_log) {
    static uint32_t s_load_logs = 0;
    if (s_load_logs++ < 400) {
      REXLOG_INFO("MetalRouteLoad #{} base={:08X} {}x{} levels={} submission={}",
             s_load_logs, uint32_t(key.base_page) << 12, key.GetWidth(),
             key.GetHeight(), plan.size(),
             submission_provider_ ? submission_provider_() : 0);
    }
  }
  [encoder setComputePipelineState:(__bridge id<MTLComputePipelineState>)
                                       direct_pipeline_];
  [encoder setBuffer:(__bridge id<MTLBuffer>)metal_shared_memory.GetBuffer()
              offset:0
             atIndex:MetalLoadShaderPipelines::kBufferIndexSource];
  MTLSize group = MTLSizeMake(MetalLoadShaderPipelines::kThreadgroupSizeX,
                              MetalLoadShaderPipelines::kThreadgroupSizeY, 1);
  for (size_t i = 0; i < plan.size(); ++i) {
    const MipLevelPlan& p = plan[i];
    // Ephemeral level-scoped write view, pre-created above: level N of the base
    // aliased as a single-level image so the unchanged image_store primitive
    // can target it. Deliberately NOT cached -- a per-level view cache would be
    // new lifetime state and a fresh instance of the stale-reference class.
    // A non-mipped texture needs no view at all.
    id<MTLTexture> dest =
        mipped ? (__bridge id<MTLTexture>)level_views[i] : base_texture;
    MetalLoadShaderPipelines::LoadConstants constants = {};
    constants.is_tiled_3d_endian_scale = 1u | (uint32_t(key.endianness) << 2);
    constants.guest_offset = p.guest_offset;
    constants.guest_pitch_aligned = p.pitch_blocks;
    constants.size_blocks[0] = p.width;
    constants.size_blocks[1] = p.height;
    constants.size_blocks[2] = 1;
    // The packed sub-tile origin rides host_offset (px low 16 / py high 16) --
    // the production-zero field the 6.1-G0 foundation proved. Zero for base and
    // non-packed levels, keeping those byte-identical to the promoted route.
    constants.host_offset = (p.px & 0xFFFFu) | ((p.py & 0xFFFFu) << 16);
    constants.host_pitch = ((p.width * 4u) + 255u) & ~255u;
    constants.height_texels = p.height;
    [encoder setBytes:&constants
               length:sizeof(constants)
              atIndex:MetalLoadShaderPipelines::kBufferIndexPushConstants];
    [encoder setTexture:dest atIndex:1];
    uint32_t threads_x = (p.width + 7u) / 8u;  // 1 thread = 8 texels along X.
    [encoder dispatchThreadgroups:MTLSizeMake(
                                      (threads_x + group.width - 1) /
                                          group.width,
                                      (p.height + group.height - 1) /
                                          group.height,
                                      1)
            threadsPerThreadgroup:group];
  }
  [encoder endEncoding];

  // The GPU reads each view for the life of this submission; retire them all
  // through the single submission-gated legacy pool, then DISARM the guard so
  // ownership transfers exactly once.
  release_on_scope_exit = false;
  for (void* v : level_views) {
    if (legacy_) {
      legacy_->RetireExternalTexture(v);
    } else {
      id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)v;
      (void)released;
    }
  }
  level_views.clear();
  return true;
}

namespace {
bool SharedRevalidateEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_SHARED_REVALIDATE");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}
bool ServeStaleEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_SHARED_SERVE_STALE");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}
bool SideLoadEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_SIDE_LOAD");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}
}  // namespace

bool MetalSharedTextureCache::ServedByCurrentSubmission(
    const MetalTexture& texture, uint64_t submission) {
  if (!submission) {
    return false;
  }
  if ((texture.GetOutdatedMask() & texture.kOutdatedBitBase) != 0 &&
      texture.last_load_submission != submission) {
    return false;
  }
  if ((texture.GetOutdatedMask() & texture.kOutdatedBitMips) != 0 &&
      texture.last_mip_load_submission != submission) {
    return false;
  }
  return true;
}

uint64_t MetalSharedTextureCache::HashGuestBase(uint32_t base,
                                                uint32_t bytes) const {
  if (!bytes || base >= ::rex::gpu::SharedMemory::kBufferSize) {
    return 0;
  }
  const uint8_t* guest = static_cast<MetalSharedMemory&>(GetSharedMemory())
                             .TranslateGuestAddress<const uint8_t*>(base);
  if (!guest) {
    return 0;
  }
  uint64_t hash = XXH3_64bits(
      guest, std::min<size_t>(bytes, ::rex::gpu::SharedMemory::kBufferSize - base));
  // 0 is the "no hash" sentinel.
  return hash ? hash : 1;
}

bool MetalSharedTextureCache::RevalidateRegions(
    MetalTexture& texture, bool load_base, uint32_t base, uint32_t base_bytes,
    bool load_mips, uint32_t mips, uint32_t mips_bytes) {
  if (!SharedRevalidateEnabled()) {
    return false;
  }
  const uint64_t total = (load_base ? uint64_t(base_bytes) : 0) +
                         (load_mips ? uint64_t(mips_bytes) : 0);
  if (!total || total > kRevalidateMaxBytes) {
    return false;
  }
  if (load_base && !base_bytes) {
    return false;
  }
  if (load_mips && !mips_bytes) {
    return false;
  }
  // A texture whose content really changes on every reload (a per-frame
  // resolve destination read back through RAM, a video surface) must not pay
  // a hash per reload forever: after four consecutive changes the hash is
  // taken only every 64th reload, which is enough to notice it going static.
  if (texture.hash_changed_streak >= 4 && (texture.loads & 63u) != 0) {
    return false;
  }
  // GPU-written pages: the buffer holds newer bytes than RAM (a resolve or
  // memexport landed there and was not published), so a RAM hash says
  // nothing about what the load would read. Reload.
  auto& metal_shared_memory = static_cast<MetalSharedMemory&>(GetSharedMemory());
  if ((load_base && metal_shared_memory.AnyPageGpuWritten(base, base_bytes)) ||
      (load_mips && metal_shared_memory.AnyPageGpuWritten(mips, mips_bytes))) {
    ++census_revalidate_gpu_skipped_;
    // Attribution probe (frame output is wrong):
    // XE_METAL_PROBE_SKIP_GPU_RELOAD=1 keeps the old content of textures
    // reloaded over GPU-written pages, to size what serving them without a load
    // could save.
    static const bool s_probe_skip =
        std::getenv("XE_METAL_PROBE_SKIP_GPU_RELOAD") != nullptr;
    if (s_probe_skip) {
      return true;
    }
    // XE_METAL_REVAL_LOG=1: which textures reload over GPU-written pages
    // (bounded). A block-compressed texture here means the flag is wider
    // than the resolve that set it, or the title really resolves there.
    static const bool s_log = std::getenv("XE_METAL_REVAL_LOG") != nullptr;
    static uint32_t s_logs = 0;
    // The first 32, then one in 64 (gameplay coverage past the menus).
    if (s_log && (s_logs++ < 32 || (s_logs & 63u) == 0)) {
      const TextureKey key = texture.GetKey();
      REXLOG_INFO(
          "MetalSharedTex GPU-WRITTEN-RELOAD fmt={} base={:08X}+{:X} "
          "mips={:08X}+{:X} {}x{} loads={} base_gpu={} mips_gpu={}",
          uint32_t(key.format), base, base_bytes, mips, mips_bytes,
          key.GetWidth(), key.GetHeight(), texture.loads,
          load_base && metal_shared_memory.AnyPageGpuWritten(base, base_bytes)
              ? 1
              : 0,
          load_mips && metal_shared_memory.AnyPageGpuWritten(mips, mips_bytes)
              ? 1
              : 0);
    }
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t base_now = load_base ? HashGuestBase(base, base_bytes) : 0;
  const uint64_t mips_now = load_mips ? HashGuestBase(mips, mips_bytes) : 0;
  census_revalidate_ns_ += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());
  if ((load_base && !base_now) || (load_mips && !mips_now)) {
    return false;
  }
  const bool base_known = !load_base || texture.base_hash != 0;
  const bool mips_known = !load_mips || texture.mips_hash != 0;
  const bool base_same = !load_base || base_now == texture.base_hash;
  const bool mips_same = !load_mips || mips_now == texture.mips_hash;
  if (base_known && mips_known && base_same && mips_same) {
    texture.hash_changed_streak = 0;
    ++census_revalidated_;
    return true;
  }
  if ((load_base && texture.base_hash && !base_same) ||
      (load_mips && texture.mips_hash && !mips_same)) {
    ++census_revalidate_changed_;
    ++texture.hash_changed_streak;
  }
  // The load that follows reads exactly these bytes; adopt the hashes on its
  // success (see LoadTextureDataFromResidentMemoryBackend).
  pending_base_hash_ = base_now;
  pending_mips_hash_ = mips_now;
  return false;
}

void MetalSharedTextureCache::CompletedSubmissionUpdated(
    uint64_t completed_submission_index) {
  ++serve_generation_;
  TextureCache::CompletedSubmissionUpdated(completed_submission_index);
  if (!legacy_) {
    return;
  }
  const uint32_t capacity = legacy_->slots_capacity();
  if (!capacity) {
    return;
  }
  // What AllocateSlot can hand out now (never allocated + freed); slots
  // retired under in-flight submissions come back at reclaim.
  const uint32_t used = legacy_->slots_used();
  const uint32_t available =
      (capacity > used ? capacity - used : 0) + legacy_->slots_free();
  const uint32_t pending = legacy_->slots_stale();
  if (available + pending >= capacity / 8) {
    return;
  }
  // Below half the heap the legacy cache's own headroom LRU (1/16 low, 3/16
  // target) is the one to give way.
  const uint32_t budget = capacity / 2;
  if (external_slots_held_ <= budget || !live_textures_) {
    return;
  }
  const uint32_t target = capacity * 3 / 8;
  const uint32_t slots_per_texture =
      std::max<uint32_t>(1, external_slots_held_ / live_textures_);
  const uint32_t want =
      (external_slots_held_ - target + slots_per_texture - 1) /
      slots_per_texture;
  const uint32_t held_before = external_slots_held_;
  const uint32_t textures_before = live_textures_;
  const uint32_t destroyed =
      DestroyLeastRecentlyUsedTextures(completed_submission_index, want);
  census_slot_destroyed_ += destroyed;
  static uint32_t s_logs = 0;
  if (s_logs++ < 8 || (s_logs & 1023) == 0) {
    REXLOG_INFO(
        "MetalSharedTex slot budget: heap {} slots, {} available (+{} "
        "pending), this cache held {} in {} textures; destroyed {} of {} "
        "wanted, {} slots held now",
        capacity, available, pending, held_before, textures_before, destroyed,
        want, external_slots_held_);
  }
}

void MetalSharedTextureCache::BeginFrame() {
  TextureCache::BeginFrame();
  if (++census_frames_ < 128) {
    return;
  }
  const double f = double(census_frames_);
  if (census_loads_base_ || census_loads_mips_ || census_revalidated_ ||
      census_forwarded_) {
    std::vector<std::pair<uint32_t, uint32_t>> ranked;
    for (const auto& kv : census_loads_by_format_) {
      ranked.emplace_back(kv.second, kv.first);
    }
    std::sort(ranked.rbegin(), ranked.rend());
    std::string top;
    for (size_t i = 0; i < ranked.size() && i < 6; ++i) {
      top += fmt::format(" fmt{}{}={:.1f}", ranked[i].second & 0xFF,
                         (ranked[i].second & 0x100) ? "L" : "",
                         double(ranked[i].first) / f);
    }
    REXLOG_INFO(
        "MetalSharedTexCensus: loads {:.1f}/frame (base {:.1f}, mips {:.1f}, "
        "reloads {:.1f}, side {:.1f}, {:.2f} MB); destroyed {:.1f}/frame "
        "(slot budget {:.1f}), live {} MB in {} textures, slots {} of {} "
        "(free {}), served-stale {:.1f}/frame; revalidated {:.1f}/frame "
        "(changed {:.1f}, gpu-written {:.1f}) in {:.2f} ms; forwarded "
        "{:.1f}/frame; raced loads {}; top:{}",
        double(census_loads_base_ + census_loads_mips_) / f,
        double(census_loads_base_) / f, double(census_loads_mips_) / f,
        double(census_reloads_) / f, double(census_side_loads_) / f,
        double(census_load_bytes_) / f / (1024.0 * 1024.0),
        double(census_destroyed_) / f, double(census_slot_destroyed_) / f,
        census_host_bytes_ >> 20, live_textures_, external_slots_held_,
        legacy_ ? legacy_->slots_capacity() : 0,
        legacy_ ? (legacy_->slots_capacity() - legacy_->slots_used() +
                   legacy_->slots_free())
                : 0,
        double(census_served_stale_) / f,
        double(census_revalidated_) / f,
        double(census_revalidate_changed_) / f,
        double(census_revalidate_gpu_skipped_) / f,
        double(census_revalidate_ns_) / 1e6 / f,
        double(census_forwarded_) / f, loads_raced_by_guest_write(), top);
  }
  census_destroyed_ = 0;
  census_slot_destroyed_ = 0;
  census_side_loads_ = 0;
  census_forwarded_ = 0;
  census_served_stale_ = 0;
  census_frames_ = 0;
  census_loads_base_ = 0;
  census_loads_mips_ = 0;
  census_reloads_ = 0;
  census_load_bytes_ = 0;
  census_revalidated_ = 0;
  census_revalidate_changed_ = 0;
  census_revalidate_gpu_skipped_ = 0;
  census_revalidate_ns_ = 0;
  census_loads_by_format_.clear();
}

bool MetalSharedTextureCache::AllocateScratch(uint32_t size,
                                              uint32_t alignment,
                                              uint64_t submission,
                                              void** buffer_out,
                                              uint32_t* offset_out) {
  auto align_up = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };
  // 1. Bump inside any buffer with room, stamping it with this submission
  //    (ids are monotonic, so the stamp is the newest user and reuse below
  //    waits for it). Before 2026-10-16 only a buffer already stamped with
  //    THIS submission qualified, so every submission that loaded anything
  //    claimed a whole 32 MB buffer: with 16 submissions in flight the
  //    8-buffer ring was exhausted by kilobyte loads ("scratch-full", Rage:
  //    a 16 MB DXT5 page cache and 1 KB 5:6:5 strips alike fell to the CPU).
  for (ScratchBuffer& sb : scratch_) {
    if (!sb.buffer || sb.submission > submission) {
      continue;
    }
    uint32_t head = align_up(sb.head, alignment);
    if (uint64_t(head) + size <= sb.size) {
      sb.head = head + size;
      sb.submission = submission;
      *buffer_out = sb.buffer;
      *offset_out = head;
      return true;
    }
  }
  // 2. Reuse a buffer whose submission the GPU has finished with. A zero
  //    submission id (S7 tags off) can never be proven complete, so such
  //    buffers are never reused.
  const uint64_t completed =
      completed_submission_provider_ ? completed_submission_provider_() : 0;
  for (ScratchBuffer& sb : scratch_) {
    if (!sb.buffer || sb.submission == submission || sb.submission == 0 ||
        sb.submission > completed || size > sb.size) {
      continue;
    }
    sb.submission = submission;
    sb.head = size;
    *buffer_out = sb.buffer;
    *offset_out = 0;
    return true;
  }
  // 3. Grow the ring, bounded; beyond the bound the caller falls back.
  if (scratch_.size() >= kScratchBufferMax) {
    return false;
  }
  uint32_t buffer_size = std::max(kScratchBufferSize, align_up(size, 1u << 20));
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLBuffer> buffer =
      [device newBufferWithLength:buffer_size
                          options:MTLResourceStorageModePrivate];
  if (!buffer) {
    return false;
  }
  buffer.label = @"shared-texcache load scratch";
  ScratchBuffer sb;
  sb.buffer = (__bridge_retained void*)buffer;
  sb.size = buffer_size;
  sb.head = size;
  sb.submission = submission;
  scratch_.push_back(sb);
  *buffer_out = sb.buffer;
  *offset_out = 0;
  return true;
}

// Buffer-writing families (everything but the direct RGBA8 class): each level
// is untiled by the upstream load shader into a linear scratch region laid
// out as the host texture rows (256-aligned pitch), then blitted into the
// private texture level in the same command buffer. Metal's default hazard
// tracking orders the compute writes before the blit reads.
bool MetalSharedTextureCache::LoadLevelsFamily(
    Texture& texture, const SharedTextureFormatEntry& entry,
    const std::vector<MipLevelPlan>& plan) {
  MetalTexture& metal_texture = static_cast<MetalTexture&>(texture);
  TextureKey key = texture.GetKey();
  auto& metal_shared_memory = static_cast<MetalSharedMemory&>(GetSharedMemory());
  void* pipeline_ptr = nullptr;
  TextureCache::LoadShaderIndex load_shader_index = kLoadShaderIndexUnknown;
  {
    const char* wanted = nullptr;
    switch (entry.load_shader) {
      case SharedLoadFamily::k8bpb:
        wanted = "texture_load_8bpb_cs";
        load_shader_index = kLoadShaderIndex8bpb;
        break;
      case SharedLoadFamily::k16bpb:
        wanted = "texture_load_16bpb_cs";
        load_shader_index = kLoadShaderIndex16bpb;
        break;
      case SharedLoadFamily::k32bpb:
        wanted = "texture_load_32bpb_cs";
        load_shader_index = kLoadShaderIndex32bpb;
        break;
      case SharedLoadFamily::k64bpb:
        wanted = "texture_load_64bpb_cs";
        load_shader_index = kLoadShaderIndex64bpb;
        break;
      case SharedLoadFamily::k16bpbRgba4ToRgba8:
        wanted = "texture_load_r4g4b4a4_r8g8b8a8_cs";
        load_shader_index = kLoadShaderIndexRGBA4ToRGBA8;
        break;
      case SharedLoadFamily::k16bpbR5G6B5ToRgba8:
        wanted = "texture_load_r5g6b5_r8g8b8a8_cs";
        load_shader_index = kLoadShaderIndexR5G6B5ToRGBA8;
        break;
      case SharedLoadFamily::k16bpbR5G5B5A1ToRgba8:
        wanted = "texture_load_r5g5b5a1_r8g8b8a8_cs";
        load_shader_index = kLoadShaderIndexR5G6B5ToRGBA8;  // nearest converter this tree publishes
        break;
      case SharedLoadFamily::kCtx1ToRg8:
        wanted = "texture_load_ctx1_cs";
        load_shader_index = kLoadShaderIndexCTX1;
        break;
      case SharedLoadFamily::kDxt3aToR8:
        wanted = "texture_load_dxt3a_cs";
        load_shader_index = kLoadShaderIndexDXT3A;
        break;
      case SharedLoadFamily::kR11G11B10ToRgba16:
        wanted = "texture_load_r11g11b10_rgba16_cs";
        load_shader_index = kLoadShaderIndexR11G11B10ToRGBA16;
        break;
      case SharedLoadFamily::kR10G11B11ToRgba16:
        wanted = "texture_load_r10g11b11_rgba16_cs";
        load_shader_index = kLoadShaderIndexR10G11B11ToRGBA16;
        break;
      case SharedLoadFamily::k128bpb:
        wanted = "texture_load_128bpb_cs";
        load_shader_index = kLoadShaderIndex128bpb;
        break;
      case SharedLoadFamily::kDepthUnormToR32:
        wanted = "texture_load_depth_unorm_cs";
        load_shader_index = kLoadShaderIndexDepthUnorm;
        break;
      case SharedLoadFamily::kDepthFloatToR32:
        wanted = "texture_load_depth_float_cs";
        load_shader_index = kLoadShaderIndexDepthFloat;
        break;
      case SharedLoadFamily::kR5G5B6ToB5G6R5Rbga:
        wanted = "texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs";
        load_shader_index = kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle;
        break;
      case SharedLoadFamily::kDxt3aAs1111ToAbgr4:
        wanted = "texture_load_dxt3aas1111_argb4_cs";
        load_shader_index = kLoadShaderIndexDXT3AAs1111ToARGB4;
        break;
      case SharedLoadFamily::kGbgr8ToRgba8:
        wanted = "texture_load_gbgr8_rgb8_cs";
        load_shader_index = kLoadShaderIndexGBGR8ToRGB8;
        break;
      case SharedLoadFamily::kBgrg8ToRgba8:
        wanted = "texture_load_bgrg8_rgb8_cs";
        load_shader_index = kLoadShaderIndexBGRG8ToRGB8;
        break;
    }
    if (!wanted) {
      return false;
    }
    int index = family_pipelines_.FindIndex(wanted);
    if (index < 0) {
      return false;
    }
    pipeline_ptr = family_pipelines_.pipeline(size_t(index));
  }
  if (!pipeline_ptr) {
    return false;
  }
  const TextureCache::LoadShaderInfo& info =
      GetLoadShaderInfo(load_shader_index);
  // Guest and host block sizes: equal for the bit-copy families, 2 -> 4 for
  // the RGBA4 / R5G6B5 -> RGBA8 expansions.
  const uint32_t guest_bpb = entry.bytes_per_block;
  const uint32_t bpb = SharedTextureHostBytesPerBlock(entry);
  if (info.bytes_per_host_block != bpb) {
    return false;
  }
  const uint32_t blocks_per_thread = 1u << info.guest_x_blocks_per_thread_log2;
  // 4x4 block-compressed formats dispatch over guest blocks. The decoding
  // families (CTX1, DXT3A) write host TEXEL rows, host_expand (4) of them per
  // guest block row; the BC-preserving families and the uncompressed formats
  // write one host block / texel per guest block (host_expand 1), so for
  // them the host side is addressed in blocks and the blit converts to
  // texels (Metal takes compressed copy sizes in texels, rows in bytes).
  // Block width and height differ for the 4:2:2 pairs (2x1).
  const uint32_t block_w = SharedTextureBlockWidth(entry);
  const uint32_t block_dim = SharedTextureBlockHeight(entry);
  const uint32_t host_expand = SharedTextureHostExpand(entry);

  // Plan every level's scratch region before encoding anything (atomic
  // fallback, like the direct path).
  struct LevelDispatch {
    MetalLoadShaderPipelines::LoadConstants constants;
    void* scratch = nullptr;
    uint32_t scratch_offset = 0;
    uint32_t host_pitch = 0;
    uint32_t region_w = 0;  // guest blocks
    uint32_t region_h = 0;
    uint32_t threads_x = 0;
    uint32_t src_x = 0;     // level origin in the scratch region, texels
    uint32_t src_y = 0;
    uint32_t depth = 1;     // 3D: slices in this level's scratch
    uint32_t src_z = 0;     // 3D packed tail: first slice of the level
  };
  std::vector<LevelDispatch> dispatches;
  dispatches.reserve(plan.size());
  const uint64_t submission = submission_provider_ ? submission_provider_() : 0;
  for (const MipLevelPlan& p : plan) {
    LevelDispatch d;
    // The shader reads blocks [0, region) relative to the level's guest
    // offset; a packed sub-tile origin (px, py) is covered by dispatching the
    // enclosing region and blitting the sub-rectangle out of it.
    // Packed origins come from the planner in texels; block-compressed
    // packed levels sit on whole blocks.
    const uint32_t px_blocks = p.px / block_w;
    const uint32_t py_blocks = p.py / block_dim;
    d.region_w = px_blocks + (p.width + block_w - 1u) / block_w;
    d.region_h = py_blocks + (p.height + block_dim - 1u) / block_dim;
    // Level origin in host units (texels for the decoders, blocks for BC).
    d.src_x = px_blocks * host_expand;
    d.src_y = py_blocks * host_expand;
    uint32_t written_row =
        ((d.region_w + blocks_per_thread - 1u) / blocks_per_thread) *
        blocks_per_thread * host_expand * bpb;
    d.host_pitch = (written_row + 255u) & ~255u;
    // A packed 3D level sits at a depth origin inside its tail: dispatch the
    // enclosing slices and blit from the origin slice.
    d.depth = p.pz + std::max(p.depth, 1u);
    d.src_z = p.pz;
    uint64_t dest_bytes =
        uint64_t(d.host_pitch) * d.region_h * host_expand * d.depth + 256u;
    static uint32_t s_scratch_logs = 0;
    if (dest_bytes > kScratchBufferSize) {
      if (s_scratch_logs++ < 16) {
        REXLOG_WARN("MetalSharedTex FAMILY-FAIL scratch-too-big fmt={} base={:08X} "
               "L{} {}x{} needs {} bytes",
               uint32_t(key.format), key.base_page << 12, p.host_level,
               p.width, p.height, dest_bytes);
      }
      return false;
    }
    if (!AllocateScratch(uint32_t(dest_bytes), 256, submission, &d.scratch,
                         &d.scratch_offset)) {
      if (s_scratch_logs++ < 16) {
        REXLOG_WARN("MetalSharedTex FAMILY-FAIL scratch-full fmt={} base={:08X} "
               "L{} {}x{} needs {} bytes",
               uint32_t(key.format), key.base_page << 12, p.host_level,
               p.width, p.height, dest_bytes);
      }
      return false;
    }
    // The planner derives pitch_blocks at 32bpp; rescale to this format's
    // block size (pitch_bytes == pitch_blocks * 4 by construction).
    uint32_t pitch_blocks = (p.pitch_blocks * 4u) / guest_bpb;
    d.constants = {};
    // Bit 0 = tiled; the upstream shaders address linear levels by pitch.
    d.constants.is_tiled_3d_endian_scale =
        (key.tiled ? 1u : 0u) | (d.depth > 1 ? 2u : 0u) |
        (uint32_t(key.endianness) << 2);
    d.constants.guest_offset = p.guest_offset;
    d.constants.guest_pitch_aligned = pitch_blocks;
    d.constants.guest_z_stride_block_rows_aligned = p.z_stride_block_rows;
    d.constants.size_blocks[0] = d.region_w;
    d.constants.size_blocks[1] = d.region_h;
    d.constants.size_blocks[2] = d.depth;
    d.constants.host_offset = 0;
    d.constants.host_pitch = d.host_pitch;
    d.constants.height_texels = d.region_h * host_expand;
    d.threads_x = (d.region_w + blocks_per_thread - 1u) / blocks_per_thread;
    dispatches.push_back(d);
  }

  void* command_buffer_ptr = prepare_transfer_ ? prepare_transfer_(side_load_) : nullptr;
  if (!command_buffer_ptr) {
    return false;
  }
  id<MTLCommandBuffer> command_buffer =
      (__bridge id<MTLCommandBuffer>)command_buffer_ptr;
  id<MTLTexture> base_texture =
      (__bridge id<MTLTexture>)metal_texture.mtl_texture();
  if (!base_texture) {
    return false;
  }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (!encoder) {
    return false;
  }
  encoder.label = @"shared-texcache family load";
  NoteRecentLoad(RecentLoad{uint32_t(key.base_page), key.GetWidth(),
                            key.GetHeight(), uint32_t(dispatches.size()),
                            submission_provider_ ? submission_provider_() : 0,
                            side_load_, false});
  [encoder setComputePipelineState:(__bridge id<MTLComputePipelineState>)
                                       pipeline_ptr];
  [encoder setBuffer:(__bridge id<MTLBuffer>)metal_shared_memory.GetBuffer()
              offset:0
             atIndex:MetalLoadShaderPipelines::kBufferIndexSource];
  MTLSize group = MTLSizeMake(MetalLoadShaderPipelines::kThreadgroupSizeX,
                              MetalLoadShaderPipelines::kThreadgroupSizeY,
                              MetalLoadShaderPipelines::kThreadgroupSizeZ);
  for (const LevelDispatch& d : dispatches) {
    [encoder setBytes:&d.constants
               length:sizeof(d.constants)
              atIndex:MetalLoadShaderPipelines::kBufferIndexPushConstants];
    [encoder setBuffer:(__bridge id<MTLBuffer>)d.scratch
                offset:d.scratch_offset
               atIndex:MetalLoadShaderPipelines::kBufferIndexDest];
    [encoder dispatchThreadgroups:MTLSizeMake(
                                      (d.threads_x + group.width - 1) /
                                          group.width,
                                      (d.region_h + group.height - 1) /
                                          group.height,
                                      d.depth)
            threadsPerThreadgroup:group];
  }
  [encoder endEncoding];

  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  if (!blit) {
    return false;
  }
  blit.label = @"shared-texcache family load blit";
  for (size_t i = 0; i < plan.size(); ++i) {
    const MipLevelPlan& p = plan[i];
    const LevelDispatch& d = dispatches[i];
    const NSUInteger bytes_per_image =
        NSUInteger(d.host_pitch) * d.region_h * host_expand;
    NSUInteger source_offset = d.scratch_offset +
                               NSUInteger(d.src_z) * bytes_per_image +
                               NSUInteger(d.src_y) * d.host_pitch +
                               NSUInteger(d.src_x) * bpb;
    // Rows in host units (texel rows, or block rows for BC); the size is in
    // texels either way, which for BC Metal requires to be the whole level
    // when it is smaller than a block (it always is the whole level here).
    [blit copyFromBuffer:(__bridge id<MTLBuffer>)d.scratch
               sourceOffset:source_offset
          sourceBytesPerRow:d.host_pitch
        sourceBytesPerImage:bytes_per_image
                 sourceSize:MTLSizeMake(p.width, p.height,
                                        std::max(p.depth, 1u))
                  toTexture:base_texture
           destinationSlice:p.slice
           destinationLevel:p.host_level
          destinationOrigin:MTLOriginMake(0, 0, 0)];
  }
  // XE_METAL_DUMP_ROUTE=<hex base or mip address>: read every loaded level of
  // this routed texture back after the load (host rows, bpb bytes per texel) to
  // /tmp/xe_route_<base>_L<level>_<n>.bin -- the texel-level oracle against
  // the legacy decode's XE_METAL_DUMP_FETCH dumps of the same base.
  static const uint32_t s_dump_route = [] {
    const char* v = std::getenv("XE_METAL_DUMP_ROUTE");
    return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
  }();
  if (s_dump_route && ((uint32_t(key.base_page) << 12) == s_dump_route ||
                       (uint32_t(key.mip_page) << 12) == s_dump_route)) {
    static uint32_t s_n = 0;
    const uint32_t n = s_n++;
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    for (const MipLevelPlan& p0 : plan) {
      NSUInteger row = NSUInteger(p0.width) * bpb;
      // Volumes: every depth slice of the level, image after image.
      const NSUInteger depth =
          base_texture.textureType == MTLTextureType3D
              ? std::max<NSUInteger>(p0.depth, 1)
              : 1;
      id<MTLBuffer> rb =
          [device newBufferWithLength:row * p0.height * depth
                              options:MTLResourceStorageModeShared];
      [blit copyFromTexture:base_texture
                  sourceSlice:0
                  sourceLevel:p0.host_level
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(p0.width, p0.height, depth)
                     toBuffer:rb
            destinationOffset:0
       destinationBytesPerRow:row
     destinationBytesPerImage:row * p0.height];
      std::string path = fmt::format("/tmp/xe_route_{:08X}_L{}_{}.bin",
                                     s_dump_route, p0.host_level, n);
      uint32_t w = p0.width, h = p0.height, lvl = p0.host_level,
               go = p0.guest_offset, px = p0.px, py = p0.py,
               pb = p0.pitch_blocks;
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        if (FILE* f = fopen(path.c_str(), "wb")) {
          fwrite([rb contents], 1, row * h * depth, f);
          fclose(f);
        }
        REXLOG_WARN("MetalRouteDump {} L{} {}x{} bpb={} guest_offset={:08X} px={} py={} pitch_blocks32={}",
               path, lvl, w, h, bpb, go, px, py, pb);
      }];
    }
  }
  [blit endEncoding];
  static uint32_t s_family_logs = 0;
  static const bool s_family_log_all =
      std::getenv("XE_METAL_FAMILY_LOAD_LOG") != nullptr;
  if (s_family_logs++ < 16 || s_family_log_all) {
    REXLOG_INFO("MetalSharedTex FAMILY-LOAD fmt={} base={:08X} {}x{} levels={} bpb={}",
           uint32_t(key.format), key.base_page << 12, key.GetWidth(),
           key.GetHeight(), plan.size(), bpb);
  }
  family_load_count_ += 1;
  return true;
}

void MetalSharedTextureCache::UpdateTextureBindingsBackend(
    uint32_t fetch_constant_mask) {
  ++serve_generation_;
  uint32_t bits = fetch_constant_mask;
  uint32_t index;
  while (rex::bit_scan_forward(bits, &index)) {
    bits = ((bits) & ((bits) - 1));
    BindingSlot& binding_slot = binding_slots_[index];
    const TextureBinding* binding = GetValidTextureBinding(index);
    const Texture* texture = binding ? binding->texture : nullptr;
    if (!texture) {
      // Park rather than release: BeginFrame resets ALL bindings each swap,
      // and releasing here would churn a fresh slot+view per warm texture
      // per frame. A parked slot is never served (the serve check requires a
      // valid matching binding) and is retired when a different binding
      // arrives or at destruction; its view pins at most 32 textures.
      continue;
    }
    if (binding_slot.texture == texture &&
        binding_slot.host_swizzle == binding->host_swizzle &&
        binding_slot.slot) {
      continue;  // Descriptor already expresses this binding.
    }
    ReleaseBindingSlot(binding_slot);
    uint32_t swizzle = binding->host_swizzle;
    void* view_ptr = nullptr;
    uint32_t slot = 0;
    if (!AcquireView(*static_cast<MetalTexture*>(
                         const_cast<Texture*>(texture)),
                     swizzle, false, &view_ptr, &slot)) {
      continue;  // Unserved; the legacy path serves this index.
    }
    binding_slot.texture = texture;
    binding_slot.host_swizzle = swizzle;
    binding_slot.sampled_view = view_ptr;
    binding_slot.slot = slot;
    binding_slot.owns_views = !RouteViewCacheEnabled();
  }
}

bool MetalSharedTextureCache::AcquireView(MetalTexture& texture,
                                          uint32_t host_swizzle,
                                          bool is_signed, void** view_out,
                                          uint32_t* slot_out) {
  ++serve_generation_;
  const bool cached = RouteViewCacheEnabled();
  if (cached) {
    for (const MetalTexture::CachedView& view : texture.views) {
      if (view.host_swizzle == host_swizzle && view.is_signed == is_signed) {
        *view_out = view.view;
        *slot_out = view.slot;
        return true;
      }
    }
  }
  if (!legacy_) {
    return false;
  }
  id<MTLTexture> base_texture = (__bridge id<MTLTexture>)texture.mtl_texture();
  // Always a view: the 2D->2DArray type change is required even for the
  // identity swizzle (guest shaders sample texture2d_array).
  MTLTextureSwizzleChannels channels = MTLTextureSwizzleChannelsMake(
      HostSwizzleComponentToMetal((host_swizzle >> 0) & 0b111),
      HostSwizzleComponentToMetal((host_swizzle >> 3) & 0b111),
      HostSwizzleComponentToMetal((host_swizzle >> 6) & 0b111),
      HostSwizzleComponentToMetal((host_swizzle >> 9) & 0b111));
  const SharedTextureFormatEntry* entry =
      FindSharedTextureFormat(texture.GetKey().format);
  // Signed bindings: an RGBA8Snorm (or the entry's signed format) view of the
  // SAME loaded bytes, with the same swizzle/type/levels.
  MTLPixelFormat view_format =
      is_signed ? (entry && entry->host_format_signed
                       ? MTLPixelFormat(entry->host_format_signed)
                       : MTLPixelFormatRGBA8Snorm)
                : (entry ? MTLPixelFormat(entry->host_format)
                         : MTLPixelFormatRGBA8Unorm);
  // FULL mip range: a mipped texture served through a single-level view
  // samples only mip 0, which would silently break guest LOD selection.
  const bool cube_base = base_texture.textureType == MTLTextureTypeCubeArray;
  const bool volume_base = base_texture.textureType == MTLTextureType3D;
  id<MTLTexture> view = [base_texture
      newTextureViewWithPixelFormat:view_format
                        textureType:cube_base     ? MTLTextureTypeCubeArray
                                    : volume_base ? MTLTextureType3D
                                                  : MTLTextureType2DArray
                             levels:NSMakeRange(0, base_texture.mipmapLevelCount)
                            slices:NSMakeRange(
                                       0, volume_base
                                              ? 1
                                              : base_texture.arrayLength *
                                                    (cube_base ? 6 : 1))
                           swizzle:channels];
  if (!view) {
    return false;
  }
  uint32_t slot = legacy_->AllocateExternalSlot();
  if (!slot) {
    return false;  // Heap exhausted; fall back rather than corrupt slot 0.
  }
  ++external_slots_held_;
  void* view_ptr = (__bridge_retained void*)view;
  legacy_->BindExternalSlot(slot, view_ptr);
  if (cached) {
    texture.views.push_back({host_swizzle, is_signed, view_ptr, slot});
  }
  *view_out = view_ptr;
  *slot_out = slot;
  return true;
}

void MetalSharedTextureCache::ForgetTextureBindings(const Texture* texture) {
  ++serve_generation_;
  for (BindingSlot& binding_slot : binding_slots_) {
    if (binding_slot.texture == texture) {
      ReleaseBindingSlot(binding_slot);
    }
  }
}

void MetalSharedTextureCache::ClearCache() {
  ++serve_generation_;
  // Release the descriptor state FIRST, while the textures it names are still
  // alive. The base clear below resets the bindings, and this cache's
  // UpdateTextureBindingsBackend deliberately parks a texture-less binding
  // instead of releasing it, so nothing else would ever retire these.
  for (BindingSlot& binding_slot : binding_slots_) {
    ReleaseBindingSlot(binding_slot);
  }
  // Derived scan results keyed by texture shape; meaningless once the textures
  // are gone, and a clear must not leave state behind that a growth check
  // would see.
  range_memo_.Clear();
  TextureCache::ClearCache();
}

void MetalSharedTextureCache::ReleaseBindingSlot(BindingSlot& binding_slot) {
  ++serve_generation_;
  if (!binding_slot.slot && !binding_slot.sampled_view) {
    binding_slot = BindingSlot{};
    return;
  }
  if (!binding_slot.owns_views) {
    // The texture's view cache owns them.
    binding_slot = BindingSlot{};
    return;
  }
  if (legacy_) {
    // In-flight draws may still index the slot / sample the view: retire
    // both through the single submission-gated legacy pools.
    if (binding_slot.slot) {
      legacy_->RetireExternalSlot(binding_slot.slot);
      if (external_slots_held_) {
        --external_slots_held_;
      }
    }
    if (binding_slot.sampled_view) {
      legacy_->RetireExternalTexture(binding_slot.sampled_view);
    }
    if (binding_slot.signed_slot) {
      legacy_->RetireExternalSlot(binding_slot.signed_slot);
      if (external_slots_held_) {
        --external_slots_held_;
      }
    }
    if (binding_slot.signed_view) {
      legacy_->RetireExternalTexture(binding_slot.signed_view);
    }
  } else {
    if (binding_slot.sampled_view) {
      id<MTLTexture> released =
          (__bridge_transfer id<MTLTexture>)binding_slot.sampled_view;
      (void)released;
    }
    if (binding_slot.signed_view) {
      id<MTLTexture> released =
          (__bridge_transfer id<MTLTexture>)binding_slot.signed_view;
      (void)released;
    }
  }
  binding_slot = BindingSlot{};
}

void MetalSharedTextureCache::PrepareEligible(uint32_t eligible_mask,
                                              uint32_t dirty_mask) {
  if (!eligible_mask) {
    return;
  }
  uint64_t submission = submission_provider_ ? submission_provider_() : 0;
  if (submission > last_begun_submission_) {
    ++serve_generation_;
    last_begun_submission_ = submission;
    BeginSubmission(submission);
  }
  // Re-parse only the eligible constants written since the last prep (the
  // command processor's WriteRegister keeps the dirty mask); an unchanged
  // binding keeps its texture, as in the D3D12 cache.
  uint32_t bits = eligible_mask & dirty_mask;
  if (bits) {
    ++serve_generation_;
  }
  uint32_t index;
  while (rex::bit_scan_forward(bits, &index)) {
    bits = ((bits) & ((bits) - 1));
    TextureFetchConstantWritten(index);
  }
  RequestTextures(eligible_mask);
  // Record which bindings are current as of this prep (see
  // BindingSlot::prep_current): the serve check reads it when a watch fires
  // in the window between here and the draw's issue.
  bits = eligible_mask;
  while (rex::bit_scan_forward(bits, &index)) {
    bits = ((bits) & ((bits) - 1));
    const TextureBinding* binding = GetValidTextureBinding(index);
    const Texture* texture = binding ? binding->texture : nullptr;
    const bool prep_current = texture && !(texture->GetOutdatedMask() & texture->kOutdatedBitBase) != 0 &&
                              !(texture->GetOutdatedMask() & texture->kOutdatedBitMips) != 0;
    binding_slots_[index].prep_current = prep_current;
    binding_slots_[index].serve_latched = false;
    if (!prep_current) {
      // An outdated binding's serve depends on later state (the stale-serve
      // rule), so no draw after this prep may reuse an earlier serve.
      ++serve_generation_;
    }
  }
}

void MetalSharedTextureCache::LatchServe(uint32_t fetch_constant_index) {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_SERVE_LATCH");
    return !(v && v[0] == '0' && !v[1]);
  }();
  if (s_enabled && fetch_constant_index < xenos::kTextureFetchConstantCount) {
    binding_slots_[fetch_constant_index].serve_latched = true;
  }
}

uint32_t MetalSharedTextureCache::GetServedSlot(uint32_t fetch_constant_index,
                                                bool is_signed) {
  if (fetch_constant_index >= xenos::kTextureFetchConstantCount) {
    return 0;
  }
  BindingSlot& binding_slot = binding_slots_[fetch_constant_index];
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding || !binding->texture || !binding_slot.slot ||
      binding_slot.texture != binding->texture) {
    return 0;
  }
  Texture* texture = binding->texture;
  MetalTexture* metal_texture = static_cast<MetalTexture*>(texture);
  if ((texture->GetOutdatedMask() & texture->kOutdatedBitBase) != 0 || (texture->GetOutdatedMask() & texture->kOutdatedBitMips) != 0) {
    // Invalidated between prep and serve. The content loaded at prep is what
    // the D3D12 cache binds too (a watch firing after RequestTextures marks
    // the texture for the NEXT request); only a texture that was NOT loaded
    // by this submission -- its load failed, or it is outdated from an
    // earlier one -- drops to the CPU path, which serves current RAM. Before
    // 2026-10-16 any outdated texture dropped, and a title streaming into a
    // texture it samples (Rage: ~140 CPU writes per frame into its 16 MB
    // DXT5 page cache) hit that window on most frames: a 2.4 ms CPU untile of
    // the whole page cache per hit. XE_METAL_SHARED_SERVE_STALE=0 restores the
    // strict rule.
    // Three ways to know the prep-time content is this draw's: the binding
    // was current when this draw's prep finished (prep_current, the D3D12
    // rule), the outdated region was loaded by this submission, or this
    // draw's route verification already served it (serve_latched: Far Cry 2
    // streams into 128x128 DXT1 textures it is drawing with, and a write
    // landing between the verification and the bind dropped the draw).
    if (!ServeStaleEnabled()) {
      return 0;
    }
    if (!(binding_slot.prep_current ||
          (submission_provider_ &&
           ServedByCurrentSubmission(*metal_texture,
                                     submission_provider_())))) {
      if (!binding_slot.serve_latched) {
        return 0;
      }
      static uint32_t s_latched_serves = 0;
      if (s_latched_serves++ < 4 || !(s_latched_serves & 1023)) {
        REXLOG_INFO(
            "MetalSharedTex: fc={} base={:08X} invalidated after its draw "
            "verified it, served as verified (#{})",
            fetch_constant_index, texture->GetKey().base_page << 12,
            s_latched_serves);
      }
    }
    ++census_served_stale_;
  }
  if (submission_fault_query_) {
    // M4: content that rode a lost command buffer must never be served. BOTH
    // regions are consulted, because base and mip loads carry independent
    // submissions -- checking only the base would make the claimed independent
    // provenance false and could serve a texture whose mip chain is garbage.
    // The next invalidation (recurring resolve dests re-invalidate every frame)
    // reloads, and the CPU path covers until then.
    bool base_faulted =
        metal_texture->last_load_submission &&
        submission_fault_query_(metal_texture->last_load_submission);
    bool mip_faulted =
        metal_texture->last_mip_load_submission &&
        submission_fault_query_(metal_texture->last_mip_load_submission);
    if (base_faulted || mip_faulted) {
      return 0;
    }
  }
  if (is_signed) {
    // Lazy signed serve: an RGBA8Snorm view of the SAME loaded bytes, with
    // the SAME swizzle/type/levels as the unsigned view. Created on first
    // signed use, in the texture's view cache.
    if (!binding_slot.signed_slot) {
      void* view_ptr = nullptr;
      uint32_t slot = 0;
      if (!AcquireView(*metal_texture, binding_slot.host_swizzle, true,
                       &view_ptr, &slot)) {
        return 0;  // Unserved; legacy path handles the signed binding.
      }
      binding_slot.signed_view = view_ptr;
      binding_slot.signed_slot = slot;
      static uint32_t s_signed_serve_logs = 0;
      if (s_signed_serve_logs++ < 16) {
        TextureKey key = texture->GetKey();
        REXLOG_INFO(
            "MetalSharedTex SIGNED-SERVE fc={} base={:08X} {}x{} pitch={} "
            "mips={} fmt={}",
            fetch_constant_index, key.base_page << 12, key.GetWidth(),
            key.GetHeight(), uint32_t(key.pitch) << 5, uint32_t(key.mip_max_level),
            uint32_t(key.format));
      }
    }
    texture->MarkAsUsed();
    if (legacy_) {
      legacy_->AddActiveTexture(binding_slot.signed_view);
    }
    return binding_slot.signed_slot;
  }
  texture->MarkAsUsed();
  if (legacy_) {
    legacy_->AddActiveTexture(binding_slot.sampled_view);
  }
  return binding_slot.slot;
}

const char* MetalSharedTextureCache::UnservedReason(
    uint32_t fetch_constant_index, bool is_signed) {
  if (fetch_constant_index >= xenos::kTextureFetchConstantCount) {
    return "index";
  }
  BindingSlot& binding_slot = binding_slots_[fetch_constant_index];
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding) {
    return "no-valid-binding";
  }
  if (!binding->texture) {
    return "no-texture";
  }
  if (!binding_slot.slot) {
    return "no-slot";
  }
  if (binding_slot.texture != binding->texture) {
    return "slot-texture-mismatch";
  }
  const bool stale_ok =
      ServeStaleEnabled() &&
      (binding_slot.prep_current || binding_slot.serve_latched ||
       (submission_provider_ &&
        ServedByCurrentSubmission(
            *static_cast<const MetalTexture*>(binding->texture),
            submission_provider_())));
  if ((binding->texture->GetOutdatedMask() & binding->texture->kOutdatedBitBase) != 0 && !stale_ok) {
    return "base-outdated";
  }
  if ((binding->texture->GetOutdatedMask() & binding->texture->kOutdatedBitMips) != 0 && !stale_ok) {
    return "mips-outdated";
  }
  if (is_signed && !binding_slot.signed_slot) {
    return "signed-view";
  }
  return "served";
}

std::string MetalSharedTextureCache::DescribeServeState(
    uint32_t fetch_constant_index, bool is_signed) {
  const char* why = UnservedReason(fetch_constant_index, is_signed);
  if (fetch_constant_index >= xenos::kTextureFetchConstantCount) {
    return why;
  }
  const BindingSlot& binding_slot = binding_slots_[fetch_constant_index];
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  const MetalTexture* texture =
      binding ? static_cast<const MetalTexture*>(binding->texture) : nullptr;
  uint64_t submission = submission_provider_ ? submission_provider_() : 0;
  bool faulted = false;
  if (texture && submission_fault_query_) {
    faulted = (texture->last_load_submission &&
               submission_fault_query_(texture->last_load_submission)) ||
              (texture->last_mip_load_submission &&
               submission_fault_query_(texture->last_mip_load_submission));
  }
  return fmt::format(
      "{} prep_current={} base_outdated={} mips_outdated={} load_sub={} "
      "mip_load_sub={} cur_sub={} faulted={}",
      why, binding_slot.prep_current ? 1 : 0,
      texture && (texture->GetOutdatedMask() & texture->kOutdatedBitBase) != 0 ? 1 : 0,
      texture && (texture->GetOutdatedMask() & texture->kOutdatedBitMips) != 0 ? 1 : 0,
      texture ? texture->last_load_submission : 0,
      texture ? texture->last_mip_load_submission : 0, submission,
      faulted ? 1 : 0);
}

bool MetalSharedTextureCache::FindResolveForward(
    uint32_t dest_base, uint32_t dest_pitch, uint32_t bpp_log2,
    uint32_t offset_x, uint32_t offset_y, uint32_t width, uint32_t height,
    uint32_t extent_bytes, ResolveForwardTarget* out) {
  // XE_METAL_RESOLVE_FORWARD_LOG=1: why resolves are not forwarded (the
  // first 300, then one in 32).
  static const bool s_log =
      std::getenv("XE_METAL_RESOLVE_FORWARD_LOG") != nullptr;
  static uint32_t s_logs = 0;
  auto note = [&](const char* why, const TextureKey* key) {
    if (ResolveForwardTraced(dest_base) ||
        (s_log && (s_logs++ < 300 || (s_logs & 31u) == 0))) {
      REXLOG_INFO(
          "MetalResolveForward {} dest={:08X} pitch={} bpp_log2={} off=({},{}) "
          "{}x{} extent={:X} tex={}",
          why, dest_base, dest_pitch, bpp_log2, offset_x, offset_y, width,
          height, extent_bytes,
          key ? fmt::format("fmt={} {:08X} {}x{} pitch={}",
                            uint32_t(key->format),
                            uint32_t(key->base_page) << 12, key->GetWidth(),
                            key->GetHeight(), key->pitch)
              : std::string());
    }
  };
  if (!ResolveForwardEnabled() || GetDrawResolutionScaleX() != 1 ||
      GetDrawResolutionScaleY() != 1 || (bpp_log2 != 2 && bpp_log2 != 3)) {
    note("disabled", nullptr);
    return false;
  }
  const uint32_t pitch_bytes = dest_pitch << bpp_log2;
  MetalTexture* best = nullptr;
  uint32_t best_kind = 0, best_origin_y = 0;
  // Textures starting at or below the destination, nearest first; no
  // forwardable texture is larger than 32 MB.
  constexpr uint32_t kMaxTextureBytes = 32u << 20;
  for (auto it = forward_textures_.upper_bound(dest_base);
       it != forward_textures_.begin();) {
    --it;
    if (dest_base - it->first >= kMaxTextureBytes) {
      break;
    }
    MetalTexture& texture = *it->second;
    const TextureKey key = texture.GetKey();
    const uint32_t base = it->first;
    const uint32_t delta = dest_base - base;
    if (delta >= texture.GetGuestBaseSize()) {
      continue;
    }
    const uint32_t kind = ResolveForwardKind(key);
    if (!kind || (kind == 2 ? 3u : 2u) != bpp_log2) {
      note("format", &key);
      continue;
    }
    // The load's own addressing: same pitch, and the destination a whole
    // number of 32-row bands in, so the texture's texel (x, origin_y + y) is
    // the destination's texel (x, y).
    std::vector<MipLevelPlan> plan;
    if (!PlanLevels(texture, true, false, &plan) || plan.size() != 1 ||
        plan[0].guest_offset != base ||
        plan[0].pitch_blocks * 4u != pitch_bytes || plan[0].px || plan[0].py ||
        plan[0].depth != 1 || plan[0].slice ||
        delta % (pitch_bytes * 32u) != 0) {
      note("layout", &key);
      continue;
    }
    const uint32_t origin_y = delta / pitch_bytes;
    // Whether the resolve rewrites every texel; otherwise the texture must be
    // current, so that the texels it leaves alone still match memory.
    const bool covers_all =
        !delta && !offset_x && !offset_y && width >= key.GetWidth() &&
        height >= key.GetHeight() && texture.GetGuestBaseSize() <= extent_bytes;
    if (!covers_all && (texture.GetOutdatedMask() & texture.kOutdatedBitBase) != 0) {
      note("partial-outdated", &key);
      continue;
    }
    // Several textures of one range (different formats or sizes): the most
    // recently used one is forwarded, the others reload as before.
    if (!best || texture.GetLastUsageSubmissionIndex() >
                     best->GetLastUsageSubmissionIndex()) {
      best = &texture;
      best_kind = kind;
      best_origin_y = origin_y;
    }
  }
  if (!best) {
    note("none", nullptr);
    return false;
  }
  if (!best->forward_view) {
    id<MTLTexture> texture = (__bridge id<MTLTexture>)best->mtl_texture();
    id<MTLTexture> view =
        texture
            ? [texture
                  newTextureViewWithPixelFormat:best_kind == 1
                                                    ? MTLPixelFormatRGBA8Uint
                                                : best_kind == 2
                                                    ? MTLPixelFormatRGBA16Uint
                                                    : MTLPixelFormatR32Uint]
            : nil;
    if (!view) {
      return false;
    }
    best->forward_view = (__bridge_retained void*)view;
  }
  const TextureKey key = best->GetKey();
  out->texture = best;
  out->mtl_texture = best->forward_view;
  out->kind = best_kind;
  out->endian = uint32_t(key.endianness);
  out->width = key.GetWidth();
  out->height = key.GetHeight();
  out->origin_y = best_origin_y;
  return true;
}

void MetalSharedTextureCache::CompleteResolveForward(
    const ResolveForwardTarget& target) {
  MetalTexture* texture = static_cast<MetalTexture*>(target.texture);
  const uint32_t base = uint32_t(texture->GetKey().base_page) << 12;
  auto range = forward_textures_.equal_range(base);
  bool live = false;
  for (auto it = range.first; it != range.second; ++it) {
    live |= it->second == texture;
  }
  // A page of it left invalid (a guest write the resolve did not cover) still
  // needs the load.
  if (!live ||
      !(uint64_t(base) + uint64_t(base + texture->GetGuestBaseSize() <= ::rex::gpu::SharedMemory::kBufferSize))) {
    if (ResolveForwardTraced(base)) {
      REXLOG_INFO("MetalResolveForward NOT-COMPLETED base={:08X} live={}", base,
             live ? 1 : 0);
    }
    return;
  }
  // The resolve's watch invalidation has outdated it; its content is now
  // memory's again, so it is current and watched again like a loaded texture.
  // The watch guard is the shared memory's own lock, which is what the base's UpdateAndWatch expects.
  auto watch_guard = GetSharedMemory().AcquireLock();
  texture->UpdateAndWatch(watch_guard);
  texture->last_load_submission =
      submission_provider_ ? submission_provider_() : 0;
  texture->base_hash = 0;
  ++texture->loads;
  texture->MarkAsUsed();
  ++census_forwarded_;
  static const bool s_log =
      std::getenv("XE_METAL_RESOLVE_FORWARD_LOG") != nullptr;
  static uint32_t s_logs = 0;
  if (ResolveForwardTraced(base) ||
      (s_log && (s_logs++ < 300 || (s_logs & 31u) == 0))) {
    REXLOG_INFO("MetalResolveForward FORWARDED base={:08X} kind={} {}x{} endian={} "
           "origin_y={} submission={}",
           base, target.kind, target.width, target.height, target.endian,
           target.origin_y, texture->last_load_submission);
  }
}

void MetalSharedTextureCache::OnSubmissionsFaulted(
    const std::vector<uint64_t>& submissions) {
  ++serve_generation_;
  if (submissions.empty()) {
    return;
  }
  ForEachTexture([&](Texture& texture) {
    MetalTexture& metal_texture = static_cast<MetalTexture&>(texture);
    // BOTH regions are checked independently: base and mip loads carry their
    // own submission, so a later partial reload of one cannot hide an earlier
    // faulted load of the other.
    bool base_faulted = false;
    bool mip_faulted = false;
    for (uint64_t submission : submissions) {
      if (metal_texture.last_load_submission &&
          metal_texture.last_load_submission == submission) {
        base_faulted = true;
      }
      if (metal_texture.last_mip_load_submission &&
          metal_texture.last_mip_load_submission == submission) {
        mip_faulted = true;
      }
    }
    if (!base_faulted && !mip_faulted) {
      return;
    }
    // The load rode a lost command buffer: its content is garbage while
    // base state says up-to-date. Invalidate through the ONE authority —
    // the watch fires, the texture goes outdated, and the next use reloads
    // from intact RAM (also evicts any overlapping legacy entry via its own
    // global watch). The region's content hash is dropped too: RAM did not
    // change, so RevalidateRegions would otherwise match the hash and skip
    // the reload, serving the garbage until the guest rewrote the bytes
    // (conformance "Base and mip provenance ... either fault refuses").
    TextureKey key = texture.GetKey();
    if (base_faulted) {
      GetSharedMemory().RangeWrittenByGPU(
          key.base_page << 12, texture.GetGuestBaseSize());
      metal_texture.last_load_submission = 0;
      metal_texture.base_hash = 0;
    }
    if (mip_faulted) {
      GetSharedMemory().RangeWrittenByGPU(
          key.mip_page << 12, texture.GetGuestMipsSize());
      metal_texture.last_mip_load_submission = 0;
      metal_texture.mips_hash = 0;
    }
  });
}

void MetalSharedTextureCache::WriteAllCachedTexturesToTrace(
    TraceWriter& trace_writer) {
  // No trace writer exists in this tree; the fork recorded each cached texture's guest ranges here.
  (void)trace_writer;
}

void MetalSharedTextureCache::AppendAllTextures(
    std::vector<void*>& out) const {
  for (const BindingSlot& binding_slot : binding_slots_) {
    if (binding_slot.sampled_view) {
      out.push_back(binding_slot.sampled_view);
    }
  }
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
