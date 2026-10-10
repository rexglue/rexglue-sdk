/**
 * @file        xenos/texture_info.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/gpu/texture_info.h (Ben Vanik, 2014).
 */

#pragma once

#include <memory>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/types.h>
#include <rex/xenos/xenos.h>

namespace rex::xenos {

/**
 * Maps a resampling or gamma control variant to the format it stores.
 *
 * @param texture_format The format to map.
 * @return The base format, or texture_format when it has no base variant.
 */
inline xenos::TextureFormat GetBaseFormat(xenos::TextureFormat texture_format) {
  switch (texture_format) {
    case xenos::TextureFormat::k_16_EXPAND:
      return xenos::TextureFormat::k_16_FLOAT;
    case xenos::TextureFormat::k_16_16_EXPAND:
      return xenos::TextureFormat::k_16_16_FLOAT;
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
      return xenos::TextureFormat::k_16_16_16_16_FLOAT;
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      return xenos::TextureFormat::k_8_8_8_8;
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return xenos::TextureFormat::k_DXT1;
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return xenos::TextureFormat::k_DXT2_3;
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return xenos::TextureFormat::k_DXT4_5;
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      return xenos::TextureFormat::k_2_10_10_10;
    case xenos::TextureFormat::k_10_11_11_AS_16_16_16_16:
      return xenos::TextureFormat::k_10_11_11;
    case xenos::TextureFormat::k_11_11_10_AS_16_16_16_16:
      return xenos::TextureFormat::k_11_11_10;
    case xenos::TextureFormat::k_8_8_8_8_GAMMA_EDRAM:
      return xenos::TextureFormat::k_8_8_8_8;
    default:
      break;
  }

  return texture_format;
}

/**
 * @param format An uncompressed format.
 * @return The size of 1 texel in bytes, or 0 for an unhandled format.
 */
inline size_t GetTexelSize(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8:
      return 1;
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_4_4_4_4:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_8_8:
      return 2;
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      return 4;
    case xenos::TextureFormat::k_16_16_16_16:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
      return 8;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return 16;
    default:
      REX_UNHANDLED_CASE(format);
      return 0;
  }
}

/**
 * @param color_format The render target color format.
 * @return The texture format with the same numeric value.
 */
inline xenos::TextureFormat ColorFormatToTextureFormat(xenos::ColorFormat color_format) {
  return static_cast<xenos::TextureFormat>(color_format);
}

/**
 * @param depth_format The depth render target format.
 * @return The matching texture format, or TextureFormat(~0) for an unhandled format.
 */
inline xenos::TextureFormat DepthRenderTargetToTextureFormat(
    xenos::DepthRenderTargetFormat depth_format) {
  switch (depth_format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return xenos::TextureFormat::k_24_8;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return xenos::TextureFormat::k_24_8_FLOAT;
    default:
      REX_UNHANDLED_CASE(depth_format);
      return xenos::TextureFormat(~0);
  }
}

/** Storage class of a texture format. */
enum class FormatType : u8 {
  kResolvable,   /**< Uncompressed and also a ColorFormat. */
  kUncompressed, /**< Uncompressed, not a resolve or memory export target. */
  kCompressed,   /**< Block compressed. */
};

/** Block layout of a texture format. */
struct REX_API FormatInfo {
  xenos::TextureFormat format; /**< The format. */
  const char* name;            /**< Enumerator name of the format. */
  FormatType type;             /**< Storage class. */
  u32 block_width;             /**< Block width in texels. */
  u32 block_height;            /**< Block height in texels. */
  u32 bits_per_pixel;          /**< Bits per texel. */

  /** @return The size of 1 block in bytes. */
  u32 bytes_per_block() const { return block_width * block_height * bits_per_pixel / 8; }

  /**
   * @param gpu_format A TextureFormat value, below 64.
   * @return The table entry for gpu_format.
   */
  static const FormatInfo* Get(u32 gpu_format);

  /**
   * @param format The format to look up.
   * @return The table entry for format.
   */
  static const FormatInfo* Get(xenos::TextureFormat format) {
    return Get(static_cast<u32>(format));
  }
};

struct TextureInfo;

/** Texel and block dimensions of 1 mip level. */
struct REX_API TextureExtent {
  u32 pitch;         /**< Row pitch in texels. */
  u32 height;        /**< Height in texels, padded like the pitch. */
  u32 block_width;   /**< Visible blocks per row. */
  u32 block_height;  /**< Visible block rows. */
  u32 block_pitch_h; /**< Blocks per row including padding. */
  u32 block_pitch_v; /**< Block rows including padding. */
  u32 depth;         /**< Depth in layers. */

  /** @return The block count including padding in every dimension. */
  u32 all_blocks() const { return block_pitch_h * block_pitch_v * depth; }

  /** @return The block count including row padding but only the visible block rows. */
  u32 visible_blocks() const { return block_pitch_h * block_height * depth; }

  /**
   * @param format_info Block layout of the texture.
   * @param pitch Row pitch in texels.
   * @param height Height in texels.
   * @param depth Depth in layers.
   * @param is_tiled True for a tiled texture, false for a linear one.
   * @param is_guest True to pad to 32x32 block tiles and 256 byte linear rows as guest memory
   *        does, false for a packed host layout.
   *
   * @return The extent.
   */
  static TextureExtent Calculate(const FormatInfo* format_info, u32 pitch, u32 height, u32 depth,
                                 bool is_tiled, bool is_guest);

  /**
   * @param texture_info The texture, using its base level size.
   * @param is_guest True for the padded guest memory layout, false for a packed host layout.
   * @return The extent.
   */
  static TextureExtent Calculate(const TextureInfo* texture_info, bool is_guest);

  /**
   * @param other The extent to compare with.
   * @return True when every field matches.
   */
  bool operator==(const TextureExtent& other) const = default;
};

/** Guest memory ranges of a texture. */
struct TextureMemoryInfo {
  u32 base_address; /**< Address of the base level, or 0 when absent. */
  u32 base_size;    /**< Size of the base level in bytes. */
  u32 mip_address;  /**< Address of the mip levels, or 0 when absent. */
  u32 mip_size;     /**< Size of the mip levels in bytes. */

  /**
   * @param other The ranges to compare with.
   * @return True when every field matches.
   */
  bool operator==(const TextureMemoryInfo& other) const = default;
};

/** Texture description decoded from a fetch constant or a resolve. */
struct REX_API TextureInfo {
  xenos::TextureFormat format; /**< Texel format. */
  xenos::Endian endianness;    /**< Swap mode of the texel data. */

  xenos::DataDimension dimension; /**< Dimension, with 1D stored as 2D. */
  u32 width;                      /**< Width in texels minus 1. */
  u32 height;                     /**< Height in texels minus 1. */
  u32 depth;                      /**< Depth or array size minus 1. */
  u32 pitch;                      /**< Row pitch in texels. */
  u32 mip_min_level;              /**< First stored mip level. */
  u32 mip_max_level;              /**< Last stored mip level. */
  bool is_stacked;                /**< True for a 2D array. */
  bool is_tiled;                  /**< True for tiled data, false for linear. */
  bool has_packed_mips;           /**< True when small mips share a packed tile. */

  TextureMemoryInfo memory; /**< Guest memory ranges. */
  TextureExtent extent;     /**< Base level extent in the guest layout. */

  /** @return The block layout of format. */
  const FormatInfo* format_info() const { return FormatInfo::Get(static_cast<u32>(format)); }

  /** @return True when format is block compressed. */
  bool is_compressed() const { return format_info()->type == FormatType::kCompressed; }

  /** @return The number of stored mip levels. */
  u32 mip_levels() const { return 1 + (mip_max_level - mip_min_level); }

  /**
   * @param fetch The texture fetch constant.
   * @param out_info Receives the texture description.
   * @return True on success.
   */
  static bool Prepare(const xenos::TextureFetch& fetch, TextureInfo* out_info);

  /**
   * Describes a tiled 2D resolve destination with a single level.
   *
   * @param physical_address Guest address of the texture data.
   * @param texture_format Texel format.
   * @param endian Swap mode of the texel data.
   * @param pitch Row pitch in texels.
   * @param width Width in texels, at least 1.
   * @param height Height in texels, at least 1.
   * @param depth Depth in layers.
   * @param out_info Receives the texture description.
   * @return True on success.
   */
  static bool PrepareResolve(u32 physical_address, xenos::TextureFormat texture_format,
                             xenos::Endian endian, u32 pitch, u32 width, u32 height, u32 depth,
                             TextureInfo* out_info);

  /** @return The mip level count of a full chain for the texture size. */
  u32 GetMaxMipLevels() const;

  /**
   * @param mip The mip level.
   * @param is_guest True for the padded guest memory layout with power of 2 mip sizes, false for a
   *        packed host layout.
   *
   * @return The extent of the mip level.
   */
  const TextureExtent GetMipExtent(u32 mip, bool is_guest) const;

  /**
   * @param mip The mip level.
   * @param width Receives the width in texels, rounded to a power of 2 for levels above 0.
   * @param height Receives the height in texels, rounded to a power of 2 for levels above 0.
   */
  void GetMipSize(u32 mip, u32* width, u32* height) const;

  /**
   * @param mip The mip level.
   * @param offset_x Receives the X offset in blocks within a packed tile.
   * @param offset_y Receives the Y offset in blocks within a packed tile.
   * @param is_guest True for the padded guest memory layout, false for a packed host layout.
   * @return The guest address of the mip level, or 0 when the texture has no mip data.
   */
  u32 GetMipLocation(u32 mip, u32* offset_x, u32* offset_y, bool is_guest) const;

  /**
   * @param width Width of the packed level set in texels.
   * @param height Height of the packed level set in texels.
   * @param format_info Block layout of the texture.
   * @param packed_tile Index of the level within the packed tile.
   * @param offset_x Receives the X offset in blocks.
   * @param offset_y Receives the Y offset in blocks.
   * @return False when the level is too large to be packed, with both offsets set to 0.
   */
  static bool GetPackedTileOffset(u32 width, u32 height, const FormatInfo* format_info,
                                  u32 packed_tile, u32* offset_x, u32* offset_y);

  /**
   * @param packed_tile Index of the level within the packed tile.
   * @param offset_x Receives the X offset in blocks.
   * @param offset_y Receives the Y offset in blocks.
   * @return False when the texture has no packed mips or the level is not packed.
   */
  bool GetPackedTileOffset(u32 packed_tile, u32* offset_x, u32* offset_y) const;

  /** @return The 64 bit XXH3 hash of every field. */
  u64 hash() const;

  /**
   * @param other The description to compare with.
   * @return True when every field matches.
   */
  bool operator==(const TextureInfo& other) const = default;

 private:
  void SetupMemoryInfo(u32 base_address, u32 mip_address);
};

}  // namespace rex::xenos
