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
#include <rex/xenos/texture_util.h>
#include <rex/xenos/register_file.h>
#include "gpu/xenos/shared_memory.h"
#include <rex/xenos/xenos.h>
#include <rex/hash.h>
#include <rex/platform/lock_order.h>
#include <rex/types.h>

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
class TextureCache : public rex::NonCopyable {
 public:
  /**
   * Limited by the half pixel offset fill in the resolve shaders covering up to 3 pixels, the bit
   * counts for passing the scale to shaders, and the 490 MB EDRAM buffer fitting the Direct3D 12
   * minimum of 128 * 2^20 texels per buffer binding (counted as R32 for a byte address buffer).
   */
  static constexpr u32 kMaxDrawResolutionScaleAlongAxis = 7;

  virtual ~TextureCache();

  /**
   * Reports whether the scale is not smaller than the requested one.
   *
   * @param x_out Receives the horizontal scale.
   * @param y_out Receives the vertical scale.
   * @return True when neither axis was clamped.
   */
  static bool GetConfigDrawResolutionScale(u32& x_out, u32& y_out);
  u32 draw_resolution_scale_x() const { return draw_resolution_scale_x_; }
  u32 draw_resolution_scale_y() const { return draw_resolution_scale_y_; }
  bool IsDrawResolutionScaled() const {
    return draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1;
  }

  virtual void ClearCache();

  virtual void CompletedSubmissionUpdated(u64 completed_submission_index);
  virtual void BeginSubmission(u64 new_submission_index);
  virtual void BeginFrame();

  void MarkRangeAsResolved(u32 start_unscaled, u32 length_unscaled);
  /**
   * Allocates the memory backing the range in the scaled resolve address space.
   *
   * @return True when the memory is allocated.
   */
  virtual bool EnsureScaledResolveMemoryCommitted(u32 /*start_unscaled*/, u32 /*length_unscaled*/,
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
    return (binding->texture && binding->texture->key().scaled_resolve) ||
           (binding->texture_signed && binding->texture_signed->key().scaled_resolve);
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

    TextureKey() { MakeInvalid(); }
    void MakeInvalid() {
      // Zero everything, including the padding, for a stable hash.
      std::memset(this, 0, sizeof(*this));
    }

    using Hasher = rex::XXHasher<TextureKey>;
    bool operator==(const TextureKey& key) const { return !std::memcmp(this, &key, sizeof(*this)); }
    bool operator!=(const TextureKey& key) const { return !(*this == key); }

    u32 GetWidth() const { return width_minus_1 + 1; }
    u32 GetHeight() const { return height_minus_1 + 1; }
    u32 GetDepthOrArraySize() const { return depth_or_array_size_minus_1 + 1; }

    xenos::TextureGuestLayout GetGuestLayout() const {
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

  class Texture : public rex::NonCopyable {
   public:
    virtual ~Texture();

    TextureCache& texture_cache() const { return texture_cache_; }

    const TextureKey& key() const { return key_; }

    const xenos::TextureGuestLayout& guest_layout() const { return guest_layout_; }
    u32 GetGuestBaseSize() const { return guest_layout().base.level_data_extent_bytes; }
    u32 GetGuestMipsSize() const { return guest_layout().mips_total_extent_bytes; }

    /** Reports whether guest memory is tiled as 3D although the host texture is 2D (3D as 2D
     * wrappers). */
    bool force_load_3d_tiling() const { return force_load_3d_tiling_; }
    void SetForceLoad3DTiling(bool force) { force_load_3d_tiling_ = force; }

    u64 last_usage_submission_index() const { return last_usage_submission_index_; }
    u64 last_usage_time() const { return last_usage_time_; }
    static constexpr u32 kOutdatedBitBase = UINT32_C(1) << 0;
    static constexpr u32 kOutdatedBitMips = UINT32_C(1) << 1;
    u32 outdated_mask() const { return outdated_mask_.load(std::memory_order_acquire); }

    bool base_outdated(
        [[maybe_unused]] const std::unique_lock<rex::platform::RankedMutex>& watch_lock) const {
      return base_outdated_;
    }
    bool mips_outdated(
        [[maybe_unused]] const std::unique_lock<rex::platform::RankedMutex>& watch_lock) const {
      return mips_outdated_;
    }
    void MakeUpToDateAndWatch(const std::unique_lock<rex::platform::RankedMutex>& watch_lock);

    void WatchCallback(bool is_mip);

    /**
     * Updates the last usage and moves the texture to the end of the LRU usage queue. Must be
     * called whenever GPU work references the texture, so it is not destroyed while in use.
     */
    void MarkAsUsed();

    void LogAction(const char* action) const;

   protected:
    /**
     * Creates the texture.
     *
     * @param texture_cache Owning texture cache.
     * @param key Key identifying the texture.
     * @param track_usage Adds the texture to the LRU list when true.
     */
    explicit Texture(TextureCache& texture_cache, const TextureKey& key, bool track_usage = true);

    void SetHostMemoryUsage(u64 new_host_memory_usage) {
      texture_cache_.UpdateTexturesTotalHostMemoryUsage(new_host_memory_usage, host_memory_usage_);
      host_memory_usage_ = new_host_memory_usage;
    }

   private:
    TextureCache& texture_cache_;

    TextureKey key_;

    xenos::TextureGuestLayout guest_layout_;

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
   * Without resolution scaling, shared memory regions of power of two sizes map to the same
   * portions of the 512 MB of physical memory, so a 64 KB aligned host region is 64 KB aligned in
   * guest memory. Tiled textures keep 32x32x4 block tiles resident as whole tiles, including
   * overflowing tiles when width exceeds pitch, so reading a 32x32 aligned portion is safe if any
   * location in it is within the texture bounds. Linear textures have a pitch aligned to 256 bytes
   * and an unaligned row count (unless the mip tail is loaded), and the overflowing last row when
   * width exceeds pitch is resident up to its last texel. Since the same shaders load tiled, linear
   * and 1D textures, a min(256 bytes, 32 blocks) aligned portion along X is safe to read within a
   * row of blocks with a single bounds check for the whole portion. Shaders should therefore read
   * power of two block counts, at most 16 blocks of 16bpb or 32 blocks of 8bpb or smaller per
   * thread under one "if (x < width)".
   *
   * With resolution scaling, buffer tile boundaries are not power of two aligned in guest
   * addresses. 2x2 scaling happens to align (host unit address = guest unit address << 2, likewise
   * 2x1 and 1x2), but with 3x scaling a 64 KB host tile covers 7281.777 guest bytes. This affects
   * the width > pitch case for linear textures, which would need the width overestimated when
   * mapping tiles but not on the guest memory extent side, to avoid uploading unallocated memory.
   * It does not matter because resolves target only tiled textures, so linear textures are never
   * resolution scaled. Tiled textures load the potentially referenced 32x32 block tiles whole, so
   * if any block in a tile is available the whole tile is.
   *
   * Destination writes to the linear buffer use host_x_blocks_per_thread, the count of pixels
   * writable without bounds checking, and the destination pitch is overaligned to it if needed.
   *
   * Resolution scaling applies to 8 byte portions of memory for 8bpp textures and 16 byte portions
   * for higher bit depths, the sizes where contiguous texels in memory are contiguous along X, so
   * 64 bit and 128 bit loads and stores can untile regardless of the scale.
   */
  struct LoadConstants {
    u32 is_tiled_3d_endian_scale;
    u32 guest_offset; /**< Base offset in bytes, resolution scaled. */
    /** Row pitch aligned to 32 and unscaled, in blocks for tiled textures and bytes for linear. */
    u32 guest_pitch_aligned;
    u32 guest_z_stride_block_rows_aligned; /**< 3D textures only, aligned to 32 and unscaled. */

    // - std140 vector boundary -

    u32 size_blocks[3]; /**< Aligned to tile dimensions for a packed mip tail. Resolution scaled. */
    u32 host_offset;    /**< Base offset in bytes. */

    // - std140 vector boundary -

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
    /**
     * Packed TextureSign values, 2 bits per component, with the fetch constant destination swizzle
     * applied.
     */
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

  explicit TextureCache(const xenos::RegisterFile& register_file, SharedMemory& shared_memory,
                        u32 draw_resolution_scale_x, u32 draw_resolution_scale_y);

  const xenos::RegisterFile& register_file() const { return register_file_; }
  SharedMemory& shared_memory() const { return shared_memory_; }

  /**
   * Destroys all textures. Implementations call this from their destructor when textures reference
   * the implementation in their own destructors.
   *
   * @param from_destructor True when called from a destructor.
   */
  void DestroyAllTextures(bool from_destructor = false);

  /**
   * Reports whether the signed version of the texture has a different host representation than the
   * unsigned one, such as a fixed point texture emulated with a larger host pixel format.
   *
   * @param key Key whose format and signedness are examined.
   */
  virtual bool IsSignedVersionSeparateForFormat(TextureKey /*key*/) const { return false; }
  /**
   * Reports whether the format supports scaled resolve. Tiling and dimensions are checked
   * externally, so implementations consider only format related fields.
   *
   * @param key Key whose format related fields are examined.
   */
  virtual bool IsScaledResolveSupportedForFormat(TextureKey /*key*/) const { return false; }
  /**
   * Returns the host swizzle for the format. Formats with fewer than 4 components replicate the
   * last component into the missing ones, as shader compilers do for unused operand components
   * (.xxxx, .xyyy, .xyzz, .xyzw). DXT3A and DXT5A use RRRR.
   *
   * @param key Key of the texture to swizzle.
   * @return Packed host swizzle.
   */
  virtual u32 GetHostFormatSwizzle(TextureKey key) const = 0;

  virtual u32 GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const = 0;
  virtual u32 GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const = 0;

  /**
   * Creates a texture with exactly this key, which must not be modified. Returns nullptr when the
   * implementation does not support the key or creation fails at runtime.
   *
   * @param key Key of the texture to create.
   * @return New texture or nullptr.
   */
  virtual std::unique_ptr<Texture> CreateTexture(TextureKey key) = 0;

  /**
   * Finds or creates the texture for the key. Returns nullptr when the key is unsupported or
   * creation failed, in which case recreation should be attempted occasionally.
   *
   * @param key Key of the texture, possibly adjusted to fit host limits.
   * @return Texture or nullptr.
   */
  Texture* FindOrCreateTexture(TextureKey key);

  static const LoadShaderInfo& GetLoadShaderInfo(LoadShaderIndex load_shader_index) {
    REX_ASSERT(load_shader_index < kLoadShaderCount);
    return load_shader_info_[load_shader_index];
  }
  bool LoadTextureData(Texture& texture);
  /**
   * Writes the texture data for the base, the mips or both, but not neither, from the shared memory
   * or the scaled resolve memory. Shared memory management is done outside, so the implementation
   * only loads the data into the texture.
   *
   * @param texture Texture receiving the data.
   * @param load_base Loads the base level.
   * @param load_mips Loads the mips.
   * @return True when the data was loaded.
   */
  virtual bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                     bool load_mips) = 0;

  /**
   * Converts a texture fetch constant to a texture key, normalizing and validating the values or
   * producing an invalid key, and gets the signedness after the guest swizzle.
   *
   * @param fetch Texture fetch constant.
   * @param key_out Receives the key.
   * @param swizzled_signs_out Receives the signedness, may be null.
   */
  static void BindingInfoFromFetchConstant(const xenos::TextureFetch& fetch, TextureKey& key_out,
                                           u8* swizzled_signs_out);

  /**
   * Makes all texture bindings invalid. Textures requested afterwards are created or untiled again,
   * retrying after an error.
   *
   * @param from_destructor True when called from a destructor.
   */
  void ResetTextureBindings(bool from_destructor = false);

  const TextureBinding* GetValidTextureBinding(u32 fetch_constant_index) const {
    const TextureBinding& binding = texture_bindings_[fetch_constant_index];
    return binding.key.is_valid ? &binding : nullptr;
  }
  /** Called when a texture binding changes, for the implementation to update its dependent state.
   */
  virtual void UpdateTextureBindingsImpl(u32 /*fetch_constant_mask*/) {}

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
   * @return True when any page in the range contains scaled resolve data.
   */
  bool IsRangeScaledResolved(u32 start_unscaled, u32 length_unscaled);
  /**
   * Global shared memory invalidation callback that unmarks scaled resolved texture data.
   *
   * @param context TextureCache instance.
   * @param address_first First invalidated address.
   * @param address_last Last invalidated address.
   * @param invalidated_by_gpu True when the GPU caused the invalidation.
   */
  static void ScaledResolveGlobalWatchCallbackThunk(void* context, u32 address_first,
                                                    u32 address_last, bool invalidated_by_gpu);
  void ScaledResolveGlobalWatchCallback(u32 address_first, u32 address_last,
                                        bool invalidated_by_gpu);

  const xenos::RegisterFile& register_file_;
  SharedMemory& shared_memory_;
  u32 draw_resolution_scale_x_;
  u32 draw_resolution_scale_y_;

  static const LoadShaderInfo load_shader_info_[kLoadShaderCount];

  std::unique_ptr<u32[]>
      /**
       * Bit per 4 KB physical page holding scaled resolve data, 32 bit words since parts are sent
       * to shaders.
       */
      scaled_resolve_pages_;
  u64 scaled_resolve_pages_l2_[SharedMemory::kBufferSize >>
                               /**
                                * Second level for rejecting non scaled textures, shifted by 12
                                * for 4 KB pages, 5 for level 1 words and 6 for level 2 words.
                                */
                               (12 + 5 + 6)];

  SharedMemory::GlobalWatchHandle scaled_resolve_global_watch_handle_ = nullptr;

  u64 current_submission_index_ = 0;
  u64 current_submission_time_ = 0;

  std::unordered_map<TextureKey, std::unique_ptr<Texture>, TextureKey::Hasher> textures_;

  u64 textures_total_host_memory_usage_ = 0;

  Texture* texture_used_first_ = nullptr;
  Texture* texture_used_last_ = nullptr;

  std::atomic<bool> texture_became_outdated_{
      /**
       * Set by a memory watch, forcing a recheck of textures regardless of fetch constant changes.
       */
      false};

  std::array<TextureBinding, xenos::kTextureFetchConstantCount> texture_bindings_;
  u32 texture_bindings_in_sync_ = 0;
};

}  // namespace rex::gpu
