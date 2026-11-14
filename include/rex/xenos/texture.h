/**
 * @file        xenos/texture.h
 * @brief       Xenos texture info, memory layout and format conversion
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/gpu/texture_info.h (Ben Vanik, 2014).
 *
 * @remarks     Derived from Xenia's xenia/gpu/texture_util.h (Ben Vanik, 2022).
 * @remarks     Derived from Xenia's xenia/gpu/texture_conversion.h (Ben Vanik, 2014).
 */

#pragma once

#include <memory>
#include <algorithm>
#include <cstring>
#include <functional>

#include <rex/platform.h>
#include <rex/assert.h>
#include <rex/types.h>
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/bit.h>

namespace rex::xenos {

/**
 * Reads the size and present levels from a fetch constant. Every output pointer may be null.
 *
 * @param width_minus_1_out Receives the width in texels minus 1.
 * @param height_minus_1_out Receives the height in texels minus 1.
 * @param depth_or_array_size_minus_1_out Receives the depth or array size minus 1, 5 for cubes.
 * @param base_page_out Receives the base level page, 0 when the base level is absent.
 * @param mip_page_out Receives the mip page, 0 when no mips are present.
 * @param mip_min_level_out Receives the first present level.
 * @param mip_max_level_out Receives the last present level.
 */
void GetSubresourcesFromFetchConstant(const xenos::TextureFetch& fetch, u32* width_minus_1_out,
                                      u32* height_minus_1_out, u32* depth_or_array_size_minus_1_out,
                                      u32* base_page_out, u32* mip_page_out, u32* mip_min_level_out,
                                      u32* mip_max_level_out);

/** Returns the first mip level stored in the packed mip tail. */
inline u32 GetPackedMipLevel(u32 width, u32 height) {
  u32 log2_size = rex::log2_ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

/**
 * Finds the offset of a mip level within the packed mip tail.
 *
 * @param depth Depth in texels, 1 for textures other than 3D.
 * @param x_blocks Receives the X offset in blocks.
 * @param y_blocks Receives the Y offset in blocks.
 * @param z_blocks Receives the Z offset in blocks.
 * @return False when the level is not packed, with all offsets set to 0.
 */
bool GetPackedMipOffset(u32 width, u32 height, u32 depth, xenos::TextureFormat format, u32 mip,
                        u32& x_blocks, u32& y_blocks, u32& z_blocks);

/**
 * Memory layout of a texture in guest memory.
 *
 * Array layers and levels are aligned to 4 KB and linear rows to 256 bytes. With packed mips,
 * levels whose width or height is 16 or less share one tail.
 */
struct GuestTextureLayout {
  struct Level {
    u32 row_pitch_bytes; /**< Block row stride with mip and linear alignment. */
    u32 z_layer_stride_block_rows;
    u32 array_layer_stride_bytes;      /**< Array layer stride, 4 KB aligned. */
    u32 x_extent_blocks;               /**< Exclusive X bound of the blocks in use. */
    u32 y_extent_blocks;               /**< Exclusive Y bound of the blocks in use. */
    u32 z_extent;                      /**< Exclusive Z bound of the layers in use. */
    u32 array_layer_data_extent_bytes; /**< Bytes an array layer reads, not 4 KB aligned. */
    u32 level_data_extent_bytes;       /**< Bytes the level reads across all array layers. */
  };

  Level base;
  /** Mip levels, stored from min(1, packed_level) to min(max_level, packed_level). */
  Level mips[xenos::kTextureMaxMips];
  u32 mip_offsets_bytes[xenos::kTextureMaxMips]; /**< Offset of each mip from mip_address. */
  u32 mips_total_extent_bytes;
  u32 max_level;    /**< Last level, clamped to the size. */
  u32 packed_level; /**< First packed level, or UINT32_MAX. */
  u32 array_size;   /**< Array layer count, 6 for cubes. */
};

/**
 * Calculates the guest memory layout of a texture, all zero when it has no texels.
 *
 * @param base_pitch_texels_div_32 Base level row pitch in texels divided by 32.
 * @param depth_or_array_size Depth for 3D textures, array size otherwise.
 * @param is_tiled True for tiled data, false for linear.
 * @param has_packed_levels True when small levels share a packed mip tail.
 * @param has_base True when the base level is present.
 */
GuestTextureLayout GetGuestTextureLayout(xenos::DataDimension dimension,
                                         u32 base_pitch_texels_div_32, u32 width_texels,
                                         u32 height_texels, u32 depth_or_array_size, bool is_tiled,
                                         xenos::TextureFormat format, bool has_packed_levels,
                                         bool has_base, u32 max_level);

/**
 * Returns the offset of a block in a 2D tiled texture. Negative coordinates are valid.
 *
 * @param pitch Row pitch in blocks, aligned to 32 internally.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 */
i32 GetTiledOffset2D(i32 x, i32 y, u32 pitch, u32 bytes_per_block_log2);

/**
 * Returns the offset of a block in a 3D tiled texture. Each 32 block row by 4 layer portion
 * occupies its own 4 KB aligned range.
 *
 * @param pitch Row pitch in blocks, aligned to 32 internally.
 * @param height Height in blocks, aligned to 32 internally.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 */
i32 GetTiledOffset3D(i32 x, i32 y, i32 z, u32 pitch, u32 height, u32 bytes_per_block_log2);

/**
 * Returns the lowest address a 2D region can touch, the origin of its first 32x32 tile. The offset
 * is in bytes.
 *
 * @param left Left edge in blocks.
 * @param top Top edge in blocks.
 * @param pitch Row pitch in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 */
inline u32 GetTiledAddressLowerBound2D(u32 left, u32 top, u32 pitch, u32 bytes_per_block_log2) {
  return static_cast<u32>(GetTiledOffset2D(
      static_cast<i32>(left & ~(xenos::kTextureTileWidthHeight - 1)),
      static_cast<i32>(top & ~(xenos::kTextureTileWidthHeight - 1)), pitch, bytes_per_block_log2));
}

/**
 * Returns the lowest address a 3D region can touch, the origin of its first 32x32x4 tile. The
 * offset is in bytes.
 *
 * @param left Left edge in blocks.
 * @param top Top edge in blocks.
 * @param pitch Row pitch in blocks.
 * @param height Height in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 */
inline u32 GetTiledAddressLowerBound3D(u32 left, u32 top, u32 front, u32 pitch, u32 height,
                                       u32 bytes_per_block_log2) {
  return static_cast<u32>(
      GetTiledOffset3D(static_cast<i32>(left & ~(xenos::kTextureTileWidthHeight - 1)),
                       static_cast<i32>(top & ~(xenos::kTextureTileWidthHeight - 1)),
                       static_cast<i32>(front & ~(xenos::kTextureTileDepth - 1)), pitch, height,
                       bytes_per_block_log2));
}

/**
 * Returns the exclusive upper address bound of a 2D region, or 0 for an empty region. The region
 * may extend past the pitch.
 *
 * @param right Exclusive right edge in blocks.
 * @param bottom Exclusive bottom edge in blocks.
 * @param pitch Row pitch in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 */
u32 GetTiledAddressUpperBound2D(u32 right, u32 bottom, u32 pitch, u32 bytes_per_block_log2);

/**
 * Returns the exclusive upper address bound of a 3D region, or 0 for an empty region. The region
 * may extend past the pitch and height.
 *
 * @param right Exclusive right edge in blocks.
 * @param bottom Exclusive bottom edge in blocks.
 * @param back Exclusive back layer.
 * @param pitch Row pitch in blocks.
 * @param height Height in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 */
u32 GetTiledAddressUpperBound3D(u32 right, u32 bottom, u32 back, u32 pitch, u32 height,
                                u32 bytes_per_block_log2);

/**
 * Packs the TextureSign of each component after the swizzle. Constant 0 and 1 count as unsigned
 * unless every other component is signed.
 *
 * @return Four TextureSign values, 2 bits each.
 */
u8 SwizzleSigns(const xenos::TextureFetch& fetch);

/**
 * Reports whether any component is not kSigned.
 *
 * @param packed_signs Packed signs from SwizzleSigns.
 */
constexpr bool HasNonSignedComponent(u8 packed_signs) {
  return packed_signs != std::to_underlying(xenos::TextureSign::kSigned) * 0b01010101;
}

/**
 * Reports whether any component is kSigned.
 *
 * @param packed_signs Packed signs from SwizzleSigns.
 */
constexpr bool HasSignedComponent(u8 packed_signs) {
  u32 xor_signed = packed_signs ^ (std::to_underlying(xenos::TextureSign::kSigned) * 0b01010101);
  return ((xor_signed | (xor_signed >> 1)) & 0b01010101) != 0b01010101;
}

/**
 * Reads the clamp modes a fetch constant uses for its dimension. Unused axes and cube textures
 * get kClampToEdge.
 *
 * @param clamp_x_out Receives the X clamp mode.
 * @param clamp_y_out Receives the Y clamp mode.
 * @param clamp_z_out Receives the Z clamp mode.
 */
void GetClampModesForDimension(const xenos::TextureFetch& fetch, xenos::ClampMode& clamp_x_out,
                               xenos::ClampMode& clamp_y_out, xenos::ClampMode& clamp_z_out);

}  // namespace rex::xenos

namespace rex::xenos {

/**
 * Maps a resampling or gamma control variant to its base format.
 *
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
 * Returns the size of one texel of an uncompressed format in bytes, or 0 for an unhandled format.
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

/** Returns the texture format with the same numeric value as a color format. */
inline xenos::TextureFormat ColorFormatToTextureFormat(xenos::ColorFormat color_format) {
  return static_cast<xenos::TextureFormat>(color_format);
}

/**
 * Returns the texture format that matches a depth render target format, or TextureFormat(~0) for an
 * unhandled format.
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
      return static_cast<xenos::TextureFormat>(~0);
  }
}

enum class FormatType : u8 {
  kResolvable,   /**< Uncompressed and usable as a resolve target. */
  kUncompressed, /**< Uncompressed, not a resolve or memory export target. */
  kCompressed,
};

struct FormatInfo {
  xenos::TextureFormat format;
  const char* name; /**< Enumerator name of the format. */
  FormatType type;
  u32 block_width;
  u32 block_height;
  u32 bits_per_pixel;

  u32 GetBytesPerBlock() const { return block_width * block_height * bits_per_pixel / 8; }

  /**
   * Returns the table entry for a TextureFormat value.
   *
   * @param gpu_format A TextureFormat value, below 64.
   */
  static const FormatInfo* Get(u32 gpu_format);

  static const FormatInfo* Get(xenos::TextureFormat format) {
    return Get(static_cast<u32>(format));
  }
};

struct TextureInfo;

struct TextureExtent {
  u32 pitch;
  u32 height;        /**< Height in texels, padded like the pitch. */
  u32 block_width;   /**< Visible blocks per row. */
  u32 block_height;  /**< Visible block rows. */
  u32 block_pitch_h; /**< Blocks per row including padding. */
  u32 block_pitch_v; /**< Block rows including padding. */
  u32 depth;

  /** Returns the block count including padding in every dimension. */
  u32 GetAllBlocks() const { return block_pitch_h * block_pitch_v * depth; }

  /** Returns the block count including row padding but only the visible block rows. */
  u32 GetVisibleBlocks() const { return block_pitch_h * block_height * depth; }

  /**
   * Returns the extent of a texture level from its block layout and size.
   *
   * @param is_tiled True for a tiled texture, false for a linear one.
   * @param is_guest True to pad as guest memory does, false for a packed host layout.
   */
  static TextureExtent Calculate(const FormatInfo* format_info, u32 pitch, u32 height, u32 depth,
                                 bool is_tiled, bool is_guest);

  /**
   * Returns the extent of the base level of a texture.
   *
   * @param texture_info The texture, using its base level size.
   * @param is_guest True for the padded guest memory layout, false for a packed host layout.
   */
  static TextureExtent Calculate(const TextureInfo* texture_info, bool is_guest);

  bool operator==(const TextureExtent& other) const = default;
};

struct TextureMemoryRanges {
  u32 base_address; /**< Address of the base level, or 0 when absent. */
  u32 base_size;
  u32 mip_address; /**< Address of the mip levels, or 0 when absent. */
  u32 mip_size;

  bool operator==(const TextureMemoryRanges& other) const = default;
};

/** Texture description decoded from a fetch constant or a resolve. */
struct TextureInfo {
  xenos::TextureFormat format;
  xenos::Endian endianness;

  xenos::DataDimension dimension; /**< Dimension, with 1D stored as 2D. */
  u32 width_minus_1;
  u32 height_minus_1;
  u32 depth_or_array_size_minus_1;
  u32 pitch;
  u32 mip_min_level;
  u32 mip_max_level;
  bool is_stacked; /**< True for a 2D array. */
  bool is_tiled;
  bool has_packed_mips; /**< True when small mips share a packed tile. */

  TextureMemoryRanges memory;
  TextureExtent extent; /**< Base level extent in the guest layout. */

  const FormatInfo* GetFormatInfo() const { return FormatInfo::Get(static_cast<u32>(format)); }

  bool IsCompressed() const { return GetFormatInfo()->type == FormatType::kCompressed; }

  u32 GetMipLevels() const { return 1 + (mip_max_level - mip_min_level); }

  /**
   * Decodes a texture description from a texture fetch constant.
   *
   * @param out_info Receives the texture description.
   */
  static bool FromFetchConstant(const xenos::TextureFetch& fetch, TextureInfo* out_info);

  /**
   * Fills the texture info of a tiled 2D resolve destination with a single level.
   *
   * @param endian Swap mode of the texel data.
   * @param width Width in texels, at least 1.
   * @param height Height in texels, at least 1.
   * @param out_info Receives the texture description.
   */
  static bool FromResolve(u32 physical_address, xenos::TextureFormat texture_format,
                          xenos::Endian endian, u32 pitch, u32 width, u32 height, u32 depth,
                          TextureInfo* out_info);

  /** Returns the mip level count of a full chain for the texture size. */
  u32 GetMaxMipLevels() const;

  /**
   * Returns the extent of a mip level.
   *
   * @param is_guest True for the padded guest layout with power of 2 mip sizes, false for packed.
   */
  const TextureExtent GetMipExtent(u32 mip, bool is_guest) const;

  /**
   * Computes the size of a mip level in texels.
   *
   * @param width Receives the width in texels, rounded to a power of 2 for levels above 0.
   * @param height Receives the height in texels, rounded to a power of 2 for levels above 0.
   */
  void GetMipSize(u32 mip, u32* width, u32* height) const;

  /**
   * Returns the guest address of a mip level and its offset within a packed tile, or 0 when the
   * texture has no mip data.
   *
   * @param offset_x Receives the X offset in blocks within a packed tile.
   * @param offset_y Receives the Y offset in blocks within a packed tile.
   * @param is_guest True for the padded guest memory layout, false for a packed host layout.
   */
  u32 GetMipLocation(u32 mip, u32* offset_x, u32* offset_y, bool is_guest) const;

  /**
   * Computes the block offset of a level within a packed tile.
   *
   * @param width Width of the packed level set in texels.
   * @param height Height of the packed level set in texels.
   * @param packed_tile Index of the level within the packed tile.
   * @param offset_x Receives the X offset in blocks.
   * @param offset_y Receives the Y offset in blocks.
   * @return False when the level is too large to be packed, with both offsets set to 0.
   */
  static bool GetPackedMipOffset(u32 width, u32 height, const FormatInfo* format_info,
                                 u32 packed_tile, u32* offset_x, u32* offset_y);

  /**
   * Computes the block offset of a level of this texture within its packed tile.
   *
   * @param packed_tile Index of the level within the packed tile.
   * @param offset_x Receives the X offset in blocks.
   * @param offset_y Receives the Y offset in blocks.
   * @return False when the texture has no packed mips or the level is not packed.
   */
  bool GetPackedMipOffset(u32 packed_tile, u32* offset_x, u32* offset_y) const;

  /** Returns the 64 bit XXH3 hash of every field. */
  u64 Hash() const;

  bool operator==(const TextureInfo& other) const = default;

 private:
  void SetupMemoryRanges(u32 base_address, u32 mip_address);
};

}  // namespace rex::xenos

namespace rex::xenos {

/** Copies a block, swapping bytes by the given endian mode. */
using CopyBlockCallback = std::function<void(xenos::Endian, void*, const void*, size_t)>;

/** Copies bytes, swapping them as endian specifies. */
void CopySwapBlock(xenos::Endian endian, void* output, const void* input, size_t length);

/**
 * Decodes an 8 byte CTX1 block into 4x4 R8G8 texels.
 *
 * @param endian The swap mode of the source block.
 * @param output Destination for 4 rows of 4 R8G8 texels.
 * @param input Source CTX1 block.
 * @param output_pitch Destination row pitch in bytes.
 */
void ConvertTexelCTX1ToR8G8(xenos::Endian endian, void* output, const void* input,
                            size_t output_pitch);

/**
 * Expands a DXT3A block into a DXT3 block with the color half zeroed.
 *
 * @param endian The swap mode of the source block.
 * @param output Destination 16 byte DXT3 block.
 * @param input Source 8 byte DXT3A block.
 * @param length Unused.
 */
void ConvertTexelDXT3AToDXT3(xenos::Endian endian, void* output, const void* input, size_t length);

using UntileCopyBlockCallback = std::function<void(void*, const void*, size_t)>;

struct UntileRegion {
  u32 offset_x;     /**< Left edge of the region in blocks. */
  u32 offset_y;     /**< Top edge of the region in blocks. */
  u32 width;        /**< Region width in blocks. */
  u32 height;       /**< Region height in blocks. */
  u32 input_pitch;  /**< Tiled source pitch in blocks. */
  u32 output_pitch; /**< Linear destination pitch in blocks. */
  const FormatInfo* input_format_info;
  const FormatInfo* output_format_info;
  UntileCopyBlockCallback copy_callback;
};

/**
 * Converts a region of a 2D tiled texture to linear layout.
 *
 * @param output_buffer Linear destination.
 * @param input_buffer Tiled source.
 * @param untile_info The region, pitches, formats and block copy callback.
 */
void Untile(u8* output_buffer, const u8* input_buffer, const UntileRegion* untile_info);

}  // namespace rex::xenos
