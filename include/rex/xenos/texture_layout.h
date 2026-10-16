/**
 * @file        xenos/texture_layout.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/gpu/texture_util.h (Ben Vanik, 2022).
 */

#pragma once

#include <algorithm>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/math.h>
#include <rex/types.h>
#include <rex/xenos/texture_info.h>
#include <rex/xenos/xenos.h>

namespace rex::xenos {

/**
 * Reads the size from a fetch constant and cleans up the addresses and mip range based on
 * which of the base level and mips are present. Every output pointer may be null.
 *
 * @param fetch The texture fetch constant.
 * @param width_minus_1_out Receives the width in texels minus 1.
 * @param height_minus_1_out Receives the height in texels minus 1.
 * @param depth_or_array_size_minus_1_out Receives the depth or array size minus 1, 5 for cubes.
 * @param base_page_out Receives the base level page, 0 when the base level is absent.
 * @param mip_page_out Receives the mip page, 0 when no mips are present.
 * @param mip_min_level_out Receives the first present level.
 * @param mip_max_level_out Receives the last present level.
 */
REX_API void GetSubresourcesFromFetchConstant(const xenos::TextureFetch& fetch,
                                              u32* width_minus_1_out, u32* height_minus_1_out,
                                              u32* depth_or_array_size_minus_1_out,
                                              u32* base_page_out, u32* mip_page_out,
                                              u32* mip_min_level_out, u32* mip_max_level_out);

/**
 * @param width Width in texels.
 * @param height Height in texels.
 * @return The first mip level stored in the packed mip tail.
 */
inline u32 GetPackedMipLevel(u32 width, u32 height) {
  u32 log2_size = rex::log2_ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

/**
 * Finds the offset of a mip level within the packed mip tail. The offset stays within the image
 * size rounded up to 32.
 *
 * @param width Width in texels.
 * @param height Height in texels.
 * @param depth Depth in texels, 1 for textures other than 3D.
 * @param format Texel format.
 * @param mip The mip level.
 * @param x_blocks Receives the X offset in blocks.
 * @param y_blocks Receives the Y offset in blocks.
 * @param z_blocks Receives the Z offset in blocks.
 * @return False when the level is not packed, with all offsets set to 0.
 */
REX_API bool GetPackedMipOffset(u32 width, u32 height, u32 depth, xenos::TextureFormat format,
                                u32 mip, u32& x_blocks, u32& y_blocks, u32& z_blocks);

/**
 * Memory layout of a texture in guest memory.
 *
 * The base level is stored at base_address with the fetch constant pitch. Mips 1 and up are
 * stored at mip_address with sizes of max(next_pow2(size) >> level, 1). Strides between array
 * layers and Z layers use 32x1x1 tiles for 1D, 32x32x1 for 2D and 32x32x4 for 3D, in texels or
 * compressed blocks. Linear rows are aligned to 256 bytes. Array layers are stored within each
 * level, and each array layer or level is aligned to 4 KB.
 *
 * With packed mips, levels whose width or height is 16 or less share 1 tail laid out like the
 * level before it. The base level can be packed as well, in which case base_address and
 * mip_address hold separate tails. Padding counts for strides but is not always backed by
 * memory, so the data extents exclude it where the layout allows.
 */
struct TextureGuestLayout {
  /** Layout of 1 stored level. */
  struct Level {
    u32 row_pitch_bytes;               /**< Block row stride with mip and linear alignment. */
    u32 z_layer_stride_block_rows;     /**< Z layer stride in block rows. */
    u32 array_layer_stride_bytes;      /**< Array layer stride, 4 KB aligned. */
    u32 x_extent_blocks;               /**< Exclusive X bound of the blocks in use. */
    u32 y_extent_blocks;               /**< Exclusive Y bound of the blocks in use. */
    u32 z_extent;                      /**< Exclusive Z bound of the layers in use. */
    u32 array_layer_data_extent_bytes; /**< Bytes an array layer reads, not 4 KB aligned. */
    u32 level_data_extent_bytes;       /**< Bytes the level reads across all array layers. */
  };

  Level base; /**< The base level. */
  /** Mip levels, stored from min(1, packed_level) to min(max_level, packed_level). */
  Level mips[xenos::kTextureMaxMips];
  u32 mip_offsets_bytes[xenos::kTextureMaxMips]; /**< Offset of each mip from mip_address. */
  u32 mips_total_extent_bytes;                   /**< Bytes all mips read. */
  u32 max_level;                                 /**< Last level, clamped to the size. */
  u32 packed_level;                              /**< First packed level, or UINT32_MAX. */
  u32 array_size;                                /**< Array layer count, 6 for cubes. */
};

/**
 * Calculates the guest memory layout of a texture. A texture with no texels gets an all zero
 * layout.
 *
 * @param dimension Texture dimension.
 * @param base_pitch_texels_div_32 Base level row pitch in texels divided by 32.
 * @param width_texels Width in texels.
 * @param height_texels Height in texels.
 * @param depth_or_array_size Depth for 3D textures, array size otherwise.
 * @param is_tiled True for tiled data, false for linear.
 * @param format Texel format.
 * @param has_packed_levels True when small levels share a packed mip tail.
 * @param has_base True when the base level is present.
 * @param max_level Last mip level.
 * @return The layout.
 */
REX_API TextureGuestLayout GetGuestTextureLayout(xenos::DataDimension dimension,
                                                 u32 base_pitch_texels_div_32, u32 width_texels,
                                                 u32 height_texels, u32 depth_or_array_size,
                                                 bool is_tiled, xenos::TextureFormat format,
                                                 bool has_packed_levels, bool has_base,
                                                 u32 max_level);

/**
 * Offset of a block in a 2D tiled texture. Negative coordinates are valid, so the result can
 * move from a region back to the texture origin when the coordinates are a multiple of the
 * addressing period. Tile origins grow with Y / 32 and then X / 32. The sequence repeats every
 * 128 blocks at 1 byte per block and every 64 blocks at 2 bytes per block. From 4 bytes per block
 * up, each 32x32 tile is stored separately in tile row order.
 *
 * @param x Block X coordinate.
 * @param y Block Y coordinate.
 * @param pitch Row pitch in blocks, aligned to 32 internally.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 * @return The byte offset.
 */
REX_API i32 GetTiledOffset2D(i32 x, i32 y, u32 pitch, u32 bytes_per_block_log2);

/**
 * Offset of a block in a 3D tiled texture. Tile origins grow with Z / 4, then Y / 32, then
 * X / 32. Addressing repeats every 8 layers along Z and every 32 blocks along Y. Each 32 block
 * row by 4 layer portion occupies its own 4 KB aligned range.
 *
 * @param x Block X coordinate.
 * @param y Block Y coordinate.
 * @param z Depth coordinate.
 * @param pitch Row pitch in blocks, aligned to 32 internally.
 * @param height Height in blocks, aligned to 32 internally.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 * @return The byte offset.
 */
REX_API i32 GetTiledOffset3D(i32 x, i32 y, i32 z, u32 pitch, u32 height, u32 bytes_per_block_log2);

/**
 * Lowest address a 2D region can touch, the origin of its first 32x32 tile.
 *
 * @param left Left edge in blocks.
 * @param top Top edge in blocks.
 * @param pitch Row pitch in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 * @return The lower bound byte offset.
 */
inline u32 GetTiledAddressLowerBound2D(u32 left, u32 top, u32 pitch, u32 bytes_per_block_log2) {
  return static_cast<u32>(GetTiledOffset2D(
      static_cast<i32>(left & ~(xenos::kTextureTileWidthHeight - 1)),
      static_cast<i32>(top & ~(xenos::kTextureTileWidthHeight - 1)), pitch, bytes_per_block_log2));
}

/**
 * Lowest address a 3D region can touch, the origin of its first 32x32x4 tile.
 *
 * @param left Left edge in blocks.
 * @param top Top edge in blocks.
 * @param front Front layer.
 * @param pitch Row pitch in blocks.
 * @param height Height in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 * @return The lower bound byte offset.
 */
inline u32 GetTiledAddressLowerBound3D(u32 left, u32 top, u32 front, u32 pitch, u32 height,
                                       u32 bytes_per_block_log2) {
  return static_cast<u32>(GetTiledOffset3D(
      static_cast<i32>(left & ~(xenos::kTextureTileWidthHeight - 1)),
      static_cast<i32>(top & ~(xenos::kTextureTileWidthHeight - 1)),
      static_cast<i32>(front & ~xenos::kTextureTileDepth), pitch, height, bytes_per_block_log2));
}

/**
 * Exclusive upper address bound of a 2D region. The region may extend past the pitch.
 *
 * @param right Exclusive right edge in blocks.
 * @param bottom Exclusive bottom edge in blocks.
 * @param pitch Row pitch in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 * @return The upper bound byte offset, or 0 for an empty region.
 */
REX_API u32 GetTiledAddressUpperBound2D(u32 right, u32 bottom, u32 pitch, u32 bytes_per_block_log2);

/**
 * Exclusive upper address bound of a 3D region. The region may extend past the pitch and height.
 *
 * @param right Exclusive right edge in blocks.
 * @param bottom Exclusive bottom edge in blocks.
 * @param back Exclusive back layer.
 * @param pitch Row pitch in blocks.
 * @param height Height in blocks.
 * @param bytes_per_block_log2 log2_floor of the bytes per block.
 * @return The upper bound byte offset, or 0 for an empty region.
 */
REX_API u32 GetTiledAddressUpperBound3D(u32 right, u32 bottom, u32 back, u32 pitch, u32 height,
                                        u32 bytes_per_block_log2);

/**
 * Packs the TextureSign of each component after the fetch constant swizzle. Constant 0 and 1
 * components count as unsigned unless every other component is signed.
 *
 * @param fetch The texture fetch constant.
 * @return 4 TextureSign values, 2 bits each.
 */
REX_API u8 SwizzleSigns(const xenos::TextureFetch& fetch);

/**
 * @param packed_signs Packed signs from SwizzleSigns.
 * @return True when any component is not kSigned.
 */
constexpr bool IsAnySignNotSigned(u8 packed_signs) {
  return packed_signs != std::to_underlying(xenos::TextureSign::kSigned) * 0b01010101;
}

/**
 * @param packed_signs Packed signs from SwizzleSigns.
 * @return True when any component is kSigned.
 */
constexpr bool IsAnySignSigned(u8 packed_signs) {
  u32 xor_signed = packed_signs ^ (std::to_underlying(xenos::TextureSign::kSigned) * 0b01010101);
  return ((xor_signed | (xor_signed >> 1)) & 0b01010101) != 0b01010101;
}

/**
 * Reads the clamp modes a fetch constant uses for its dimension. Unused axes and cube textures
 * get kClampToEdge.
 *
 * @param fetch The texture fetch constant.
 * @param clamp_x_out Receives the X clamp mode.
 * @param clamp_y_out Receives the Y clamp mode.
 * @param clamp_z_out Receives the Z clamp mode.
 */
REX_API void GetClampModesForDimension(const xenos::TextureFetch& fetch,
                                       xenos::ClampMode& clamp_x_out, xenos::ClampMode& clamp_y_out,
                                       xenos::ClampMode& clamp_z_out);

}  // namespace rex::xenos
