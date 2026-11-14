/**
 * @file        xenos/format.h
 * @brief       Data, texture, vertex and render target formats and conversions
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
#include <bit>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex::xenos {

/** Endian swap mode for 32 bit data. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class Endian : u32 {
  kNone = 0,
  k8in16 = 1,  /**< Swap bytes in 16 bit words. */
  k8in32 = 2,  /**< Swap bytes in 32 bit words. */
  k16in32 = 3, /**< Swap 16 bit halves in 32 bit words. */
};

/** Endian swap mode for up to 128 bit data. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class Endian128 : u32 {
  kNone = 0,
  k8in16 = 1,  /**< Swap bytes in 16 bit words. */
  k8in32 = 2,  /**< Swap bytes in 32 bit words. */
  k16in32 = 3, /**< Swap 16 bit halves in 32 bit words. */
  k8in64 = 4,  /**< Swap bytes in 64 bit words. */
  k8in128 = 5, /**< Swap bytes in 128 bit words. */
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class IndexFormat : u32 {
  kInt16,
  kInt32,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SurfaceNumberFormat : u32 {
  kUnsignedRepeatingFraction = 0, /**< Unsigned normalized. */
  kSignedRepeatingFraction = 1,   /**< Signed normalized with scale 2^(n-1) - 1. */
  kUnsignedInteger = 2,
  kSignedInteger = 3,
  kFloat = 7,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ColorRenderTargetFormat : u32 {
  k_8_8_8_8 = 0,          /**< Four 8 bit unorm components. */
  k_8_8_8_8_GAMMA = 1,    /**< Four 8 bit components with gamma. */
  k_2_10_10_10 = 2,       /**< Three 10 bit and one 2 bit unorm component. */
  k_2_10_10_10_FLOAT = 3, /**< 7e3 [0, 32) RGB with unorm alpha. */
  k_16_16 = 4,            /**< Two 16 bit fixed point components in -32...32. */
  k_16_16_16_16 = 5,      /**< Four 16 bit fixed point components in -32...32. */
  k_16_16_FLOAT = 6,
  k_16_16_16_16_FLOAT = 7,
  k_2_10_10_10_AS_10_10_10_10 = 10,       /**< k_2_10_10_10 with 10 bit blending precision. */
  k_2_10_10_10_FLOAT_AS_16_16_16_16 = 12, /**< k_2_10_10_10_FLOAT with 16 bit blending. */
  k_32_FLOAT = 14,
  k_32_32_FLOAT = 15,
};

/**
 * @return Enumerator name, or "kUnknown".
 */
const char* GetColorRenderTargetFormatName(ColorRenderTargetFormat format);

/**
 * Reports whether a color render target format uses 64 bits per sample.
 *
 * @return True when the format is k_16_16_16_16, k_16_16_16_16_FLOAT or k_32_32_FLOAT.
 */
constexpr bool IsColorRenderTargetFormat64bpp(ColorRenderTargetFormat format) {
  return format == ColorRenderTargetFormat::k_16_16_16_16 ||
         format == ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
         format == ColorRenderTargetFormat::k_32_32_FLOAT;
}

/**
 * @return 1, 2 or 4, or 0 for an unknown format.
 */
inline u32 GetColorRenderTargetFormatComponentCount(ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case ColorRenderTargetFormat::k_2_10_10_10:
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case ColorRenderTargetFormat::k_16_16_16_16:
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return 4;
    case ColorRenderTargetFormat::k_16_16:
    case ColorRenderTargetFormat::k_16_16_FLOAT:
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return 2;
    case ColorRenderTargetFormat::k_32_FLOAT:
      return 1;
    default:
      REX_UNHANDLED_CASE(format);
      return 0;
  }
}

/**
 * Strips the blending precision modifier from a color render target format.
 *
 * @return Format with the same packing and value meaning, without the modifier.
 */
constexpr ColorRenderTargetFormat GetStorageColorFormat(ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return ColorRenderTargetFormat::k_2_10_10_10;
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
    default:
      return format;
  }
}

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class DepthRenderTargetFormat : u32 {
  kD24S8 = 0,  /**< 24 bit unorm depth, 8 bit stencil. */
  kD24FS8 = 1, /**< 20e4 [0, 2) floating point depth, 8 bit stencil. */
};

/**
 * @return Enumerator name, or "kUnknown".
 */
const char* GetDepthRenderTargetFormatName(DepthRenderTargetFormat format);

/**
 * Converts a piecewise linear gamma value to linear.
 *
 * @param gamma Gamma value, saturated before conversion.
 * @return Value from 0 to 1 on the linear scale.
 */
f32 PWLGammaToLinear(f32 gamma);

/**
 * Converts a linear value to piecewise linear gamma.
 *
 * @param linear Linear value, saturated before conversion.
 * @return Value from 0 to 1 on the gamma curve.
 */
f32 LinearToPWLGamma(f32 linear);

/**
 * Converts a Xenos 7e3 color value to an IEEE 754 32 bit number without clamping.
 *
 * @param f10 Value in bits 0:9.
 */
f32 Float7e3To32(u32 f10);

/**
 * Converts an IEEE 754 32 bit number to Xenos 20e4 depth.
 *
 * @param value Value to convert. Values not above 0 become 0.
 * @param round_to_nearest_even Round to the nearest even value instead of toward 0.
 * @return 20e4 value in bits 0:23.
 */
u32 Float32To20e4(f32 value, bool round_to_nearest_even);

/**
 * Converts Xenos 20e4 depth to an IEEE 754 32 bit number without clamping.
 *
 * @param f24 Value in bits 0:23.
 */
f32 Float20e4To32(u32 f24);

/** Converts 24 bit unorm depth to an IEEE 754 32 bit number without clamping. */
constexpr f32 UNorm24To32(u32 n24) {
  return static_cast<f32>(n24 + (n24 >> 23)) * (1.0f / static_cast<f32>(1 << 24));
}

constexpr u32 kColorRenderTargetFormatBits = 4;
constexpr u32 kDepthRenderTargetFormatBits = 1;
/** Width of a field that holds either render target format. */
constexpr u32 kRenderTargetFormatBits =
    std::max(kColorRenderTargetFormatBits, kDepthRenderTargetFormatBits);

constexpr u32 kFormatBits = 6;

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TextureFormat : u32 {
  k_1_REVERSE = 0, /**< 1 bit, reversed. */
  k_1 = 1,
  k_8 = 2,
  k_1_5_5_5 = 3,
  k_5_6_5 = 4,
  k_6_5_5 = 5,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_8_A = 8, /**< Sampled as k_8, may hold alpha in resolves. */
  k_8_B = 9, /**< Sampled as k_8, may hold blue in resolves. */
  k_8_8 = 10,
  k_Cr_Y1_Cb_Y0_REP = 11, /**< Packed unorm YCbCr, Cr Y1 Cb Y0 order. */
  k_Y1_Cr_Y0_Cb_REP = 12, /**< Packed unorm YCbCr, Y1 Cr Y0 Cb order. */
  k_16_16_EDRAM = 13,
  k_8_8_8_8_A = 14, /**< Resolved and memexported as k_8_8_8_8. */
  k_4_4_4_4 = 15,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_DXT1 = 18,
  k_DXT2_3 = 19,
  k_DXT4_5 = 20,
  k_16_16_16_16_EDRAM = 21,
  k_24_8 = 22,       /**< 24 bit unorm depth and 8 bit stencil. */
  k_24_8_FLOAT = 23, /**< 20e4 depth and 8 bit stencil. */
  k_16 = 24,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_EXPAND = 27,
  k_16_16_EXPAND = 28,
  k_16_16_16_16_EXPAND = 29,
  k_16_FLOAT = 30,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_AS_8 = 39,
  k_32_AS_8_8 = 40,
  k_16_MPEG = 41,
  k_16_16_MPEG = 42,
  k_8_INTERLACED = 43,
  k_32_AS_8_INTERLACED = 44,
  k_32_AS_8_8_INTERLACED = 45,
  k_16_INTERLACED = 46,
  k_16_MPEG_INTERLACED = 47,
  k_16_16_MPEG_INTERLACED = 48,
  k_DXN = 49, /**< DXN two component block compression. */
  k_8_8_8_8_AS_16_16_16_16 = 50,
  k_DXT1_AS_16_16_16_16 = 51,
  k_DXT2_3_AS_16_16_16_16 = 52,
  k_DXT4_5_AS_16_16_16_16 = 53,
  k_2_10_10_10_AS_16_16_16_16 = 54,
  k_10_11_11_AS_16_16_16_16 = 55,
  k_11_11_10_AS_16_16_16_16 = 56,
  k_32_32_32_FLOAT = 57,
  k_DXT3A = 58,
  k_DXT5A = 59,
  k_CTX1 = 60, /**< CTX1 two component block compression. */
  k_DXT3A_AS_1_1_1_1 = 61,
  k_8_8_8_8_GAMMA_EDRAM = 62,
  k_2_10_10_10_FLOAT_EDRAM = 63, /**< EDRAM 7e3 data. */
};

/** Subset of TextureFormat that render targets can be resolved to. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ColorFormat : u32 {
  k_8 = 2,
  k_1_5_5_5 = 3,
  k_5_6_5 = 4,
  k_6_5_5 = 5,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_8_A = 8,
  k_8_B = 9,
  k_8_8 = 10,
  k_8_8_8_8_A = 14,
  k_4_4_4_4 = 15,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16 = 24,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_FLOAT = 30,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_8_8_8_8_AS_16_16_16_16 = 50,
  k_2_10_10_10_AS_16_16_16_16 = 54,
  k_10_11_11_AS_16_16_16_16 = 55,
  k_11_11_10_AS_16_16_16_16 = 56,
};

/**
 * Reports whether resolving a render target format to a color format copies bits unchanged.
 *
 * Resolves write unsigned data for fixed point formats, so the signed k_16_16 and
 * k_16_16_16_16 render target formats never match.
 *
 * @return True when the resolve is a bitwise copy.
 */
constexpr bool IsColorResolveFormatBitwiseEquivalent(ColorRenderTargetFormat render_target_format,
                                                     ColorFormat color_format) {
  switch (render_target_format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return color_format == ColorFormat::k_8_8_8_8 || color_format == ColorFormat::k_8_8_8_8_A ||
             color_format == ColorFormat::k_8_8_8_8_AS_16_16_16_16;
    case ColorRenderTargetFormat::k_2_10_10_10:
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return color_format == ColorFormat::k_2_10_10_10 ||
             color_format == ColorFormat::k_2_10_10_10_AS_16_16_16_16;
    case ColorRenderTargetFormat::k_16_16_FLOAT:
      return color_format == ColorFormat::k_16_16_FLOAT;
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return color_format == ColorFormat::k_16_16_16_16_FLOAT;
    case ColorRenderTargetFormat::k_32_FLOAT:
      return color_format == ColorFormat::k_32_FLOAT;
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return color_format == ColorFormat::k_32_32_FLOAT;
    default:
      return false;
  }
}

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexFormat : u32 {
  kUndefined = 0,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_32_32_FLOAT = 57,
};

/**
 * @return 1 to 4, or 0 for an unknown format.
 */
inline int GetVertexFormatComponentCount(VertexFormat format) {
  switch (format) {
    case VertexFormat::k_32:
    case VertexFormat::k_32_FLOAT:
      return 1;
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_FLOAT:
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_FLOAT:
      return 2;
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
    case VertexFormat::k_32_32_32_FLOAT:
      return 3;
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_2_10_10_10:
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_16_16_16_16_FLOAT:
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_32_32_32_32_FLOAT:
      return 4;
    default:
      REX_UNHANDLED_CASE(format);
      return 0;
  }
}

/**
 * Returns which 32 bit words of a vertex element hold the used components. The mask uses bits 0:3.
 *
 * @param used_components Mask of used components in bits 0:3.
 */
inline u32 GetVertexFormatNeededWords(VertexFormat format, u32 used_components) {
  REX_ASSERT((used_components & ~static_cast<u32>(0b1111)) == 0);
  if (!used_components) {
    return 0;
  }
  switch (format) {
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_2_10_10_10:
      return 0b0001;
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
      return (used_components & 0b0111) ? 0b0001 : 0b0000;
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_FLOAT:
      return (used_components & 0b0011) ? 0b0001 : 0b0000;
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_16_16_16_16_FLOAT:
      return ((used_components & 0b0011) ? 0b0001 : 0b0000) |
             ((used_components & 0b1100) ? 0b0010 : 0b0000);
    case VertexFormat::k_32:
    case VertexFormat::k_32_FLOAT:
      return used_components & 0b0001;
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_FLOAT:
      return used_components & 0b0011;
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_32_32_32_32_FLOAT:
      return used_components;
    case VertexFormat::k_32_32_32_FLOAT:
      return used_components & 0b0111;
    default:
      REX_UNHANDLED_CASE(format);
      return 0b0000;
  }
}

/**
 * Applies a GPU endian swap to a 16 bit value.
 *
 * @param endianness Swap mode, kNone or k8in16.
 */
inline u16 GPUSwap(u16 value, Endian endianness) {
  switch (endianness) {
    case Endian::kNone:
      return value;
    case Endian::k8in16:
      return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    default:
      REX_UNHANDLED_CASE(endianness);
      return value;
  }
}

inline u32 GPUSwap(u32 value, Endian endianness) {
  switch (endianness) {
    default:
    case Endian::kNone:
      return value;
    case Endian::k8in16:
      return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    case Endian::k8in32:
      return std::byteswap(value);
    case Endian::k16in32:
      return ((value >> 16) & 0xFFFF) | (value << 16);
  }
}

/** Applies a GPU endian swap to the bits of a 32 bit floating point value. */
inline f32 GPUSwap(f32 value, Endian endianness) {
  union {
    u32 i;
    f32 f;
  } v;
  v.f = value;
  v.i = GPUSwap(v.i, endianness);
  return v.f;
}

/**
 * Converts a GPU address to a CPU address.
 *
 * @param p GPU address.
 * @return The address unchanged.
 */
inline u32 GPUToCPU(u32 p) {
  return p;
}

/**
 * Converts a CPU address to a GPU physical address.
 *
 * @param p CPU address.
 * @return Low 29 bits of the address.
 */
inline u32 CPUToGPU(u32 p) {
  return p & 0x1FFFFFFF;
}

/**
 * Converts a 32 bit value to the Xenos 16 bit floating point encoding.
 *
 * The encoding uses the extended range instead of infinity and NaN, and saturates.
 *
 * @param preserve_denormal Keep denormalized results instead of flushing them to 0.
 * @param round_to_nearest_even Round to the nearest even value instead of toward 0.
 * @return 16 bit encoded value.
 */
inline u16 FloatToHalf(f32 value, bool preserve_denormal = false,
                       bool round_to_nearest_even = false) {
  u32 integer_value = std::bit_cast<u32>(value);
  u32 abs_value = integer_value & 0x7FFFFFFFu;
  u32 result;
  if (abs_value >= 0x47FFE000u) {
    result = 0x7FFFu;
  } else {
    if (abs_value < 0x38800000u) {
      if (preserve_denormal) {
        u32 shift = std::min(static_cast<u32>(113u - (abs_value >> 23u)), static_cast<u32>(24u));
        result = (0x800000u | (abs_value & 0x7FFFFFu)) >> shift;
      } else {
        result = 0u;
      }
    } else {
      result = abs_value + 0xC8000000u;
    }
    if (round_to_nearest_even) {
      result += 0xFFFu + ((result >> 13u) & 1u);
    }
    result = (result >> 13u) & 0x7FFFu;
  }
  return static_cast<u16>(result | ((integer_value & 0x80000000u) >> 16u));
}

/**
 * Converts a Xenos 16 bit floating point value to 32 bits.
 *
 * @param value 16 bit encoded value.
 * @param preserve_denormal Keep denormalized inputs instead of flushing them to 0.
 */
inline f32 HalfToFloat(u16 value, bool preserve_denormal = false) {
  u32 mantissa = value & 0x3FFu;
  u32 exponent = (value >> 10u) & 0x1Fu;
  if (!exponent) {
    if (!preserve_denormal) {
      mantissa = 0;
    } else if (mantissa) {
      u32 mantissa_lzcnt = static_cast<u32>(std::countl_zero(mantissa)) - (32u - 11u);
      exponent = static_cast<u32>(1 - static_cast<i32>(mantissa_lzcnt));
      mantissa = (mantissa << mantissa_lzcnt) & 0x3FFu;
    }
    if (!mantissa) {
      exponent = static_cast<u32>(-112);
    }
  }
  u32 result =
      (static_cast<u32>(value & 0x8000u) << 16u) | ((exponent + 112u) << 23u) | (mantissa << 13u);
  return std::bit_cast<f32>(result);
}

}  // namespace rex::xenos
