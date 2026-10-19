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

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/xenos/texture_layout.h>
#include <rex/math.h>

namespace rex::xenos {

void GetSubresourcesFromFetchConstant(const xenos::TextureFetch& fetch, u32* width_minus_1_out,
                                      u32* height_minus_1_out, u32* depth_or_array_size_minus_1_out,
                                      u32* base_page_out, u32* mip_page_out, u32* mip_min_level_out,
                                      u32* mip_max_level_out) {
  u32 width_minus_1 = 0;
  u32 height_minus_1 = 0;
  u32 depth_or_array_size_minus_1 = 0;
  switch (fetch.dimension) {
    case xenos::DataDimension::k1D:
      REX_ASSERT(!fetch.stacked);
      REX_ASSERT(!fetch.tiled);
      REX_ASSERT(!fetch.packed_mips);
      width_minus_1 = fetch.size_1d.width;
      break;
    case xenos::DataDimension::k2DOrStacked:
      width_minus_1 = fetch.size_2d.width;
      height_minus_1 = fetch.size_2d.height;
      depth_or_array_size_minus_1 = fetch.stacked ? fetch.size_2d.stack_depth : 0;
      break;
    case xenos::DataDimension::k3D:
      REX_ASSERT(!fetch.stacked);
      width_minus_1 = fetch.size_3d.width;
      height_minus_1 = fetch.size_3d.height;
      depth_or_array_size_minus_1 = fetch.size_3d.depth;
      break;
    case xenos::DataDimension::kCube:
      REX_ASSERT(!fetch.stacked);
      REX_ASSERT(fetch.size_2d.stack_depth == 5);
      width_minus_1 = fetch.size_2d.width;
      height_minus_1 = fetch.size_2d.height;
      depth_or_array_size_minus_1 = 5;
      break;
  }
  if (width_minus_1_out) {
    *width_minus_1_out = width_minus_1;
  }
  if (height_minus_1_out) {
    *height_minus_1_out = height_minus_1;
  }
  if (depth_or_array_size_minus_1_out) {
    *depth_or_array_size_minus_1_out = depth_or_array_size_minus_1;
  }

  u32 longest_axis_minus_1 = std::max(width_minus_1, height_minus_1);
  if (fetch.dimension == xenos::DataDimension::k3D) {
    longest_axis_minus_1 = std::max(longest_axis_minus_1, depth_or_array_size_minus_1);
  }
  u32 size_mip_max_level = rex::log2_floor(longest_axis_minus_1 + static_cast<u32>(1));

  u32 base_page = fetch.base_address & 0x1FFFF;
  u32 mip_page = fetch.mip_address & 0x1FFFF;

  u32 mip_min_level, mip_max_level;
  // Shader fetches may override mip_filter, so kBaseMap does not limit mip_max_level.
  if (mip_page == 0) {
    mip_min_level = 0;
    mip_max_level = 0;
  } else {
    mip_min_level = std::min(static_cast<u32>(fetch.mip_min_level), size_mip_max_level);
    mip_max_level = std::max(std::min(static_cast<u32>(fetch.mip_max_level), size_mip_max_level),
                             mip_min_level);
  }
  if (mip_max_level != 0) {
    if (base_page == 0) {
      mip_min_level = std::max(mip_min_level, static_cast<u32>(1));
    }
    if (mip_min_level != 0) {
      base_page = 0;
    }
  } else {
    mip_page = 0;
  }

  if (base_page_out) {
    *base_page_out = base_page;
  }
  if (mip_page_out) {
    *mip_page_out = mip_page;
  }
  if (mip_min_level_out) {
    *mip_min_level_out = mip_min_level;
  }
  if (mip_max_level_out) {
    *mip_max_level_out = mip_max_level;
  }
}

bool GetPackedMipOffset(u32 width, u32 height, u32 depth, xenos::TextureFormat format, u32 mip,
                        u32& x_blocks, u32& y_blocks, u32& z_blocks) {
  // Levels of 16 texels or less share 1 32x32 tile, placed by their logical size.

  u32 log2_width = rex::log2_ceil(width);
  u32 log2_height = rex::log2_ceil(height);
  u32 log2_size = std::min(log2_width, log2_height);
  if (log2_size > 4 + mip) {
    x_blocks = 0;
    y_blocks = 0;
    z_blocks = 0;
    return false;
  }
  u32 packed_mip_base = (log2_size > 4) ? (log2_size - 4) : 0;
  u32 packed_mip = mip - packed_mip_base;

  if (packed_mip < 3) {
    if (log2_width > log2_height) {
      x_blocks = 0;
      y_blocks = 16 >> packed_mip;
    } else {
      x_blocks = 16 >> packed_mip;
      y_blocks = 0;
    }
    z_blocks = 0;
  } else {
    u32 offset;
    if (log2_width > log2_height) {
      offset = (1 << (log2_width - packed_mip_base)) >> (packed_mip - 2);
      x_blocks = offset;
      y_blocks = 0;
    } else {
      x_blocks = 0;
      offset = (1 << (log2_height - packed_mip_base)) >> (packed_mip - 2);
      y_blocks = offset;
    }
    if (offset < 4) {
      u32 log2_depth = rex::log2_ceil(depth);
      if (log2_depth > 1 + packed_mip) {
        z_blocks = (log2_depth - packed_mip) * 4;
      } else {
        z_blocks = 4;
      }
    } else {
      z_blocks = 0;
    }
  }

  const FormatInfo* format_info = FormatInfo::Get(format);
  x_blocks /= format_info->block_width;
  y_blocks /= format_info->block_height;
  return true;
}

GuestTextureLayout GetGuestTextureLayout(xenos::DataDimension dimension,
                                         u32 base_pitch_texels_div_32, u32 width_texels,
                                         u32 height_texels, u32 depth_or_array_size, bool is_tiled,
                                         xenos::TextureFormat format, bool has_packed_levels,
                                         bool has_base, u32 max_level) {
  GuestTextureLayout layout;

  if (dimension == xenos::DataDimension::k1D) {
    REX_ASSERT(!is_tiled);
    // GetPackedMipOffset may pack along Y when width > height.
    REX_ASSERT(!has_packed_levels);
    height_texels = 1;
  }
  u32 depth = dimension == xenos::DataDimension::k3D ? depth_or_array_size : 1;
  switch (dimension) {
    case xenos::DataDimension::k2DOrStacked:
      layout.array_size = depth_or_array_size;
      break;
    case xenos::DataDimension::kCube:
      layout.array_size = 6;
      break;
    default:
      layout.array_size = 1;
  }

  // The extent math assumes at least 1 row, so an empty texture returns here.
  if (!width_texels || !height_texels || !depth || !layout.array_size) {
    layout = {};
    return layout;
  }

  u32 max_level_for_dimensions =
      rex::log2_floor(std::max(std::max(width_texels, height_texels), depth));
  REX_ASSERT(max_level <= max_level_for_dimensions);
  max_level = std::min(max_level, max_level_for_dimensions);
  layout.max_level = max_level;

  layout.packed_level =
      has_packed_levels ? GetPackedMipLevel(width_texels, height_texels) : UINT32_MAX;

  if (!has_base) {
    layout.base = {};
  }
  if (layout.packed_level != 0) {
    layout.mips[0] = {};
  }
  u32 max_stored_level = std::min(max_level, layout.packed_level);
  {
    u32 mips_end = max_stored_level + 1;
    REX_ASSERT(mips_end <= rex::CountOf(layout.mips));
    u32 mips_unused_count = static_cast<u32>(rex::CountOf(layout.mips)) - mips_end;
    if (mips_unused_count) {
      std::memset(&layout.mips[mips_end], 0, sizeof(layout.mips[0]) * mips_unused_count);
      std::memset(&layout.mip_offsets_bytes[mips_end], 0,
                  sizeof(layout.mip_offsets_bytes[0]) * mips_unused_count);
    }
  }
  layout.mips_total_extent_bytes = 0;

  const FormatInfo* format_info = FormatInfo::Get(format);
  u32 bytes_per_block = format_info->GetBytesPerBlock();

  // With the packed tail at level 0, iteration 0 is the base tail and iteration 1 the mip tail.
  u32 loop_level_last;
  if (layout.packed_level == 0) {
    loop_level_last = static_cast<u32>(max_level != 0);
  } else {
    loop_level_last = max_stored_level;
  }
  u32 mip_offset_bytes = 0;
  for (u32 loop_level = has_base ? 0 : 1; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    u32 level = (layout.packed_level == 0) ? 0 : loop_level;
    GuestTextureLayout::Level& level_layout = is_base ? layout.base : layout.mips[level];

    // Mip strides use power of 2 sizes aligned to 32x32x4 blocks, linear rows align to 256 bytes.
    u32 row_pitch_texels_unaligned;
    u32 z_layer_stride_texel_rows_unaligned;
    if (is_base) {
      row_pitch_texels_unaligned = base_pitch_texels_div_32 << 5;
      z_layer_stride_texel_rows_unaligned = height_texels;
    } else {
      row_pitch_texels_unaligned =
          std::max(rex::next_pow2(width_texels) >> level, static_cast<u32>(1));
      z_layer_stride_texel_rows_unaligned =
          std::max(rex::next_pow2(height_texels) >> level, static_cast<u32>(1));
    }
    u32 row_pitch_blocks_tile_aligned =
        rex::RoundUpPow2(rex::RoundUpPow2(row_pitch_texels_unaligned, format_info->block_width) /
                             format_info->block_width,
                         xenos::kTextureTileWidthHeight);
    level_layout.row_pitch_bytes = row_pitch_blocks_tile_aligned * bytes_per_block;
    if (!is_tiled && !is_base) {
      level_layout.row_pitch_bytes =
          rex::RoundUpPow2(level_layout.row_pitch_bytes, xenos::kTextureLinearRowAlignmentBytes);
    }
    level_layout.z_layer_stride_block_rows =
        dimension != xenos::DataDimension::k1D
            ? rex::RoundUpPow2(
                  rex::RoundUpPow2(z_layer_stride_texel_rows_unaligned, format_info->block_height) /
                      format_info->block_height,
                  xenos::kTextureTileWidthHeight)
            : 1;
    level_layout.array_layer_stride_bytes =
        level_layout.row_pitch_bytes * level_layout.z_layer_stride_block_rows;
    u32 z_stride_bytes = level_layout.array_layer_stride_bytes;
    if (dimension == xenos::DataDimension::k3D) {
      level_layout.array_layer_stride_bytes *=
          rex::RoundUpPow2(depth_or_array_size, xenos::kTextureTileDepth);
    }
    level_layout.array_layer_stride_bytes = rex::RoundUpPow2(
        level_layout.array_layer_stride_bytes, xenos::kTextureSubresourceAlignmentBytes);

    // The referenced memory may be smaller or larger than the stride.
    if (level == layout.packed_level) {
      // Only the rows the needed mips reach count, not the whole 32 row tail.
      level_layout.x_extent_blocks = 0;
      level_layout.y_extent_blocks = 0;
      level_layout.z_extent = 0;
      u32 packed_sublevel_last = is_base ? 0 : max_level;
      for (u32 packed_sublevel = layout.packed_level; packed_sublevel <= packed_sublevel_last;
           ++packed_sublevel) {
        u32 packed_sublevel_x_blocks;
        u32 packed_sublevel_y_blocks;
        u32 packed_sublevel_z;
        GetPackedMipOffset(width_texels, height_texels, depth, format, packed_sublevel,
                           packed_sublevel_x_blocks, packed_sublevel_y_blocks, packed_sublevel_z);
        level_layout.x_extent_blocks = std::max(
            level_layout.x_extent_blocks,
            packed_sublevel_x_blocks +
                rex::RoundUpPow2(std::max(width_texels >> packed_sublevel, static_cast<u32>(1)),
                                 format_info->block_width) /
                    format_info->block_width);
        level_layout.y_extent_blocks = std::max(
            level_layout.y_extent_blocks,
            packed_sublevel_y_blocks +
                rex::RoundUpPow2(std::max(height_texels >> packed_sublevel, static_cast<u32>(1)),
                                 format_info->block_height) /
                    format_info->block_height);
        level_layout.z_extent =
            std::max(level_layout.z_extent,
                     packed_sublevel_z + std::max(depth >> packed_sublevel, static_cast<u32>(1)));
      }
    } else {
      level_layout.x_extent_blocks =
          rex::RoundUpPow2(std::max(width_texels >> level, static_cast<u32>(1)),
                           format_info->block_width) /
          format_info->block_width;
      level_layout.y_extent_blocks =
          rex::RoundUpPow2(std::max(height_texels >> level, static_cast<u32>(1)),
                           format_info->block_height) /
          format_info->block_height;
      level_layout.z_extent = std::max(depth >> level, static_cast<u32>(1));
    }
    if (is_tiled) {
      u32 bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
      if (dimension == xenos::DataDimension::k3D) {
        level_layout.array_layer_data_extent_bytes = GetTiledAddressUpperBound3D(
            level_layout.x_extent_blocks, level_layout.y_extent_blocks, level_layout.z_extent,
            row_pitch_blocks_tile_aligned, level_layout.y_extent_blocks, bytes_per_block_log2);
      } else {
        level_layout.array_layer_data_extent_bytes =
            GetTiledAddressUpperBound2D(level_layout.x_extent_blocks, level_layout.y_extent_blocks,
                                        row_pitch_blocks_tile_aligned, bytes_per_block_log2);
      }
    } else {
      level_layout.array_layer_data_extent_bytes =
          z_stride_bytes * (level_layout.z_extent - 1) +
          level_layout.row_pitch_bytes * (level_layout.y_extent_blocks - 1) +
          bytes_per_block * level_layout.x_extent_blocks;
    }
    level_layout.level_data_extent_bytes =
        level_layout.array_layer_stride_bytes * (layout.array_size - 1) +
        level_layout.array_layer_data_extent_bytes;

    if (!is_base) {
      layout.mip_offsets_bytes[level] = mip_offset_bytes;
      layout.mips_total_extent_bytes = std::max(
          layout.mips_total_extent_bytes, mip_offset_bytes + level_layout.level_data_extent_bytes);
      mip_offset_bytes += level_layout.array_layer_stride_bytes * layout.array_size;
    }
  }

  return layout;
}

i32 GetTiledOffset2D(i32 x, i32 y, u32 pitch, u32 bytes_per_block_log2) {
  pitch = rex::RoundUpPow2(pitch, xenos::kTextureTileWidthHeight);
  i32 macro = ((x >> 5) + (y >> 5) * static_cast<i32>(pitch >> 5)) << (bytes_per_block_log2 + 7);
  i32 micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  i32 offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

i32 GetTiledOffset3D(i32 x, i32 y, i32 z, u32 pitch, u32 height, u32 bytes_per_block_log2) {
  pitch = rex::RoundUpPow2(pitch, xenos::kTextureTileWidthHeight);
  height = rex::RoundUpPow2(height, xenos::kTextureTileWidthHeight);
  i32 macro_outer =
      ((y >> 4) + (z >> 2) * static_cast<i32>(height >> 4)) * static_cast<i32>(pitch >> 5);
  i32 macro = ((((x >> 5) + macro_outer) << (bytes_per_block_log2 + 6)) & 0xFFFFFFF) << 1;
  i32 micro = (((x & 7) + ((y & 6) << 2)) << (bytes_per_block_log2 + 6)) >> 6;
  i32 offset_outer = ((y >> 3) + (z >> 2)) & 1;
  i32 offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  i32 offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                ((z & 3) << (bytes_per_block_log2 + 6)) + ((y & 1) << 4);
  i32 address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

u32 GetTiledAddressUpperBound2D(u32 right, u32 bottom, u32 pitch, u32 bytes_per_block_log2) {
  if (!right || !bottom) {
    return 0;
  }
  u32 upper_bound = static_cast<u32>(
      GetTiledOffset2D(static_cast<i32>((right - 1) & ~(xenos::kTextureTileWidthHeight - 1)),
                       static_cast<i32>((bottom - 1) & ~(xenos::kTextureTileWidthHeight - 1)),
                       pitch, bytes_per_block_log2));
  switch (bytes_per_block_log2) {
    case 0:
      // 128x128 portions address independently, the extent is 0xA00 bytes from the tile origin.
      upper_bound += 0xA00;
      break;
    case 1:
      // 64x64 portions address independently, the extent is 0xC00 bytes from the tile origin.
      upper_bound += 0xC00;
      break;
    default:
      upper_bound += UINT32_C(0x400) << bytes_per_block_log2;
      break;
  }
  return upper_bound;
}

u32 GetTiledAddressUpperBound3D(u32 right, u32 bottom, u32 back, u32 pitch, u32 height,
                                u32 bytes_per_block_log2) {
  if (!right || !bottom || !back) {
    return 0;
  }
  u32 upper_bound = static_cast<u32>(
      GetTiledOffset3D(static_cast<i32>((right - 1) & ~(xenos::kTextureTileWidthHeight - 1)),
                       static_cast<i32>((bottom - 1) & ~(xenos::kTextureTileWidthHeight - 1)),
                       static_cast<i32>((back - 1) & ~(xenos::kTextureTileDepth - 1)), pitch,
                       height, bytes_per_block_log2));
  u32 pitch_aligned = rex::RoundUpPow2(pitch, xenos::kTextureTileWidthHeight);
  switch (bytes_per_block_log2) {
    case 0:
      // 64x32x8 portions address independently.
      upper_bound +=
          ((pitch_aligned >> 6) << 12) + 0xC00 + ((pitch_aligned & (1 << 5)) << (10 - 5));
      break;
    default:
      // 32x32x8 portions address independently.
      upper_bound += ((pitch_aligned << (12 - 5 - 1)) + (0x1000 >> 1)) << bytes_per_block_log2;
      break;
  }
  return upper_bound;
}

u8 SwizzleSigns(const xenos::TextureFetch& fetch) {
  u8 signs = 0;
  bool any_not_signed = false, any_signed = false;
  u8 constant_mask = 0b00000000;
  for (u32 i = 0; i < 4; ++i) {
    u32 swizzle = (fetch.swizzle >> (i * 3)) & 0b111;
    if (swizzle & 0b100) {
      constant_mask |= static_cast<u8>(1) << (i * 2);
    } else {
      xenos::TextureSign sign =
          static_cast<xenos::TextureSign>((fetch.dword_0 >> (2 + swizzle * 2)) & 0b11);
      signs |= static_cast<u8>(sign) << (i * 2);
      if (sign == xenos::TextureSign::kSigned) {
        any_signed = true;
      } else {
        any_not_signed = true;
      }
    }
  }
  xenos::TextureSign constants_sign = xenos::TextureSign::kUnsigned;
  if (constant_mask == 0b01010101) {
    // A swizzle of constants only follows the original format, shaders still bind it for queries.
    if (((fetch.dword_0 >> 2) & 0b11111111) ==
        std::to_underlying(xenos::TextureSign::kSigned) * 0b01010101) {
      constants_sign = xenos::TextureSign::kSigned;
    }
  } else {
    if (any_signed && !any_not_signed) {
      constants_sign = xenos::TextureSign::kSigned;
    }
  }
  signs |= static_cast<u8>(constants_sign) * constant_mask;
  return signs;
}

void GetClampModesForDimension(const xenos::TextureFetch& fetch, xenos::ClampMode& clamp_x_out,
                               xenos::ClampMode& clamp_y_out, xenos::ClampMode& clamp_z_out) {
  clamp_x_out = xenos::ClampMode::kClampToEdge;
  clamp_y_out = xenos::ClampMode::kClampToEdge;
  clamp_z_out = xenos::ClampMode::kClampToEdge;
  switch (fetch.dimension) {
    case xenos::DataDimension::k3D:
      clamp_z_out = fetch.clamp_z;
      [[fallthrough]];
    case xenos::DataDimension::k2DOrStacked:
      clamp_y_out = fetch.clamp_y;
      [[fallthrough]];
    case xenos::DataDimension::k1D:
      clamp_x_out = fetch.clamp_x;
      break;
    default:
      break;
  }
}

}  // namespace rex::xenos
