/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>
#include <cmath>
#include <cstring>

#include <rex/xenos/texture.h>
#include <rex/hash.h>
#include <rex/logging/log.h>
#include <rex/bit.h>
#include <rex/memory.h>

namespace rex::xenos {

bool TextureInfo::FromFetchConstant(const TextureFetch& fetch, TextureInfo* out_info) {
  std::memset(out_info, 0, sizeof(TextureInfo));

  auto& info = *out_info;

  info.format = fetch.format;
  info.endianness = fetch.endianness;

  info.dimension = fetch.dimension;
  info.width_minus_1 = info.height_minus_1 = info.depth_or_array_size_minus_1 = 0;
  info.is_stacked = false;
  switch (info.dimension) {
    case xenos::DataDimension::k1D:
      info.dimension = DataDimension::k2DOrStacked;
      info.width_minus_1 = fetch.size_1d.width;
      REX_ASSERT(!fetch.stacked);
      break;
    case xenos::DataDimension::k2DOrStacked:
      info.width_minus_1 = fetch.size_2d.width;
      info.height_minus_1 = fetch.size_2d.height;
      if (fetch.stacked) {
        info.depth_or_array_size_minus_1 = fetch.size_2d.stack_depth;
        info.is_stacked = true;
      }
      break;
    case xenos::DataDimension::k3D:
      info.width_minus_1 = fetch.size_3d.width;
      info.height_minus_1 = fetch.size_3d.height;
      info.depth_or_array_size_minus_1 = fetch.size_3d.depth;
      REX_ASSERT(!fetch.stacked);
      break;
    case xenos::DataDimension::kCube:
      info.width_minus_1 = fetch.size_2d.width;
      info.height_minus_1 = fetch.size_2d.height;
      REX_ASSERT(fetch.size_2d.stack_depth == 5);
      info.depth_or_array_size_minus_1 = fetch.size_2d.stack_depth;
      REX_ASSERT(!fetch.stacked);
      break;
    default:
      REX_UNHANDLED_CASE(info.dimension);
      break;
  }
  info.pitch = fetch.pitch << 5;

  info.mip_min_level = fetch.mip_min_level;
  info.mip_max_level =
      std::max(static_cast<u32>(fetch.mip_min_level), static_cast<u32>(fetch.mip_max_level));

  info.is_tiled = fetch.tiled;
  info.has_packed_mips = fetch.packed_mips;

  info.extent = TextureExtent::Calculate(out_info, true);
  info.SetupMemoryRanges(fetch.base_address << 12, fetch.mip_address << 12);

  if (info.mip_max_level > 0 && !info.memory.mip_address) {
    info.mip_max_level = 0;
  }

  REX_ASSERT(info.mip_min_level <= info.mip_max_level);

  return true;
}

bool TextureInfo::FromResolve(u32 physical_address, xenos::TextureFormat format,
                              xenos::Endian endian, u32 pitch, u32 width, u32 height, u32 depth,
                              TextureInfo* out_info) {
  REX_ASSERT(width > 0);
  REX_ASSERT(height > 0);

  std::memset(out_info, 0, sizeof(TextureInfo));

  auto& info = *out_info;
  info.format = format;
  info.endianness = endian;

  info.dimension = xenos::DataDimension::k2DOrStacked;
  info.width_minus_1 = width - 1;
  info.height_minus_1 = height - 1;
  info.depth_or_array_size_minus_1 = depth - 1;

  info.pitch = pitch;
  info.mip_min_level = 0;
  info.mip_max_level = 0;

  info.is_tiled = true;
  info.has_packed_mips = false;

  info.extent = TextureExtent::Calculate(out_info, true);
  info.SetupMemoryRanges(physical_address, 0);
  return true;
}

u32 TextureInfo::GetMaxMipLevels() const {
  return 1 + rex::log2_floor(std::max(
                 {width_minus_1 + 1, height_minus_1 + 1, depth_or_array_size_minus_1 + 1}));
}

const TextureExtent TextureInfo::GetMipExtent(u32 mip, bool is_guest) const {
  if (mip == 0) {
    return extent;
  }
  u32 mip_width, mip_height;
  if (is_guest) {
    mip_width = rex::next_pow2(width_minus_1 + 1) >> mip;
    mip_height = rex::next_pow2(height_minus_1 + 1) >> mip;
  } else {
    mip_width = std::max(1u, (width_minus_1 + 1) >> mip);
    mip_height = std::max(1u, (height_minus_1 + 1) >> mip);
  }
  return TextureExtent::Calculate(GetFormatInfo(), mip_width, mip_height,
                                  depth_or_array_size_minus_1 + 1, is_tiled, is_guest);
}

void TextureInfo::GetMipSize(u32 mip, u32* out_width, u32* out_height) const {
  REX_ASSERT(out_width != nullptr);
  REX_ASSERT(out_height != nullptr);
  if (mip == 0) {
    *out_width = width_minus_1 + 1;
    *out_height = height_minus_1 + 1;
    return;
  }
  u32 width_pow2 = rex::next_pow2(width_minus_1 + 1);
  u32 height_pow2 = rex::next_pow2(height_minus_1 + 1);
  *out_width = std::max(width_pow2 >> mip, 1u);
  *out_height = std::max(height_pow2 >> mip, 1u);
}

u32 TextureInfo::GetMipLocation(u32 mip, u32* offset_x, u32* offset_y, bool is_guest) const {
  if (mip == 0) {
    if (!has_packed_mips) {
      *offset_x = 0;
      *offset_y = 0;
    } else {
      GetPackedMipOffset(0, offset_x, offset_y);
    }
    return memory.base_address;
  }

  if (!memory.mip_address) {
    *offset_x = 0;
    *offset_y = 0;
    return 0;
  }

  u32 address_base, address_offset;
  address_base = memory.mip_address;
  address_offset = 0;

  auto bytes_per_block = GetFormatInfo()->GetBytesPerBlock();

  if (!has_packed_mips) {
    for (u32 i = 1; i < mip; i++) {
      address_offset += GetMipExtent(i, is_guest).GetAllBlocks() * bytes_per_block;
    }
    *offset_x = 0;
    *offset_y = 0;
    return address_base + address_offset;
  }

  u32 width_pow2 = rex::next_pow2(width_minus_1 + 1);
  u32 height_pow2 = rex::next_pow2(height_minus_1 + 1);

  u32 packed_mip_base = 1;
  for (u32 i = packed_mip_base; i < mip; i++, packed_mip_base++) {
    u32 mip_width = std::max(width_pow2 >> i, 1u);
    u32 mip_height = std::max(height_pow2 >> i, 1u);
    if (std::min(mip_width, mip_height) <= 16) {
      break;
    }
    address_offset += GetMipExtent(i, is_guest).GetAllBlocks() * bytes_per_block;
  }

  GetPackedMipOffset(width_pow2 >> mip, height_pow2 >> mip, GetFormatInfo(), mip - packed_mip_base,
                     offset_x, offset_y);
  return address_base + address_offset;
}

bool TextureInfo::GetPackedMipOffset(u32 width, u32 height, const FormatInfo* format_info,
                                     u32 packed_tile, u32* offset_x, u32* offset_y) {
  // Levels of 16 texels or less share 1 32x32 tile, placed by their logical size.

  u32 log2_width = rex::log2_ceil(width);
  u32 log2_height = rex::log2_ceil(height);
  if (std::min(log2_width, log2_height) > 4) {
    *offset_x = 0;
    *offset_y = 0;
    return false;
  }

  if (packed_tile < 3) {
    if (log2_width > log2_height) {
      *offset_x = 0;
      *offset_y = 16 >> packed_tile;
    } else {
      *offset_x = 16 >> packed_tile;
      *offset_y = 0;
    }
  } else {
    if (log2_width > log2_height) {
      *offset_x = 16 >> (packed_tile - 2);
      *offset_y = 0;
    } else {
      *offset_x = 0;
      *offset_y = 16 >> (packed_tile - 2);
    }
  }

  *offset_x /= format_info->block_width;
  *offset_y /= format_info->block_height;
  return true;
}

bool TextureInfo::GetPackedMipOffset(u32 packed_tile, u32* offset_x, u32* offset_y) const {
  if (!has_packed_mips) {
    *offset_x = 0;
    *offset_y = 0;
    return false;
  }
  return GetPackedMipOffset(rex::next_pow2(width_minus_1 + 1), rex::next_pow2(height_minus_1 + 1),
                            GetFormatInfo(), packed_tile, offset_x, offset_y);
}

u64 TextureInfo::Hash() const {
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  const auto add = [&state](const auto&... fields) {
    (XXH3_64bits_update(&state, &fields, sizeof(fields)), ...);
  };
  add(format, endianness, dimension, width_minus_1, height_minus_1, depth_or_array_size_minus_1,
      pitch, mip_min_level, mip_max_level, is_stacked, is_tiled, has_packed_mips,
      memory.base_address, memory.base_size, memory.mip_address, memory.mip_size, extent.pitch,
      extent.height, extent.block_width, extent.block_height, extent.block_pitch_h,
      extent.block_pitch_v, extent.depth);
  return XXH3_64bits_digest(&state);
}

void TextureInfo::SetupMemoryRanges(u32 base_address, u32 mip_address) {
  u32 bytes_per_block = GetFormatInfo()->GetBytesPerBlock();

  memory.base_address = 0;
  memory.base_size = 0;
  memory.mip_address = 0;
  memory.mip_size = 0;

  if (mip_min_level == 0 && base_address) {
    memory.base_address = base_address;
    memory.base_size = GetMipExtent(0, true).GetVisibleBlocks() * bytes_per_block;
  }

  if (mip_min_level == 0 && mip_max_level == 0) {
    return;
  }

  if (mip_min_level == 0 && base_address == mip_address) {
    return;
  }

  if (mip_min_level > 0) {
    if ((base_address && !mip_address) || (base_address == mip_address)) {
      mip_address = base_address;
      base_address = 0;
    } else if (base_address || !mip_address) {
      REX_ASSERT_ALWAYS();
    }
  }

  memory.mip_address = mip_address;

  if (!has_packed_mips) {
    for (u32 mip = std::max(1u, mip_min_level); mip < mip_max_level; mip++) {
      memory.mip_size += GetMipExtent(mip, true).GetAllBlocks() * bytes_per_block;
    }
    memory.mip_size += GetMipExtent(mip_max_level, true).GetVisibleBlocks() * bytes_per_block;
    return;
  }

  u32 width_pow2 = rex::next_pow2(width_minus_1 + 1);
  u32 height_pow2 = rex::next_pow2(height_minus_1 + 1);

  u32 packed_mip_base = std::max(1u, mip_min_level);
  for (u32 mip = packed_mip_base; mip < mip_max_level; mip++, packed_mip_base++) {
    u32 mip_width = std::max(width_pow2 >> mip, 1u);
    u32 mip_height = std::max(height_pow2 >> mip, 1u);
    if (std::min(mip_width, mip_height) <= 16) {
      break;
    }
    memory.mip_size += GetMipExtent(mip, true).GetAllBlocks() * bytes_per_block;
  }
}

}  // namespace rex::xenos
