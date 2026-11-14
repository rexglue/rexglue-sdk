/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/utility.h>
#include <rex/xenos/texture.h>
#include <rex/xenos/register.h>
#include "gpu/xenos/shared_memory.h"
#include "gpu/xenos/xenos_settings.h"
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/hash.h>
#include <rex/platform/lock_order.h>
#include <rex/types.h>
#include "gpu/xenos/xenos_export.h"

namespace rex::gpu {

/**
 * Manages host copies of guest textures, untiling and converting format and endianness from the
 * shared memory, and handling invalidation.
 *
 * A texture with a base address and no mip address has only the largest level. With different
 * nonzero base and mip addresses the host texture has mip_max_level+1 levels and mip_min_level is
 * sampler state only. With a mip address and a base address of 0 or equal to the mip address it is
 * mipmapped with min/max LOD clamped to at least 1.
 */
class REX_GPU_XENOS_API TextureCache : public rex::NonCopyable {
 public:
  /**
   * The half pixel offset fill in the resolve shaders covers up to three pixels. The bit counts
   * for passing the scale to shaders limit it. The 490 MB EDRAM buffer must fit the Direct3D 12
   * minimum of 128 * 2^20 texels per buffer binding.
   */
  static constexpr u32 kMaxDrawResolutionScaleAlongAxis = 7;

  virtual ~TextureCache();

  /**
   * Clamps the requested draw resolution scale to the supported range.
   *
   * @param x_out Receives the horizontal scale.
   * @param y_out Receives the vertical scale.
   * @return True when neither axis was clamped.
   */
  static bool GetConfigDrawResolutionScale(u32 requested_x, u32 requested_y, u32& x_out,
                                           u32& y_out);
  const XenosSettings& GetXenosSettings() const { return settings_; }
  u32 GetDrawResolutionScaleX() const { return draw_resolution_scale_x_; }
  u32 GetDrawResolutionScaleY() const { return draw_resolution_scale_y_; }
  bool IsDrawResolutionScaled() const {
    return draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1;
  }

  virtual void ClearCache();

  virtual void CompletedSubmissionUpdated(u64 completed_submission_index);
  virtual void BeginSubmission(u64 new_submission_index);
  virtual void BeginFrame();

  void MarkRangeAsResolved(u32 start_unscaled, u32 length_unscaled);
  /** Allocates the memory backing the range in the scaled resolve address space. */
  virtual bool CommitScaledResolveMemory(u32 /*start_unscaled*/, u32 /*length_unscaled*/,
                                         u32 /*length_scaled_alignment_log2*/ = 0) {
    return false;
  }

  static u32 GuestToHostSwizzle(u32 guest_swizzle, u32 host_format_swizzle);

  void TextureFetchConstantWritten(u32 index) { TextureFetchConstantsWritten(index, index); }
  void TextureFetchConstantsWritten(u32 first_index, u32 last_index) {
    if (first_index > last_index) {
      u32 swap_index = first_index;
      first_index = last_index;
      last_index = swap_index;
    }
    if (first_index > 31) {
      return;
    }
    if (last_index > 31) {
      last_index = 31;
    }
    u32 bit_count = last_index - first_index + 1;
    u32 mask = bit_count == 32 ? UINT32_MAX : ((UINT32_C(1) << bit_count) - 1) << first_index;
    texture_bindings_in_sync_ &= ~mask;
  }

  virtual void RequestTextures(u32 used_texture_mask);

  // Active textures are those of the latest RequestTextures call.

  u32 GetActiveTextureHostSwizzle(u32 fetch_constant_index) const {
    const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
    return binding ? binding->host_swizzle : xenos::REXGPU_SWIZZLE_0000;
  }
  u8 GetActiveTextureSwizzledSigns(u32 fetch_constant_index) const {
    const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
    return binding ? binding->swizzled_signs : kSwizzledSignsUnsigned;
  }
  bool IsActiveTextureResolutionScaled(u32 fetch_constant_index) const {
    const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
    if (!binding) {
      return false;
    }
    return (binding->texture && binding->texture->GetKey().scaled_resolve) ||
           (binding->texture_signed && binding->texture_signed->GetKey().scaled_resolve);
  }

 protected:
  struct TextureKey {
    /** Physical 4 KB page of the base mip, without the A/C/E address range prefix. */
    u32 base_page : 17;
    xenos::DataDimension dimension : 2;
    u32 width_minus_1 : 13;

    u32 height_minus_1 : 13;
    u32 tiled : 1;
    u32 packed_mips : 1;
    u32 mip_page : 17; /**< Physical 4 KB page of mip 1 and smaller. */

    /** (Layers for stacked and 3D, 6 for cube, 1 for others) - 1. */
    u32 depth_or_array_size_minus_1 : 10;
    u32 pitch : 9;
    u32 mip_max_level : 4;
    xenos::TextureFormat format : 6;
    xenos::Endian endianness : 2;
    /** Signed with a host representation different from the unsigned view. */
    u32 signed_separate : 1;

    u32 scaled_resolve : 1;
    u32 is_valid : 1;
    u32 padding : 30;

    TextureKey() { Invalidate(); }
    void Invalidate() {
      // Zero everything, including the padding, for a stable hash.
      std::memset(this, 0, sizeof(*this));
    }

    using Hasher = rex::XXHasher<TextureKey>;
    bool operator==(const TextureKey& key) const { return !std::memcmp(this, &key, sizeof(*this)); }
    bool operator!=(const TextureKey& key) const { return !(*this == key); }

    u32 GetWidth() const { return width_minus_1 + 1; }
    u32 GetHeight() const { return height_minus_1 + 1; }
    u32 GetDepthOrArraySize() const { return depth_or_array_size_minus_1 + 1; }

    xenos::GuestTextureLayout GetGuestLayout() const {
      return xenos::GetGuestTextureLayout(dimension, pitch, GetWidth(), GetHeight(),
                                          GetDepthOrArraySize(), tiled, format, packed_mips,
                                          base_page != 0, mip_max_level);
    }

    static const char* GetLogDimensionName(xenos::DataDimension dimension);
    const char* GetLogDimensionName() const { return GetLogDimensionName(dimension); }
    void LogAction(const char* action) const;
  };
  static_assert(std::is_trivially_copyable_v<TextureKey> &&
                std::has_unique_object_representations_v<TextureKey>);

  class REX_GPU_XENOS_API Texture : public rex::NonCopyable {
   public:
    virtual ~Texture();

    TextureCache& GetTextureCache() const { return texture_cache_; }

    const TextureKey& GetKey() const { return key_; }

    const xenos::GuestTextureLayout& GetGuestLayout() const { return guest_layout_; }
    u32 GetGuestBaseSize() const { return GetGuestLayout().base.level_data_extent_bytes; }
    u32 GetGuestMipsSize() const { return GetGuestLayout().mips_total_extent_bytes; }

    /** Reports whether guest memory is tiled as 3D although the host texture is 2D wrapping 3D. */
    bool IsForceLoad3DTiling() const { return force_load_3d_tiling_; }
    void SetForceLoad3DTiling(bool force) { force_load_3d_tiling_ = force; }

    u64 GetLastUsageSubmissionIndex() const { return last_usage_submission_index_; }
    u64 GetLastUsageTime() const { return last_usage_time_; }
    static constexpr u32 kOutdatedBitBase = UINT32_C(1) << 0;
    static constexpr u32 kOutdatedBitMips = UINT32_C(1) << 1;
    u32 GetOutdatedMask() const { return outdated_mask_.load(std::memory_order_acquire); }

    bool IsBaseOutdated([[maybe_unused]] const ScopeGuard& watch_lock) const {
      return base_outdated_;
    }
    bool IsMipsOutdated([[maybe_unused]] const ScopeGuard& watch_lock) const {
      return mips_outdated_;
    }
    void UpdateAndWatch(const ScopeGuard& watch_lock);

    void WatchCallback(bool is_mip);

    /** Updates the last usage and moves the texture to the end of the LRU usage queue. */
    void MarkAsUsed();

    void LogAction(const char* action) const;

   protected:
    /** Adds the texture to the LRU list when track_usage is true. */
    explicit Texture(TextureCache& texture_cache, const TextureKey& key, bool track_usage = true);

    void SetHostMemoryUsage(u64 new_host_memory_usage) {
      texture_cache_.UpdateTexturesTotalHostMemoryUsage(new_host_memory_usage, host_memory_usage_);
      host_memory_usage_ = new_host_memory_usage;
    }

   private:
    TextureCache& texture_cache_;

    TextureKey key_;

    xenos::GuestTextureLayout guest_layout_;

    u64 host_memory_usage_ = 0;

    u64 last_usage_submission_index_;
    u64 last_usage_time_;
    Texture* used_previous_;
    Texture* used_next_;
    bool in_usage_list_;
    bool force_load_3d_tiling_ = false;

    // Accessed within the global critical region to synchronize with shared memory.
    bool base_outdated_ = false;
    bool mips_outdated_ = false;
    std::atomic<u32> outdated_mask_{0};
    SharedMemory::WatchHandle base_watch_handle_ = nullptr;
    SharedMemory::WatchHandle mips_watch_handle_ = nullptr;
  };

  /**
   * Load shader constants. Load shaders obey these access rules.
   *
   * Source reads from the shared memory or the scaled resolve buffer must not touch unmapped
   * regions of the sparse (Direct3D 12 tiled) buffer, since tiled resources tier 1 hardware faults
   * on it. Buffer tiles are 64 KB on Direct3D 12, and Vulkan has its own sparse binding alignment.
   *
   * Tiled textures keep 32x32x4 block tiles resident whole, so reading a 32x32 aligned portion is
   * safe if any location in it is within the texture bounds. Shaders should read power of two
   * block counts, at most 16 blocks of 16bpb or 32 blocks of 8bpb or smaller per thread under one
   * "if (x < width)".
   *
   * Destination writes to the linear buffer use host_x_blocks_per_thread, the count of pixels
   * writable without bounds checking, and the destination pitch is overaligned to it if needed.
   *
   * Resolution scaling applies to 8 byte portions of memory for 8bpp textures and 16 byte portions
   * for higher bit depths, so 64 bit and 128 bit loads and stores can untile regardless of scale.
   */
  struct LoadConstants {
    u32 is_tiled_3d_endian_scale;
    u32 guest_offset; /**< Base offset in bytes, resolution scaled. */
    /** Row pitch aligned to 32 and unscaled, in blocks for tiled textures and bytes for linear. */
    u32 guest_pitch_aligned;
    u32 guest_z_stride_block_rows_aligned; /**< 3D textures only, aligned to 32 and unscaled. */

    // std140 vector boundary

    u32 size_blocks[3]; /**< Aligned to tile dimensions for a packed mip tail. Resolution scaled. */
    u32 host_offset;    /**< Base offset in bytes. */

    // std140 vector boundary

    u32 host_pitch;
    u32 height_texels;
  };

  static constexpr u32 kLoadGuestXThreadsPerGroupLog2 = 2;
  static constexpr u32 kLoadGuestYBlocksPerGroupLog2 = 5;

  enum LoadShaderIndex : u8 {
    kLoadShaderIndex8bpb,
    kLoadShaderIndex16bpb,
    kLoadShaderIndex32bpb,
    kLoadShaderIndex64bpb,
    kLoadShaderIndex128bpb,
    kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
    kLoadShaderIndexR5G6B5ToB5G6R5,
    kLoadShaderIndexR5G6B5ToRGBA8,
    kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
    kLoadShaderIndexRGBA4ToBGRA4,
    kLoadShaderIndexRGBA4ToARGB4,
    kLoadShaderIndexRGBA4ToRGBA8,
    kLoadShaderIndexGBGR8ToGRGB8,
    kLoadShaderIndexGBGR8ToRGB8,
    kLoadShaderIndexBGRG8ToRGBG8,
    kLoadShaderIndexBGRG8ToRGB8,
    kLoadShaderIndexR10G11B11ToRGBA16,
    kLoadShaderIndexR10G11B11ToRGBA16SNorm,
    kLoadShaderIndexR11G11B10ToRGBA16,
    kLoadShaderIndexR11G11B10ToRGBA16SNorm,
    kLoadShaderIndexR16UNormToFloat,
    kLoadShaderIndexR16SNormToFloat,
    kLoadShaderIndexRG16UNormToFloat,
    kLoadShaderIndexRG16SNormToFloat,
    kLoadShaderIndexRGBA16UNormToFloat,
    kLoadShaderIndexRGBA16SNormToFloat,
    kLoadShaderIndexDXT1ToRGBA8,
    kLoadShaderIndexDXT3ToRGBA8,
    kLoadShaderIndexDXT5ToRGBA8,
    kLoadShaderIndexDXNToRG8,
    kLoadShaderIndexDXT3A,
    kLoadShaderIndexDXT3AAs1111ToBGRA4,
    kLoadShaderIndexDXT3AAs1111ToARGB4,
    kLoadShaderIndexDXT5AToR8,
    kLoadShaderIndexCTX1,
    kLoadShaderIndexDepthUnorm,
    kLoadShaderIndexDepthFloat,

    kLoadShaderCount,
    kLoadShaderIndexUnknown = kLoadShaderCount,
  };

  struct LoadShaderInfo {
    u32 source_bpe_log2; /**< Log2 of the element size in bytes of the source (guest) binding. */
    u32 dest_bpe_log2; /**< Log2 of the element size in bytes of the destination (host) binding. */
    /**
     * Bytes written per host resolution scaled block (a guest block, or a host texel when
     * decompressing).
     */
    u32 bytes_per_host_block;
    /** Log2 of the guest resolution scaled blocks along X loaded per thread. */
    u32 guest_x_blocks_per_thread_log2;

    u32 GetGuestXBlocksPerGroupLog2() const {
      return kLoadGuestXThreadsPerGroupLog2 + guest_x_blocks_per_thread_log2;
    }
  };

  static constexpr u8 kSwizzledSignsUnsigned =
      static_cast<u8>(xenos::TextureSign::kUnsigned) * static_cast<u8>(0b01010101);

  struct TextureBinding {
    TextureKey key;
    u32 host_swizzle;
    /** Packed TextureSign values, 2 bits per component, after the fetch constant swizzle. */
    u8 swizzled_signs;
    Texture* texture;        /**< Unsigned version, or signed if the data is identical. */
    Texture* texture_signed; /**< Signed version, when its host data differs. */

    TextureBinding() { Reset(); }

    void Reset() {
      std::memset(this, 0, sizeof(*this));
      host_swizzle = xenos::REXGPU_SWIZZLE_0000;
      swizzled_signs = kSwizzledSignsUnsigned;
    }
  };

  explicit TextureCache(const xenos::RegisterFile& register_file, const XenosSettings& settings,
                        SharedMemory& shared_memory, u32 draw_resolution_scale_x,
                        u32 draw_resolution_scale_y);

  const xenos::RegisterFile& GetRegisterFile() const { return register_file_; }
  SharedMemory& GetSharedMemory() const { return shared_memory_; }

  /**
   * @param from_destructor True when called from a destructor.
   */
  void DestroyAllTextures(bool from_destructor = false);

  /**
   * Reports whether the signed version of the texture has a different host representation than the
   * unsigned one, such as a fixed point texture emulated with a larger host pixel format.
   */
  virtual bool IsSignedVersionSeparateForFormat(TextureKey /*key*/) const { return false; }
  /**
   * Reports whether the format supports scaled resolve. Tiling and dimensions are checked by the
   * caller, so implementations consider only format related fields.
   */
  virtual bool IsScaledResolveSupportedForFormat(TextureKey /*key*/) const { return false; }
  /**
   * Formats with fewer than 4 components replicate the last component into the missing ones (.xxxx,
   * .xyyy, .xyzz, .xyzw), and DXT3A and DXT5A use RRRR.
   *
   * @return Packed host swizzle.
   */
  virtual u32 GetHostFormatSwizzle(TextureKey key) const = 0;

  virtual u32 GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const = 0;
  virtual u32 GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const = 0;

  /**
   * Creates a texture with exactly this key, which must not be modified.
   *
   * @return New texture, or null if the key is unsupported or creation fails.
   */
  virtual std::unique_ptr<Texture> CreateTexture(TextureKey key) = 0;

  /**
   * Finds or creates the texture for the key. Recreation after a failure should be attempted
   * occasionally.
   *
   * @param key Key of the texture, possibly adjusted to fit host limits.
   * @return Texture, or null if the key is unsupported or creation failed.
   */
  Texture* FindOrCreateTexture(TextureKey key);

  static const LoadShaderInfo& GetLoadShaderInfo(LoadShaderIndex load_shader_index) {
    REX_ASSERT(load_shader_index < kLoadShaderCount);
    return kLoadShaderInfo[load_shader_index];
  }
  bool LoadTextureData(Texture& texture);
  /**
   * Loads the base level, the mips or both from the shared memory or the scaled resolve memory.
   * The caller manages shared memory, so the implementation only loads the data into the texture.
   */
  virtual bool LoadHostTextureDataFromResidentMemory(Texture& texture, bool load_base,
                                                     bool load_mips) = 0;

  /**
   * Converts a texture fetch constant to a texture key, normalizing and validating the values or
   * producing an invalid key, and gets the signedness after the guest swizzle.
   *
   * @param key_out Receives the key.
   * @param swizzled_signs_out Receives the signedness, may be null.
   */
  void GetTextureKeyFromFetchConstant(const xenos::TextureFetch& fetch, TextureKey& key_out,
                                      u8* swizzled_signs_out) const;

  /**
   * Makes all texture bindings invalid, so textures requested afterwards are created or untiled
   * again, retrying after an error.
   *
   * @param from_destructor True when called from a destructor.
   */
  void ResetTextureBindings(bool from_destructor = false);

  const TextureBinding* GetValidTextureBinding(u32 fetch_constant_index) const {
    const TextureBinding& binding = texture_bindings_[fetch_constant_index];
    return binding.key.is_valid ? &binding : nullptr;
  }
  /** Called when a texture binding changes, for the implementation to update dependent state. */
  virtual void UpdateHostTextureBindings(u32 /*fetch_constant_mask*/) {}

 private:
  struct PendingTextureLoad {
    Texture* texture = nullptr;
    bool load_base = false;
    bool load_mips = false;
  };
  struct PendingSharedMemoryRange {
    u32 start = 0;
    u32 length = 0;
  };
  bool PrepareTextureLoad(Texture& texture, PendingTextureLoad& pending_load_out,
                          PendingSharedMemoryRange* pending_ranges_out,
                          size_t& pending_range_count_out);
  bool CommitPreparedTextureLoad(const PendingTextureLoad& pending_load);

  void UpdateTexturesTotalHostMemoryUsage(u64 add, u64 subtract);

  static void WatchCallback(void* context, void* data, u64 argument, bool invalidated_by_gpu);

  /**
   * Reports whether any page in the range contains scaled resolve data.
   *
   * @param start_unscaled Start address, unscaled.
   * @param length_unscaled Length in bytes, unscaled.
   */
  bool IsRangeScaledResolved(u32 start_unscaled, u32 length_unscaled);
  /**
   * Global shared memory invalidation callback that unmarks scaled resolved texture data.
   *
   * @param address_first First invalidated address.
   * @param address_last Last invalidated address.
   * @param invalidated_by_gpu True when the GPU caused the invalidation.
   */
  static void ScaledResolveGlobalWatchCallbackThunk(void* context, u32 address_first,
                                                    u32 address_last, bool invalidated_by_gpu);
  void ScaledResolveGlobalWatchCallback(u32 address_first, u32 address_last,
                                        bool invalidated_by_gpu);

  const xenos::RegisterFile& register_file_;
  const XenosSettings& settings_;
  SharedMemory& shared_memory_;
  u32 draw_resolution_scale_x_;
  u32 draw_resolution_scale_y_;

  static const LoadShaderInfo kLoadShaderInfo[kLoadShaderCount];

  /**
   * Bit per 4 KB physical page holding scaled resolve data, 32 bit words since parts are sent to
   * shaders.
   */
  std::unique_ptr<u32[]> scaled_resolve_pages_;
  /**
   * Second level for rejecting non scaled textures, shifted by 12 for 4 KB pages, 5 for level 1
   * words and 6 for level 2 words.
   */
  u64 scaled_resolve_pages_l2_[SharedMemory::kBufferSize >> (12 + 5 + 6)];

  SharedMemory::GlobalWatchHandle scaled_resolve_global_watch_handle_ = nullptr;

  u64 current_submission_index_ = 0;
  u64 current_submission_time_ = 0;

  std::unordered_map<TextureKey, std::unique_ptr<Texture>, TextureKey::Hasher> textures_;

  u64 textures_total_host_memory_usage_ = 0;

  Texture* texture_used_first_ = nullptr;
  Texture* texture_used_last_ = nullptr;

  /** Set by a memory watch, forcing a recheck of textures regardless of fetch constant changes. */
  std::atomic<bool> texture_became_outdated_{false};

  std::array<TextureBinding, xenos::kTextureFetchConstantCount> texture_bindings_;
  u32 texture_bindings_in_sync_ = 0;
};

}  // namespace rex::gpu
