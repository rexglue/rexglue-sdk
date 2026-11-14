/**
 * @file        xenos/fetch.h
 * @brief       Fetch constant layouts, texture sampling enums and limits
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/gpu/xenos.h (Ben Vanik, 2020).
 */

#pragma once

#include <algorithm>

#include <rex/assert.h>
#include <rex/types.h>
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>

namespace rex::xenos {

/** Texture fetch constant dimension. Stacked textures are stored as 2D. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class DataDimension : u32 {
  k1D = 0,
  k2DOrStacked = 1,
  k3D = 2,
  kCube = 3,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ClampMode : u32 {
  kRepeat = 0,
  kMirroredRepeat = 1,
  kClampToEdge = 2,
  kMirrorClampToEdge = 3,    /**< Mirror once, then clamp to the edge texel. */
  kClampToHalfway = 4,       /**< Clamp halfway between the edge texel and the border. */
  kMirrorClampToHalfway = 5, /**< Mirror once, then clamp halfway. */
  kClampToBorder = 6,
  kMirrorClampToBorder = 7, /**< Mirror once, then clamp to the border color. */
};

/**
 * Reports whether a clamp mode samples the border color.
 *
 * @return True when the clamp mode is kClampToBorder or kMirrorClampToBorder.
 */
constexpr bool ClampModeUsesBorder(ClampMode clamp_mode) {
  return clamp_mode == ClampMode::kClampToBorder || clamp_mode == ClampMode::kMirrorClampToBorder;
}

/** Texture component signedness (TEX_FORMAT_COMP, GPUSIGN on Xenos). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TextureSign : u32 {
  kUnsigned = 0,
  kSigned = 1,         /**< Two's complement data. */
  kUnsignedBiased = 2, /**< Unsigned data remapped as 2 * color - 1. */
  kGamma = 3,          /**< Linearized when sampled. */
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TextureFilter : u32 {
  kPoint = 0,
  kLinear = 1,
  kBaseMap = 2,       /**< Mip filter only, samples the base level without mip selection. */
  kUseFetchConst = 3, /**< Use the filter from the fetch constant. */
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class AnisoFilter : u32 {
  kDisabled = 0,
  kMax11 = 1,
  kMax21 = 2,
  kMax41 = 3,
  kMax81 = 4,
  kMax161 = 5,
  kUseFetchConst = 7, /**< Use the ratio from the fetch constant. */
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class BorderColor : u32 {
  k_ABGR_Black = 0,
  k_ABGR_White = 1,
  k_ACBYCR_Black = 2, /**< Host border (0.5, 0, 0.5) in unsigned (Cr, Y, Cb). */
  k_ACBCRY_Black = 3, /**< Host border (0, 0.5, 0.5) in unsigned (Y, Cr, Cb). */
};

/** Dimension of the tfetch instruction. Stacked textures are accessed with tfetch3D. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class FetchOpDimension : u32 {
  k1D = 0,
  k2D = 1,
  k3DOrStacked = 2,
  kCube = 3,
};

/** Returns the number of texture coordinate components a fetch dimension uses. It is 1, 2 or 3. */
inline int GetFetchOpDimensionComponentCount(FetchOpDimension dimension) {
  switch (dimension) {
    case FetchOpDimension::k1D:
      return 1;
    case FetchOpDimension::k2D:
      return 2;
    case FetchOpDimension::k3DOrStacked:
    case FetchOpDimension::kCube:
      return 3;
    default:
      REX_UNHANDLED_CASE(dimension);
      return 1;
  }
}

/**
 * Packs four TextureSwizzle component selectors into a 12 bit swizzle.
 *
 * @param x Selector suffix for the first component.
 * @param y Selector suffix for the second component.
 * @param z Selector suffix for the third component.
 * @param w Selector suffix for the fourth component.
 * @code
 * u32 swizzle = REXGPU_SWIZZLE(R, G, B, A);
 * @endcode
 */
#define REXGPU_SWIZZLE(x, y, z, w)                                                         \
  (((::rex::xenos::REXGPU_SWIZZLE_##x) << 0) | ((::rex::xenos::REXGPU_SWIZZLE_##y) << 3) | \
   ((::rex::xenos::REXGPU_SWIZZLE_##z) << 6) | ((::rex::xenos::REXGPU_SWIZZLE_##w) << 9))

enum TextureSwizzle : u16 {
  REXGPU_SWIZZLE_X = 0,
  REXGPU_SWIZZLE_R = 0,
  REXGPU_SWIZZLE_Y = 1,
  REXGPU_SWIZZLE_G = 1,
  REXGPU_SWIZZLE_Z = 2,
  REXGPU_SWIZZLE_B = 2,
  REXGPU_SWIZZLE_W = 3,
  REXGPU_SWIZZLE_A = 3,
  REXGPU_SWIZZLE_0 = 4,
  REXGPU_SWIZZLE_1 = 5,
  REXGPU_SWIZZLE_RRRR = REXGPU_SWIZZLE(R, R, R, R),
  REXGPU_SWIZZLE_RGGG = REXGPU_SWIZZLE(R, G, G, G),
  REXGPU_SWIZZLE_RGBB = REXGPU_SWIZZLE(R, G, B, B),
  REXGPU_SWIZZLE_RGBA = REXGPU_SWIZZLE(R, G, B, A),
  REXGPU_SWIZZLE_0000 = REXGPU_SWIZZLE(0, 0, 0, 0),
};

/** Loop constant from the shader loop constant register range. */
union alignas(u32) LoopConstant {
  u32 value;
  struct {
    u32 count : 8;
    u32 start : 8; /**< aL is iterator * step + start, in [-256, 256]. */
    i32 step : 8;
    u32 pad_24 : 8;
  };
};
REX_STATIC_ASSERT_SIZE(LoopConstant, sizeof(u32));

/** Fetch constant type (SQ_TEX_VTX_INVALID/VALID_TEXTURE/BUFFER). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class FetchConstantType : u32 {
  kInvalidTexture,
  kInvalidVertex,
  kTexture,
  kVertex,
};

constexpr u32 kTextureFetchConstantCount = 32;
/** Three per texture fetch constant slot. */
constexpr u32 kVertexFetchConstantCount = 3 * kTextureFetchConstantCount;

/** Vertex fetch constant from the shader fetch constant register range. */
union alignas(u32) VertexFetch {
  struct {
    u32 dword_0;
    u32 dword_1;
  };
  struct {
    FetchConstantType type : 2;
    u32 address : 30; /**< Address in dwords. */

    Endian endian : 2;
    u32 size : 24; /**< Size in words. */
    u32 pad_26 : 6;
  };
};
REX_STATIC_ASSERT_SIZE(VertexFetch, sizeof(u32) * 2);

/** Log2 of the byte alignment of texture subresources, omitted from base and mip addresses. */
constexpr u32 kTextureSubresourceAlignmentBytesLog2 = 12;
constexpr u32 kTextureSubresourceAlignmentBytes = 1 << kTextureSubresourceAlignmentBytesLog2;

constexpr u32 kTexture1DMaxWidthLog2 = 24;
constexpr u32 kTexture1DMaxWidth = 1 << kTexture1DMaxWidthLog2;
constexpr u32 kTexture2DCubeMaxWidthHeightLog2 = 13;
constexpr u32 kTexture2DCubeMaxWidthHeight = 1 << kTexture2DCubeMaxWidthHeightLog2;
constexpr u32 kTexture2DMaxStackDepthLog2 = 6;
constexpr u32 kTexture2DMaxStackDepth = 1 << kTexture2DMaxStackDepthLog2;
constexpr u32 kTexture3DMaxWidthHeightLog2 = 11;
constexpr u32 kTexture3DMaxWidthHeight = 1 << kTexture3DMaxWidthHeightLog2;
constexpr u32 kTexture3DMaxDepthLog2 = 10;
constexpr u32 kTexture3DMaxDepth = 1 << kTexture3DMaxDepthLog2;

constexpr u32 kTextureMaxMips =
    std::max(kTexture2DCubeMaxWidthHeightLog2, kTexture3DMaxWidthHeightLog2) + 1;

constexpr u32 kTextureTileWidthHeightLog2 = 5;
constexpr u32 kTextureTileWidthHeight = 1 << kTextureTileWidthHeightLog2;
/** Log2 of the 3D texture tile depth. Depths 0 to 3 and 4 to 7 use separate address ranges. */
constexpr u32 kTextureTileDepthLog2 = 2;
constexpr u32 kTextureTileDepth = 1 << kTextureTileDepthLog2;

/**
 * Returns log2 of the X period of the tiled texture address function.
 *
 * Periods are 128x128 for 2D 1bpb, 64x64 for 2D 2bpb, 32x32 for 2D 4bpb and above, 64x32x8 for
 * 3D 1bpb and 32x32x8 for 3D 2bpb and above.
 */
constexpr u32 GetTextureTiledXBaseGranularityLog2(bool is_3d, u32 bytes_per_block_log2) {
  return 7 - std::min(UINT32_C(2), bytes_per_block_log2 + static_cast<u32>(is_3d));
}

/** Returns log2 of the Y period of the tiled texture address function. */
constexpr u32 GetTextureTiledYBaseGranularityLog2(bool is_3d, u32 bytes_per_block_log2) {
  return is_3d ? 5 : (7 - std::min(UINT32_C(2), bytes_per_block_log2));
}

constexpr u32 kTextureTiledZBaseGranularityLog2 = 3;
constexpr u32 kTextureTiledZBaseGranularity = 1 << kTextureTiledZBaseGranularityLog2;

constexpr u32 kTextureLinearRowAlignmentBytesLog2 = 8;
constexpr u32 kTextureLinearRowAlignmentBytes = 1 << kTextureLinearRowAlignmentBytesLog2;

/** Texture fetch constant from the shader fetch constant register range. */
union alignas(u32) TextureFetch {
  struct {
    u32 dword_0;
    u32 dword_1;
    u32 dword_2;
    u32 dword_3;
    u32 dword_4;
    u32 dword_5;
  };
  struct {
    FetchConstantType type : 2;
    TextureSign sign_x : 2; /**< Signedness before the swizzle. */
    TextureSign sign_y : 2;
    TextureSign sign_z : 2;
    TextureSign sign_w : 2;
    ClampMode clamp_x : 3;
    ClampMode clamp_y : 3;
    ClampMode clamp_z : 3;
    u32 pad_19 : 3;
    u32 pitch : 9; /**< Base row pitch in pixels >> 5. */
    u32 tiled : 1;

    TextureFormat format : 6;
    Endian endianness : 2;
    u32 request_size : 2;
    u32 stacked : 1;
    u32 nearest_clamp_policy : 1; /**< Direct3D or OpenGL nearest clamp. */
    u32 base_address : 20;        /**< Address shifted right by 12. */

    union {
      struct {
        u32 width : 24; /**< Width minus 1. */
        u32 pad_24 : 8;
      } size_1d;
      struct {
        u32 width : 13;      /**< Width minus 1. */
        u32 height : 13;     /**< Height minus 1. */
        u32 stack_depth : 6; /**< Stack depth minus 1, ignored unless stacked, 5 for cube maps. */
      } size_2d;
      struct {
        u32 width : 11;  /**< Width minus 1. */
        u32 height : 11; /**< Height minus 1. */
        u32 depth : 10;  /**< Depth minus 1. */
      } size_3d;
    };

    u32 num_format : 1; /**< Fraction or integer. */
    u32 swizzle : 12;   /**< XYZW TextureSwizzle, 3 bits each. */
    i32 exp_adjust : 6;
    TextureFilter mag_filter : 2;
    TextureFilter min_filter : 2;
    TextureFilter mip_filter : 2;
    AnisoFilter aniso_filter : 3;
    ArbitraryFilter arbitrary_filter : 3;
    u32 border_size : 1;

    u32 vol_mag_filter : 1;
    u32 vol_min_filter : 1;
    u32 mip_min_level : 4;
    u32 mip_max_level : 4;
    u32 mag_aniso_walk : 1;
    u32 min_aniso_walk : 1;
    i32 lod_bias : 10; /**< LOD bias with 5 fractional bits. */
    i32 grad_exp_adjust_h : 5;
    i32 grad_exp_adjust_v : 5;

    BorderColor border_color : 2;
    u32 force_bc_w_to_max : 1;
    u32 tri_clamp : 2;
    i32 aniso_bias : 4;
    DataDimension dimension : 2;
    u32 packed_mips : 1;
    u32 mip_address : 20; /**< Address shifted right by 12. */
  };
};
REX_STATIC_ASSERT_SIZE(TextureFetch, sizeof(u32) * 6);

/** Six fetch constant dwords, as one texture fetch or three vertex fetches. */
union alignas(u32) FetchGroup {
  struct {
    u32 dword_0;
    u32 dword_1;
    u32 dword_2;
    u32 dword_3;
    u32 dword_4;
    u32 dword_5;
  };
  TextureFetch texture_fetch;
  struct {
    VertexFetch vertex_fetch_0;
    VertexFetch vertex_fetch_1;
    VertexFetch vertex_fetch_2;
  };
  struct {
    u32 type_0 : 2;
    u32 data_0_a : 30;
    u32 data_0_b : 32;
    u32 type_1 : 2;
    u32 data_1_a : 30;
    u32 data_1_b : 32;
    u32 type_2 : 2;
    u32 data_2_a : 30;
    u32 data_2_b : 32;
  };
};
REX_STATIC_ASSERT_SIZE(FetchGroup, sizeof(u32) * 6);

/**
 * Memory export stream constant.
 *
 * A shader writes this constant plus an element offset to eA, usually with
 * `mad eA, r#, const0100, c#`, then writes up to 5 elements to eM0 to eM4. Each field is
 * packed as a normalized floating point number so the Xenos ALU does not flush it.
 * - X holds the base address in dwords with 0b01 in the top 2 bits.
 * - Y holds the element offset added to 2^23.
 * - Z holds the format fields with an exponent of 23.
 * - W holds the element count with an exponent of 23.
 *
 * Exports are discarded if the index is above the maximum, the index sign bit is set, or the
 * index exponent is not 23. Alloc instructions and `serialize` end the current export.
 */
union alignas(u32) MemExportStream {
  struct {
    u32 dword_0;
    u32 dword_1;
    u32 dword_2;
    u32 dword_3;
  };
  struct {
    u32 base_address : 30; /**< Physical address >> 2. */
    u32 const_0x1 : 2;     /**< 0b01. */

    u32 const_0x4b000000; /**< Dword 1, 2^23 with the offset added by the shader. */

    Endian128 endianness : 3;
    u32 unused_0 : 5;
    ColorFormat format : 6;
    u32 unused_1 : 2;
    SurfaceNumberFormat num_format : 3;
    u32 red_blue_swap : 1;
    u32 const_0x4b0 : 12; /**< Exponent 23. */

    u32 index_count : 23;
    u32 const_0x96 : 9; /**< Exponent 23. */
  };
};
REX_STATIC_ASSERT_SIZE(MemExportStream, sizeof(u32) * 4);

}  // namespace rex::xenos
