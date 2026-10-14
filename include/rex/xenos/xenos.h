/**
 * @file        xenos/xenos.h
 * @brief       Xenos GPU register enums, fetch constant layouts and packet helpers.
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

#include <rex/macros.h>
#include <rex/assert.h>
#include <rex/memory/fourcc.h>
#include <rex/types.h>

namespace rex::xenos {

/** Signature that VdSwap writes as the first payload dword of its swap packet. */
constexpr memory::FourCCT kSwapSignature = memory::make_fourcc("SWAP");

/** Shader stage. */
// NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
enum class ShaderType : u32 {
  kVertex = 0, /**< Vertex shader. */
  kPixel = 1,  /**< Pixel shader. */
};

/** Number of low vertex index bits the GPU uses. */
constexpr u32 kVertexIndexBits = 24;
/** Mask of the vertex index bits the GPU uses. */
constexpr u32 kVertexIndexMask = (static_cast<u32>(1) << kVertexIndexBits) - 1;

/**
 * Primitive type of a draw.
 *
 * Types from kExplicitMajorModeForceStart onward always use explicit major mode. When the VGT
 * output path is VGTOutputPath::kTessellationEnable, that range holds tessellation patch types,
 * and the vertex shader receives the patch index instead of control point indices.
 */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PrimitiveType : u32 {
  kNone = 0x00,                        /**< No primitive. */
  kPointList = 0x01,                   /**< Point list. */
  kLineList = 0x02,                    /**< Line list. */
  kLineStrip = 0x03,                   /**< Line strip. */
  kTriangleList = 0x04,                /**< Triangle list. */
  kTriangleFan = 0x05,                 /**< Triangle fan. */
  kTriangleStrip = 0x06,               /**< Triangle strip. */
  kTriangleWithWFlags = 0x07,          /**< Triangle list with W flags. */
  kRectangleList = 0x08,               /**< Rectangle list, 3 vertices per rectangle. */
  kLineLoop = 0x0C,                    /**< Line loop. */
  kQuadList = 0x0D,                    /**< Quad list. */
  kQuadStrip = 0x0E,                   /**< Quad strip. */
  kPolygon = 0x0F,                     /**< Polygon. */
  kExplicitMajorModeForceStart = 0x10, /**< First type that forces explicit major mode. */
  k2DCopyRectListV0 = 0x10,            /**< 2D copy rectangle list, variant 0. */
  k2DCopyRectListV1 = 0x11,            /**< 2D copy rectangle list, variant 1. */
  k2DCopyRectListV2 = 0x12,            /**< 2D copy rectangle list, variant 2. */
  k2DCopyRectListV3 = 0x13,            /**< 2D copy rectangle list, variant 3. */
  k2DFillRectList = 0x14,              /**< 2D fill rectangle list. */
  k2DLineStrip = 0x15,                 /**< 2D line strip. */
  k2DTriStrip = 0x16,                  /**< 2D triangle strip. */
  kLinePatch = 0x10,                   /**< Line patch with tessellation enabled. */
  kTrianglePatch = 0x11,               /**< Triangle patch with tessellation enabled. */
  kQuadPatch = 0x12,                   /**< Quad patch with tessellation enabled. */
};

/** Texture fetch constant dimension. Stacked textures are stored as 2D. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class DataDimension : u32 {
  k1D = 0,          /**< 1D texture. */
  k2DOrStacked = 1, /**< 2D or stacked texture. */
  k3D = 2,          /**< 3D texture. */
  kCube = 3,        /**< Cube texture. */
};

/** Texture coordinate clamp mode. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ClampMode : u32 {
  kRepeat = 0,               /**< Repeat. */
  kMirroredRepeat = 1,       /**< Mirrored repeat. */
  kClampToEdge = 2,          /**< Clamp to the edge texel. */
  kMirrorClampToEdge = 3,    /**< Mirror once, then clamp to the edge texel. */
  kClampToHalfway = 4,       /**< Clamp halfway between the edge texel and the border. */
  kMirrorClampToHalfway = 5, /**< Mirror once, then clamp halfway. */
  kClampToBorder = 6,        /**< Clamp to the border color. */
  kMirrorClampToBorder = 7,  /**< Mirror once, then clamp to the border color. */
};

/**
 * Reports whether a clamp mode samples the border color.
 *
 * @param clamp_mode Clamp mode to test.
 * @return True for kClampToBorder and kMirrorClampToBorder.
 */
constexpr bool ClampModeUsesBorder(ClampMode clamp_mode) {
  return clamp_mode == ClampMode::kClampToBorder || clamp_mode == ClampMode::kMirrorClampToBorder;
}

/** Texture component signedness (TEX_FORMAT_COMP, GPUSIGN on Xenos). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TextureSign : u32 {
  kUnsigned = 0,       /**< Unsigned data. */
  kSigned = 1,         /**< Two's complement data. */
  kUnsignedBiased = 2, /**< Unsigned data remapped as 2 * color - 1. */
  kGamma = 3,          /**< Linearized when sampled. */
};

/** Texture filter. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TextureFilter : u32 {
  kPoint = 0,         /**< Point sampling. */
  kLinear = 1,        /**< Linear filtering. */
  kBaseMap = 2,       /**< Mip filter only, samples the base level without mip selection. */
  kUseFetchConst = 3, /**< Use the filter from the fetch constant. */
};

/** Anisotropic filter ratio. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class AnisoFilter : u32 {
  kDisabled = 0,      /**< Anisotropic filtering off. */
  kMax11 = 1,         /**< Up to 1:1. */
  kMax21 = 2,         /**< Up to 2:1. */
  kMax41 = 3,         /**< Up to 4:1. */
  kMax81 = 4,         /**< Up to 8:1. */
  kMax161 = 5,        /**< Up to 16:1. */
  kUseFetchConst = 7, /**< Use the ratio from the fetch constant. */
};

/** Texture border color. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class BorderColor : u32 {
  k_ABGR_Black = 0,   /**< Black (0, 0, 0). */
  k_ABGR_White = 1,   /**< White (1, 1, 1, 1). */
  k_ACBYCR_Black = 2, /**< Host border (0.5, 0, 0.5) in unsigned (Cr, Y, Cb). */
  k_ACBCRY_Black = 3, /**< Host border (0, 0.5, 0.5) in unsigned (Y, Cr, Cb). */
};

/** Dimension of the tfetch instruction. Stacked textures are accessed with tfetch3D. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class FetchOpDimension : u32 {
  k1D = 0,          /**< 1D fetch. */
  k2D = 1,          /**< 2D fetch. */
  k3DOrStacked = 2, /**< 3D or stacked fetch. */
  kCube = 3,        /**< Cube fetch. */
};

/**
 * Returns the number of texture coordinate components a fetch dimension uses.
 *
 * @param dimension Fetch dimension.
 * @return 1, 2 or 3.
 */
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

/** Interpolation location within a pixel. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SampleLocation : u32 {
  kCentroid = 0, /**< Centroid. */
  kCenter = 1,   /**< Pixel center. */
};

/** Endian swap mode for 32-bit data. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class Endian : u32 {
  kNone = 0,   /**< No swap. */
  k8in16 = 1,  /**< Swap bytes in 16-bit words. */
  k8in32 = 2,  /**< Swap bytes in 32-bit words. */
  k16in32 = 3, /**< Swap 16-bit halves in 32-bit words. */
};

/** Endian swap mode for up to 128-bit data. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class Endian128 : u32 {
  kNone = 0,   /**< No swap. */
  k8in16 = 1,  /**< Swap bytes in 16-bit words. */
  k8in32 = 2,  /**< Swap bytes in 32-bit words. */
  k16in32 = 3, /**< Swap 16-bit halves in 32-bit words. */
  k8in64 = 4,  /**< Swap bytes in 64-bit words. */
  k8in128 = 5, /**< Swap bytes in 128-bit words. */
};

/** Index buffer element size. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class IndexFormat : u32 {
  kInt16, /**< 16-bit indices. */
  kInt32, /**< 32-bit indices. */
};

/** Surface number format (SurfaceNumberX). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SurfaceNumberFormat : u32 {
  kUnsignedRepeatingFraction = 0, /**< Unsigned normalized. */
  kSignedRepeatingFraction = 1,   /**< Signed normalized with scale 2^(n-1) - 1. */
  kUnsignedInteger = 2,           /**< Unsigned integer. */
  kSignedInteger = 3,             /**< Signed integer. */
  kFloat = 7,                     /**< Floating-point. */
};

/** EDRAM multisample count. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class MSAASamples : u32 {
  k1X = 0, /**< 1 sample. */
  k2X = 1, /**< 2 samples, 1x2 per pixel. */
  k4X = 2, /**< 4 samples, 2x2 per pixel. */
};

/** Width of an MSAASamples field. */
constexpr u32 kMSAASamplesBits = 2;

/** Width of a color render target index. */
constexpr u32 kColorRenderTargetIndexBits = 2;
/** Number of color render targets. */
constexpr u32 kMaxColorRenderTargets = 4;

/** Color render target format. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ColorRenderTargetFormat : u32 {
  k_8_8_8_8 = 0,                          /**< 4 8-bit unorm components. */
  k_8_8_8_8_GAMMA = 1,                    /**< 4 8-bit components with gamma. */
  k_2_10_10_10 = 2,                       /**< 3 10-bit and a 2-bit unorm component. */
  k_2_10_10_10_FLOAT = 3,                 /**< 7e3 [0, 32) RGB with unorm alpha. */
  k_16_16 = 4,                            /**< 2 16-bit fixed-point components in -32...32. */
  k_16_16_16_16 = 5,                      /**< 4 16-bit fixed-point components in -32...32. */
  k_16_16_FLOAT = 6,                      /**< 2 16-bit floating-point components. */
  k_16_16_16_16_FLOAT = 7,                /**< 4 16-bit floating-point components. */
  k_2_10_10_10_AS_10_10_10_10 = 10,       /**< k_2_10_10_10 with 10-bit blending precision. */
  k_2_10_10_10_FLOAT_AS_16_16_16_16 = 12, /**< k_2_10_10_10_FLOAT with 16-bit blending. */
  k_32_FLOAT = 14,                        /**< 1 32-bit floating-point component. */
  k_32_32_FLOAT = 15,                     /**< 2 32-bit floating-point components. */
};

/**
 * Returns the name of a color render target format.
 *
 * @param format Color render target format.
 * @return Enumerator name, or "kUnknown".
 */
REX_API const char* GetColorRenderTargetFormatName(ColorRenderTargetFormat format);

/**
 * Reports whether a color render target format uses 64 bits per sample.
 *
 * @param format Color render target format.
 * @return True for k_16_16_16_16, k_16_16_16_16_FLOAT and k_32_32_FLOAT.
 */
constexpr bool IsColorRenderTargetFormat64bpp(ColorRenderTargetFormat format) {
  return format == ColorRenderTargetFormat::k_16_16_16_16 ||
         format == ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
         format == ColorRenderTargetFormat::k_32_32_FLOAT;
}

/**
 * Returns the number of components of a color render target format.
 *
 * @param format Color render target format.
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
 * @param format Color render target format.
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

/** Depth render target format. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class DepthRenderTargetFormat : u32 {
  kD24S8 = 0,  /**< 24-bit unorm depth, 8-bit stencil. */
  kD24FS8 = 1, /**< 20e4 [0, 2) floating-point depth, 8-bit stencil. */
};

/**
 * Returns the name of a depth render target format.
 *
 * @param format Depth render target format.
 * @return Enumerator name, or "kUnknown".
 */
REX_API const char* GetDepthRenderTargetFormatName(DepthRenderTargetFormat format);

/**
 * Converts a piecewise linear gamma value to linear.
 *
 * @param gamma Gamma value, saturated before conversion.
 * @return Linear value in [0, 1].
 */
REX_API f32 PWLGammaToLinear(f32 gamma);

/**
 * Converts a linear value to piecewise linear gamma.
 *
 * @param linear Linear value, saturated before conversion.
 * @return Gamma value in [0, 1].
 */
REX_API f32 LinearToPWLGamma(f32 linear);

/**
 * Converts a Xenos 7e3 color value to an IEEE-754 32-bit number without clamping.
 *
 * @param f10 Value in bits 0:9.
 * @return Converted value.
 */
REX_API f32 Float7e3To32(u32 f10);

/**
 * Converts an IEEE-754 32-bit number to Xenos 20e4 depth.
 *
 * @param value Value to convert. Values not above 0 become 0.
 * @param round_to_nearest_even Round to the nearest even value instead of towards 0.
 * @return 20e4 value in bits 0:23.
 */
REX_API u32 Float32To20e4(f32 value, bool round_to_nearest_even);

/**
 * Converts Xenos 20e4 depth to an IEEE-754 32-bit number without clamping.
 *
 * @param f24 Value in bits 0:23.
 * @return Converted value.
 */
REX_API f32 Float20e4To32(u32 f24);

/**
 * Converts 24-bit unorm depth to an IEEE-754 32-bit number without clamping.
 *
 * @param n24 Unorm value.
 * @return Converted value.
 */
constexpr f32 UNorm24To32(u32 n24) {
  return static_cast<f32>(n24 + (n24 >> 23)) * (1.0f / static_cast<f32>(1 << 24));
}

/**
 * Scale from PA_SU_POLY_OFFSET_FRONT/BACK_SCALE subpixel units to pixel units.
 *
 * Host APIs compute the slope from adjacent pixel differences, as max(|ddx(z)|, |ddy(z)|).
 */
constexpr f32 kPolygonOffsetScaleSubpixelUnit = 1.0f / 16.0f;

/** Width of a ColorRenderTargetFormat field. */
constexpr u32 kColorRenderTargetFormatBits = 4;
/** Width of a DepthRenderTargetFormat field. */
constexpr u32 kDepthRenderTargetFormatBits = 1;
/** Width of a field that holds either render target format. */
constexpr u32 kRenderTargetFormatBits =
    std::max(kColorRenderTargetFormatBits, kDepthRenderTargetFormatBits);

/** EDRAM tile width in 32bpp samples. */
constexpr u32 kEDRAMTileWidthSamples = 80;
/** EDRAM tile height in 32bpp samples. */
constexpr u32 kEDRAMTileHeightSamples = 16;
/** Number of EDRAM tiles. Tile addressing wraps around at this count. */
constexpr u32 kEDRAMTileCount = 2048;
/** EDRAM size in bytes. */
constexpr u32 kEDRAMSizeBytes = static_cast<u32>(u64{kEDRAMTileCount} * kEDRAMTileHeightSamples *
                                                 kEDRAMTileWidthSamples * sizeof(u32));

/** Width of RB_SURFACE_INFO::surface_pitch. */
constexpr u32 kEDRAMPitchPixelsBits = 14;
/** Usable width of RB_COLOR_INFO::color_base and RB_DEPTH_INFO::depth_base. */
constexpr u32 kEDRAMBaseTilesBits = 11;

/**
 * Returns the EDRAM row pitch of a surface in tiles.
 *
 * @param pitch_pixels Surface pitch in pixels.
 * @param msaa_samples Sample count. 4x doubles the width in samples.
 * @param is_64bpp Whether the surface uses 64 bits per sample, which doubles the tile count.
 * @return Pitch in tiles.
 */
constexpr u32 GetSurfacePitchTiles(u32 pitch_pixels, MSAASamples msaa_samples, bool is_64bpp) {
  u32 pitch_samples = pitch_pixels << u32(msaa_samples >= MSAASamples::k4X);
  u32 pitch_tiles = (pitch_samples + (kEDRAMTileWidthSamples - 1)) / kEDRAMTileWidthSamples;
  if (is_64bpp) {
    pitch_tiles <<= 1;
  }
  return pitch_tiles;
}

/** Bits needed for the largest GetSurfacePitchTiles result with a 16383 pixel pitch. */
constexpr u32 kEDRAMPitchTilesBits = 10;

/** Width of a texture format field. */
constexpr u32 kFormatBits = 6;

/** Texture format (a2xx_sq_surfaceformat). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TextureFormat : u32 {
  k_1_REVERSE = 0,                  /**< 1-bit, reversed. */
  k_1 = 1,                          /**< 1-bit. */
  k_8 = 2,                          /**< 1 8-bit component. */
  k_1_5_5_5 = 3,                    /**< 3 5-bit and a 1-bit component. */
  k_5_6_5 = 4,                      /**< 5, 6 and 5-bit components. */
  k_6_5_5 = 5,                      /**< 6, 5 and 5-bit components. */
  k_8_8_8_8 = 6,                    /**< 4 8-bit components. */
  k_2_10_10_10 = 7,                 /**< 3 10-bit and a 2-bit component. */
  k_8_A = 8,                        /**< Sampled as k_8, may hold alpha in resolves. */
  k_8_B = 9,                        /**< Sampled as k_8, may hold blue in resolves. */
  k_8_8 = 10,                       /**< 2 8-bit components. */
  k_Cr_Y1_Cb_Y0_REP = 11,           /**< Packed unorm YCbCr, Cr Y1 Cb Y0 order. */
  k_Y1_Cr_Y0_Cb_REP = 12,           /**< Packed unorm YCbCr, Y1 Cr Y0 Cb order. */
  k_16_16_EDRAM = 13,               /**< EDRAM k_16_16 data. */
  k_8_8_8_8_A = 14,                 /**< Resolved and memexported as k_8_8_8_8. */
  k_4_4_4_4 = 15,                   /**< 4 4-bit components. */
  k_10_11_11 = 16,                  /**< 10, 11 and 11-bit components. */
  k_11_11_10 = 17,                  /**< 11, 11 and 10-bit components. */
  k_DXT1 = 18,                      /**< DXT1 block compression. */
  k_DXT2_3 = 19,                    /**< DXT2 and DXT3 block compression. */
  k_DXT4_5 = 20,                    /**< DXT4 and DXT5 block compression. */
  k_16_16_16_16_EDRAM = 21,         /**< EDRAM k_16_16_16_16 data. */
  k_24_8 = 22,                      /**< 24-bit unorm depth and 8-bit stencil. */
  k_24_8_FLOAT = 23,                /**< 20e4 depth and 8-bit stencil. */
  k_16 = 24,                        /**< 1 16-bit component. */
  k_16_16 = 25,                     /**< 2 16-bit components. */
  k_16_16_16_16 = 26,               /**< 4 16-bit components. */
  k_16_EXPAND = 27,                 /**< 1 16-bit component, expand variant. */
  k_16_16_EXPAND = 28,              /**< 2 16-bit components, expand variant. */
  k_16_16_16_16_EXPAND = 29,        /**< 4 16-bit components, expand variant. */
  k_16_FLOAT = 30,                  /**< 1 16-bit floating-point component. */
  k_16_16_FLOAT = 31,               /**< 2 16-bit floating-point components. */
  k_16_16_16_16_FLOAT = 32,         /**< 4 16-bit floating-point components. */
  k_32 = 33,                        /**< 1 32-bit component. */
  k_32_32 = 34,                     /**< 2 32-bit components. */
  k_32_32_32_32 = 35,               /**< 4 32-bit components. */
  k_32_FLOAT = 36,                  /**< 1 32-bit floating-point component. */
  k_32_32_FLOAT = 37,               /**< 2 32-bit floating-point components. */
  k_32_32_32_32_FLOAT = 38,         /**< 4 32-bit floating-point components. */
  k_32_AS_8 = 39,                   /**< 32-bit data as 8-bit. */
  k_32_AS_8_8 = 40,                 /**< 32-bit data as 8_8. */
  k_16_MPEG = 41,                   /**< 1 16-bit MPEG component. */
  k_16_16_MPEG = 42,                /**< 2 16-bit MPEG components. */
  k_8_INTERLACED = 43,              /**< Interlaced k_8. */
  k_32_AS_8_INTERLACED = 44,        /**< Interlaced k_32_AS_8. */
  k_32_AS_8_8_INTERLACED = 45,      /**< Interlaced k_32_AS_8_8. */
  k_16_INTERLACED = 46,             /**< Interlaced k_16. */
  k_16_MPEG_INTERLACED = 47,        /**< Interlaced k_16_MPEG. */
  k_16_16_MPEG_INTERLACED = 48,     /**< Interlaced k_16_16_MPEG. */
  k_DXN = 49,                       /**< DXN 2-component block compression. */
  k_8_8_8_8_AS_16_16_16_16 = 50,    /**< k_8_8_8_8 as 16_16_16_16. */
  k_DXT1_AS_16_16_16_16 = 51,       /**< k_DXT1 as 16_16_16_16. */
  k_DXT2_3_AS_16_16_16_16 = 52,     /**< k_DXT2_3 as 16_16_16_16. */
  k_DXT4_5_AS_16_16_16_16 = 53,     /**< k_DXT4_5 as 16_16_16_16. */
  k_2_10_10_10_AS_16_16_16_16 = 54, /**< k_2_10_10_10 as 16_16_16_16. */
  k_10_11_11_AS_16_16_16_16 = 55,   /**< k_10_11_11 as 16_16_16_16. */
  k_11_11_10_AS_16_16_16_16 = 56,   /**< k_11_11_10 as 16_16_16_16. */
  k_32_32_32_FLOAT = 57,            /**< 3 32-bit floating-point components. */
  k_DXT3A = 58,                     /**< DXT3 alpha block compression. */
  k_DXT5A = 59,                     /**< DXT5 alpha block compression. */
  k_CTX1 = 60,                      /**< CTX1 2-component block compression. */
  k_DXT3A_AS_1_1_1_1 = 61,          /**< k_DXT3A as 1_1_1_1. */
  k_8_8_8_8_GAMMA_EDRAM = 62,       /**< EDRAM k_8_8_8_8 gamma data. */
  k_2_10_10_10_FLOAT_EDRAM = 63,    /**< EDRAM 7e3 data. */
};

/** Subset of TextureFormat that render targets can be resolved to. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ColorFormat : u32 {
  k_8 = 2,                          /**< TextureFormat::k_8. */
  k_1_5_5_5 = 3,                    /**< TextureFormat::k_1_5_5_5. */
  k_5_6_5 = 4,                      /**< TextureFormat::k_5_6_5. */
  k_6_5_5 = 5,                      /**< TextureFormat::k_6_5_5. */
  k_8_8_8_8 = 6,                    /**< TextureFormat::k_8_8_8_8. */
  k_2_10_10_10 = 7,                 /**< TextureFormat::k_2_10_10_10. */
  k_8_A = 8,                        /**< TextureFormat::k_8_A. */
  k_8_B = 9,                        /**< TextureFormat::k_8_B. */
  k_8_8 = 10,                       /**< TextureFormat::k_8_8. */
  k_8_8_8_8_A = 14,                 /**< TextureFormat::k_8_8_8_8_A. */
  k_4_4_4_4 = 15,                   /**< TextureFormat::k_4_4_4_4. */
  k_10_11_11 = 16,                  /**< TextureFormat::k_10_11_11. */
  k_11_11_10 = 17,                  /**< TextureFormat::k_11_11_10. */
  k_16 = 24,                        /**< TextureFormat::k_16. */
  k_16_16 = 25,                     /**< TextureFormat::k_16_16. */
  k_16_16_16_16 = 26,               /**< TextureFormat::k_16_16_16_16. */
  k_16_FLOAT = 30,                  /**< TextureFormat::k_16_FLOAT. */
  k_16_16_FLOAT = 31,               /**< TextureFormat::k_16_16_FLOAT. */
  k_16_16_16_16_FLOAT = 32,         /**< TextureFormat::k_16_16_16_16_FLOAT. */
  k_32_FLOAT = 36,                  /**< TextureFormat::k_32_FLOAT. */
  k_32_32_FLOAT = 37,               /**< TextureFormat::k_32_32_FLOAT. */
  k_32_32_32_32_FLOAT = 38,         /**< TextureFormat::k_32_32_32_32_FLOAT. */
  k_8_8_8_8_AS_16_16_16_16 = 50,    /**< TextureFormat::k_8_8_8_8_AS_16_16_16_16. */
  k_2_10_10_10_AS_16_16_16_16 = 54, /**< TextureFormat::k_2_10_10_10_AS_16_16_16_16. */
  k_10_11_11_AS_16_16_16_16 = 55,   /**< TextureFormat::k_10_11_11_AS_16_16_16_16. */
  k_11_11_10_AS_16_16_16_16 = 56,   /**< TextureFormat::k_11_11_10_AS_16_16_16_16. */
};

/**
 * Reports whether resolving a render target format to a color format copies bits unchanged.
 *
 * Resolves write unsigned data for fixed-point formats, so the signed k_16_16 and
 * k_16_16_16_16 render target formats never match.
 *
 * @param render_target_format Source render target format.
 * @param color_format Destination color format.
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

/** Vertex fetch format. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexFormat : u32 {
  kUndefined = 0,           /**< No format. */
  k_8_8_8_8 = 6,            /**< TextureFormat::k_8_8_8_8. */
  k_2_10_10_10 = 7,         /**< TextureFormat::k_2_10_10_10. */
  k_10_11_11 = 16,          /**< TextureFormat::k_10_11_11. */
  k_11_11_10 = 17,          /**< TextureFormat::k_11_11_10. */
  k_16_16 = 25,             /**< TextureFormat::k_16_16. */
  k_16_16_16_16 = 26,       /**< TextureFormat::k_16_16_16_16. */
  k_16_16_FLOAT = 31,       /**< TextureFormat::k_16_16_FLOAT. */
  k_16_16_16_16_FLOAT = 32, /**< TextureFormat::k_16_16_16_16_FLOAT. */
  k_32 = 33,                /**< TextureFormat::k_32. */
  k_32_32 = 34,             /**< TextureFormat::k_32_32. */
  k_32_32_32_32 = 35,       /**< TextureFormat::k_32_32_32_32. */
  k_32_FLOAT = 36,          /**< TextureFormat::k_32_FLOAT. */
  k_32_32_FLOAT = 37,       /**< TextureFormat::k_32_32_FLOAT. */
  k_32_32_32_32_FLOAT = 38, /**< TextureFormat::k_32_32_32_32_FLOAT. */
  k_32_32_32_FLOAT = 57,    /**< TextureFormat::k_32_32_32_FLOAT. */
};

/**
 * Returns the number of components of a vertex format.
 *
 * @param format Vertex format.
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
 * Returns which 32-bit words of a vertex element hold the used components.
 *
 * @param format Vertex format.
 * @param used_components Mask of used components in bits 0:3.
 * @return Mask of needed words in bits 0:3.
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

/** Depth, stencil and alpha test comparison function. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class CompareFunction : u32 {
  kNever = 0b000,        /**< Never passes. */
  kLess = 0b001,         /**< Passes if less. */
  kEqual = 0b010,        /**< Passes if equal. */
  kLessEqual = 0b011,    /**< Passes if less or equal. */
  kGreater = 0b100,      /**< Passes if greater. */
  kNotEqual = 0b101,     /**< Passes if not equal. */
  kGreaterEqual = 0b110, /**< Passes if greater or equal. */
  kAlways = 0b111,       /**< Always passes. */
};

/** Stencil operation. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class StencilOp : u32 {
  kKeep = 0,           /**< Keep the value. */
  kZero = 1,           /**< Set to 0. */
  kReplace = 2,        /**< Replace with the reference. */
  kIncrementClamp = 3, /**< Increment and clamp. */
  kDecrementClamp = 4, /**< Decrement and clamp. */
  kInvert = 5,         /**< Invert the bits. */
  kIncrementWrap = 6,  /**< Increment and wrap. */
  kDecrementWrap = 7,  /**< Decrement and wrap. */
};

/** Blend factor (adreno_rb_blend_factor). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class BlendFactor : u32 {
  kZero = 0,                   /**< 0. */
  kOne = 1,                    /**< 1. */
  kSrcColor = 4,               /**< Source color. */
  kOneMinusSrcColor = 5,       /**< 1 minus source color. */
  kSrcAlpha = 6,               /**< Source alpha. */
  kOneMinusSrcAlpha = 7,       /**< 1 minus source alpha. */
  kDstColor = 8,               /**< Destination color. */
  kOneMinusDstColor = 9,       /**< 1 minus destination color. */
  kDstAlpha = 10,              /**< Destination alpha. */
  kOneMinusDstAlpha = 11,      /**< 1 minus destination alpha. */
  kConstantColor = 12,         /**< Constant color. */
  kOneMinusConstantColor = 13, /**< 1 minus constant color. */
  kConstantAlpha = 14,         /**< Constant alpha. */
  kOneMinusConstantAlpha = 15, /**< 1 minus constant alpha. */
  kSrcAlphaSaturate = 16,      /**< Saturated source alpha. */
};

/** Blend operation. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class BlendOp : u32 {
  kAdd = 0,         /**< Source plus destination. */
  kSubtract = 1,    /**< Source minus destination. */
  kMin = 2,         /**< Minimum. */
  kMax = 3,         /**< Maximum. */
  kRevSubtract = 4, /**< Destination minus source. */
};

/** State invalidation mask. */
enum class InvalidateMask : u16 {
  kVertexShader = 1 << 8, /**< Vertex shader state. */
  kPixelShader = 1 << 9,  /**< Pixel shader state. */
  kAll = 0x7FFF,          /**< All state. */
};

/** Index source of a draw (VGT_DRAW_INITIATOR::DI_SRC_SEL). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SourceSelect : u32 {
  kDMA,       /**< Indices fetched from memory. */
  kImmediate, /**< Indices embedded in the packet. */
  kAutoIndex, /**< Indices generated automatically. */
};

/** Major mode of a draw (VGT_DRAW_INITIATOR::DI_MAJOR_MODE). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class MajorMode : u32 {
  kImplicit, /**< Implicit major mode. */
  kExplicit, /**< Explicit major mode. */
};

/**
 * Reports whether a draw uses explicit major mode.
 *
 * @param major_mode Major mode from the draw initiator.
 * @param primitive_type Primitive type of the draw.
 * @return True when the mode is explicit or the primitive type forces it.
 */
inline bool IsMajorModeExplicit(MajorMode major_mode, PrimitiveType primitive_type) {
  return major_mode != MajorMode::kImplicit ||
         primitive_type >= PrimitiveType::kExplicitMajorModeForceStart;
}

/** Signed normalized value mapping. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SignedRepeatingFractionMode : u32 {
  kZeroClampMinusOne, /**< 2 representations of -1, 1 of them clamped. */
  kNoZero,            /**< Alternate mapping with no representation for 0. */
};

/** Arbitrary texture filter kernel. Deprecated in the R400 documentation. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ArbitraryFilter : u32 {
  k2x4Sym = 0,        /**< 2x4 symmetric. */
  k2x4Asym = 1,       /**< 2x4 asymmetric. */
  k4x2Sym = 2,        /**< 4x2 symmetric. */
  k4x2Asym = 3,       /**< 4x2 asymmetric. */
  k4x4Sym = 4,        /**< 4x4 symmetric. */
  k4x4Asym = 5,       /**< 4x4 asymmetric. */
  kUseFetchConst = 7, /**< Use the kernel from the fetch constant. */
};

/** Log2 of the shader temporary register count. */
constexpr u32 kMaxShaderTempRegistersLog2 = 6;
/** Number of shader temporary registers. */
constexpr u32 kMaxShaderTempRegisters = UINT32_C(1) << kMaxShaderTempRegistersLog2;

/** Vertex shader export mode (a2xx_sq_ps_vtx_mode). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexShaderExportMode : u32 {
  kPosition1Vector = 0,            /**< 1 position vector. */
  kPosition2VectorsSprite = 2,     /**< 2 position vectors, sprite. */
  kPosition2VectorsEdge = 3,       /**< 2 position vectors, edge flags. */
  kPosition2VectorsKill = 4,       /**< 2 position vectors, kill flag. */
  kPosition2VectorsSpriteKill = 5, /**< 2 position vectors, sprite and kill flag. */
  kPosition2VectorsEdgeKill = 6,   /**< 2 position vectors, edge and kill flags. */
  kMultipass = 7,                  /**< Outputs ignored and all primitives killed. */
};

/** Number of shader interpolators. */
constexpr u32 kMaxInterpolators = 16;

/** Interpolation sample location control (SQ_CONTEXT_MISC::sc_sample_cntl). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SampleControl : u32 {
  kCentroidsOnly = 0,       /**< All interpolators at centroids. */
  kCentersOnly = 1,         /**< All interpolators at centers. */
  kCentroidsAndCenters = 2, /**< Per interpolator, from the sampling pattern. */
};

/**
 * Returns the per-interpolator SampleLocation bits.
 *
 * @param msaa_samples RB_SURFACE_INFO::msaa_samples.
 * @param sample_control SQ_CONTEXT_MISC::sc_sample_cntl.
 * @param interpolator_control_sampling_pattern SQ_INTERPOLATOR_CNTL::sampling_pattern.
 * @return 1 bit per interpolator, set for kCenter.
 */
inline u32 GetInterpolatorSamplingPattern(MSAASamples msaa_samples, SampleControl sample_control,
                                          u32 interpolator_control_sampling_pattern) {
  if (msaa_samples == MSAASamples::k1X || sample_control == SampleControl::kCentersOnly) {
    return ((1 << kMaxInterpolators) - 1) * std::to_underlying(SampleLocation::kCenter);
  }
  if (sample_control == SampleControl::kCentroidsOnly) {
    return ((1 << kMaxInterpolators) - 1) * std::to_underlying(SampleLocation::kCentroid);
  }
  REX_ASSERT(sample_control == SampleControl::kCentroidsAndCenters);
  return interpolator_control_sampling_pattern;
}

/** VGT output path (VGT_OUTPUT_PATH_CNTL::path_select). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VGTOutputPath : u32 {
  kVertexReuse = 0,        /**< Vertex reuse. */
  kTessellationEnable = 1, /**< Tessellation. */
  kPassthru = 2,           /**< Passthrough. */
};

/** Tessellation mode. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TessellationMode : u32 {
  kDiscrete = 0,   /**< Discrete tessellation. */
  kContinuous = 1, /**< Continuous tessellation. */
  kAdaptive = 2,   /**< Adaptive tessellation with per-edge factors. */
};

/**
 * Polygon mode enable.
 *
 * Any value other than kDualMode renders filled triangles, so only kDualMode applies the
 * front and back polygon types.
 */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PolygonModeEnable : u32 {
  kDisabled = 0, /**< Render triangles. */
  kDualMode = 1, /**< Use the front and back polygon types. */
};

/** Polygon fill type. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PolygonType : u32 {
  kPoints = 0,    /**< Points. */
  kLines = 1,     /**< Lines. */
  kTriangles = 2, /**< Filled triangles. */
};

/** Pixel center convention (PA_SU_VTX_CNTL::pix_center). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PixelCenter : u32 {
  kD3DZero = 0, /**< Center at .0, as in Direct3D 9. */
  kOGLHalf = 1, /**< Center at .5, as in OpenGL. */
};

/** Vertex position rounding. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexRounding : u32 {
  kTruncate = 0,    /**< Truncate, as in OpenGL. */
  kRound = 1,       /**< Round. */
  kRoundToEven = 2, /**< Round to even, as in Direct3D. */
  kRoundToOdd = 3,  /**< Round to odd. */
};

/** Vertex position quantization. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexQuantization : u32 {
  k_1_16th = 0, /**< 1/16 pixel. */
  k_1_8th = 1,  /**< 1/8 pixel. */
  k_1_4th = 2,  /**< 1/4 pixel. */
  k_1_2 = 3,    /**< 1/2 pixel. */
  k_1 = 4,      /**< 1 pixel. */
};

/** EDRAM operation mode of a draw. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class EDRAMMode : u32 {
  kNoOperation = 0, /**< No EDRAM access. */
  kColorDepth = 4,  /**< Color and depth output. */
  kDepthOnly = 5,   /**< Depth output only, the pixel shader is treated as unused. */
  kCopy = 6,        /**< Resolve from EDRAM to memory. */
};

/**
 * Log2 of the resolve offset and size granularity in pixels.
 *
 * Resolves copy EDRAM contents to tiled textures by drawing with EDRAMMode::kCopy, and the
 * window scissor applies to the copied region.
 */
constexpr u32 kResolveAlignmentPixelsLog2 = 3;
/** Resolve offset and size granularity in pixels. */
constexpr u32 kResolveAlignmentPixels = 1 << kResolveAlignmentPixelsLog2;

/** Width of RB_SURFACE_INFO::surface_pitch and the RB_COPY_DEST_PITCH fields. */
constexpr u32 kResolveSizeBits = 14;
/** Largest resolve width or height in pixels. */
constexpr u32 kMaxResolveSize = (1 << kResolveSizeBits) - kResolveAlignmentPixels;

/** Resolve copy command. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class CopyCommand : u32 {
  kRaw = 0,         /**< Raw copy. */
  kConvert = 1,     /**< Copy with format conversion. */
  kConstantOne = 2, /**< Write constant 1. */
  kNull = 3,        /**< Null copy. Resolve rejects it as unsupported. */
};

/** Samples a resolve reads (a2xx_rb_copy_sample_select). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class CopySampleSelect : u32 {
  k0,    /**< Sample 0. */
  k1,    /**< Sample 1. */
  k2,    /**< Sample 2. */
  k3,    /**< Sample 3. */
  k01,   /**< Average of samples 0 and 1. */
  k23,   /**< Average of samples 2 and 3. */
  k0123, /**< Average of samples 0 to 3. */
};

/**
 * Reports whether a resolve reads a single sample.
 *
 * @param copy_sample_select Sample selection.
 * @return True for k0 to k3.
 */
constexpr bool IsSingleCopySampleSelected(CopySampleSelect copy_sample_select) {
  return copy_sample_select >= CopySampleSelect::k0 && copy_sample_select <= CopySampleSelect::k3;
}

/**
 * Packs 4 TextureSwizzle component selectors into a 12-bit swizzle.
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

/** Texture swizzle component selectors and common packed swizzles. */
enum TextureSwizzle : u16 {
  REXGPU_SWIZZLE_X = 0,                             /**< Component X. */
  REXGPU_SWIZZLE_R = 0,                             /**< Component R. */
  REXGPU_SWIZZLE_Y = 1,                             /**< Component Y. */
  REXGPU_SWIZZLE_G = 1,                             /**< Component G. */
  REXGPU_SWIZZLE_Z = 2,                             /**< Component Z. */
  REXGPU_SWIZZLE_B = 2,                             /**< Component B. */
  REXGPU_SWIZZLE_W = 3,                             /**< Component W. */
  REXGPU_SWIZZLE_A = 3,                             /**< Component A. */
  REXGPU_SWIZZLE_0 = 4,                             /**< Constant 0. */
  REXGPU_SWIZZLE_1 = 5,                             /**< Constant 1. */
  REXGPU_SWIZZLE_RRRR = REXGPU_SWIZZLE(R, R, R, R), /**< RRRR. */
  REXGPU_SWIZZLE_RGGG = REXGPU_SWIZZLE(R, G, G, G), /**< RGGG. */
  REXGPU_SWIZZLE_RGBB = REXGPU_SWIZZLE(R, G, B, B), /**< RGBB. */
  REXGPU_SWIZZLE_RGBA = REXGPU_SWIZZLE(R, G, B, A), /**< RGBA. */
  REXGPU_SWIZZLE_0000 = REXGPU_SWIZZLE(0, 0, 0, 0), /**< All 0. */
};

/**
 * Applies a GPU endian swap to a 16-bit value.
 *
 * @param value Value to swap.
 * @param endianness Swap mode, kNone or k8in16.
 * @return Swapped value.
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

/**
 * Applies a GPU endian swap to a 32-bit value.
 *
 * @param value Value to swap.
 * @param endianness Swap mode.
 * @return Swapped value.
 */
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

/**
 * Applies a GPU endian swap to the bits of a 32-bit floating-point value.
 *
 * @param value Value to swap.
 * @param endianness Swap mode.
 * @return Swapped value.
 */
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

/** Loop constant from the shader loop constant register range. */
union alignas(u32) LoopConstant {
  u32 value; /**< Raw register value. */
  struct {
    u32 count : 8;  /**< Iteration count. */
    u32 start : 8;  /**< AL start. aL is iterator * step + start, in [-256, 256]. */
    i32 step : 8;   /**< AL step. */
    u32 pad_24 : 8; /**< Padding. */
  };
};
REX_STATIC_ASSERT_SIZE(LoopConstant, sizeof(u32));

/** Fetch constant type (SQ_TEX_VTX_INVALID/VALID_TEXTURE/BUFFER). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class FetchConstantType : u32 {
  kInvalidTexture, /**< Invalid texture. */
  kInvalidVertex,  /**< Invalid vertex buffer. */
  kTexture,        /**< Texture. */
  kVertex,         /**< Vertex buffer. */
};

/** Number of texture fetch constants. */
constexpr u32 kTextureFetchConstantCount = 32;
/** Number of vertex fetch constants, 3 per texture fetch constant slot. */
constexpr u32 kVertexFetchConstantCount = 3 * kTextureFetchConstantCount;

/** Vertex fetch constant from the shader fetch constant register range. */
union alignas(u32) VertexFetch {
  struct {
    u32 dword_0; /**< Raw dword 0. */
    u32 dword_1; /**< Raw dword 1. */
  };
  struct {
    FetchConstantType type : 2; /**< Constant type. */
    u32 address : 30;           /**< Address in dwords. */

    Endian endian : 2; /**< Endian swap. */
    u32 size : 24;     /**< Size in words. */
    u32 pad_26 : 6;    /**< Padding. */
  };
};
REX_STATIC_ASSERT_SIZE(VertexFetch, sizeof(u32) * 2);

/** Log2 of the byte alignment of texture subresources, also omitted from base and mip addresses. */
constexpr u32 kTextureSubresourceAlignmentBytesLog2 = 12;
/** Byte alignment of texture subresources. */
constexpr u32 kTextureSubresourceAlignmentBytes = 1 << kTextureSubresourceAlignmentBytesLog2;

/** Log2 of the largest 1D texture width. */
constexpr u32 kTexture1DMaxWidthLog2 = 24;
/** Largest 1D texture width. */
constexpr u32 kTexture1DMaxWidth = 1 << kTexture1DMaxWidthLog2;
/** Log2 of the largest 2D or cube texture width and height. */
constexpr u32 kTexture2DCubeMaxWidthHeightLog2 = 13;
/** Largest 2D or cube texture width and height. */
constexpr u32 kTexture2DCubeMaxWidthHeight = 1 << kTexture2DCubeMaxWidthHeightLog2;
/** Log2 of the largest stacked texture depth. */
constexpr u32 kTexture2DMaxStackDepthLog2 = 6;
/** Largest stacked texture depth. */
constexpr u32 kTexture2DMaxStackDepth = 1 << kTexture2DMaxStackDepthLog2;
/** Log2 of the largest 3D texture width and height. */
constexpr u32 kTexture3DMaxWidthHeightLog2 = 11;
/** Largest 3D texture width and height. */
constexpr u32 kTexture3DMaxWidthHeight = 1 << kTexture3DMaxWidthHeightLog2;
/** Log2 of the largest 3D texture depth. */
constexpr u32 kTexture3DMaxDepthLog2 = 10;
/** Largest 3D texture depth. */
constexpr u32 kTexture3DMaxDepth = 1 << kTexture3DMaxDepthLog2;

/** Largest mip level count. */
constexpr u32 kTextureMaxMips =
    std::max(kTexture2DCubeMaxWidthHeightLog2, kTexture3DMaxWidthHeightLog2) + 1;

/** Log2 of the texture tile width and height. */
constexpr u32 kTextureTileWidthHeightLog2 = 5;
/** Texture tile width and height. */
constexpr u32 kTextureTileWidthHeight = 1 << kTextureTileWidthHeightLog2;
/** Log2 of the 3D texture tile depth. Depths 0 to 3 and 4 to 7 use separate address ranges. */
constexpr u32 kTextureTileDepthLog2 = 2;
/** 3D texture tile depth. */
constexpr u32 kTextureTileDepth = 1 << kTextureTileDepthLog2;

/**
 * Returns log2 of the X period of the tiled texture address function.
 *
 * Periods are 128x128 for 2D 1bpb, 64x64 for 2D 2bpb, 32x32 for 2D 4bpb and above,
 * 64x32x8 for 3D 1bpb and 32x32x8 for 3D 2bpb and above.
 *
 * @param is_3d Whether the texture is 3D.
 * @param bytes_per_block_log2 Log2 of the bytes per block.
 * @return Log2 of the X period in blocks.
 */
constexpr u32 GetTextureTiledXBaseGranularityLog2(bool is_3d, u32 bytes_per_block_log2) {
  return 7 - std::min(UINT32_C(2), bytes_per_block_log2 + static_cast<u32>(is_3d));
}

/**
 * Returns log2 of the Y period of the tiled texture address function.
 *
 * @param is_3d Whether the texture is 3D.
 * @param bytes_per_block_log2 Log2 of the bytes per block.
 * @return Log2 of the Y period in blocks.
 */
constexpr u32 GetTextureTiledYBaseGranularityLog2(bool is_3d, u32 bytes_per_block_log2) {
  return is_3d ? 5 : (7 - std::min(UINT32_C(2), bytes_per_block_log2));
}

/** Log2 of the Z period of the 3D tiled texture address function. */
constexpr u32 kTextureTiledZBaseGranularityLog2 = 3;
/** Z period of the 3D tiled texture address function. */
constexpr u32 kTextureTiledZBaseGranularity = 1 << kTextureTiledZBaseGranularityLog2;

/** Log2 of the row pitch alignment of linear textures in bytes. */
constexpr u32 kTextureLinearRowAlignmentBytesLog2 = 8;
/** Row pitch alignment of linear textures in bytes. */
constexpr u32 kTextureLinearRowAlignmentBytes = 1 << kTextureLinearRowAlignmentBytesLog2;

/**
 * Texture fetch constant from the shader fetch constant register range.
 *
 * Size fields store each dimension minus 1.
 */
union alignas(u32) TextureFetch {
  struct {
    u32 dword_0; /**< Raw dword 0. */
    u32 dword_1; /**< Raw dword 1. */
    u32 dword_2; /**< Raw dword 2. */
    u32 dword_3; /**< Raw dword 3. */
    u32 dword_4; /**< Raw dword 4. */
    u32 dword_5; /**< Raw dword 5. */
  };
  struct {
    FetchConstantType type : 2; /**< Constant type. */
    TextureSign sign_x : 2;     /**< X signedness before the swizzle. */
    TextureSign sign_y : 2;     /**< Y signedness. */
    TextureSign sign_z : 2;     /**< Z signedness. */
    TextureSign sign_w : 2;     /**< W signedness. */
    ClampMode clamp_x : 3;      /**< X clamp mode. */
    ClampMode clamp_y : 3;      /**< Y clamp mode. */
    ClampMode clamp_z : 3;      /**< Z clamp mode. */
    u32 pad_19 : 3;             /**< Padding. */
    u32 pitch : 9;              /**< Base row pitch in pixels >> 5. */
    u32 tiled : 1;              /**< Tiled layout. */

    TextureFormat format : 6;     /**< Texture format. */
    Endian endianness : 2;        /**< Endian swap. */
    u32 request_size : 2;         /**< Request size. */
    u32 stacked : 1;              /**< Stacked 2D texture. */
    u32 nearest_clamp_policy : 1; /**< Direct3D or OpenGL nearest clamp. */
    u32 base_address : 20;        /**< Base address >> 12. */

    union {
      struct {
        u32 width : 24; /**< Width minus 1. */
        u32 pad_24 : 8; /**< Padding. */
      } size_1d;        /**< Dword 2, 1D size. */
      struct {
        u32 width : 13;      /**< Width minus 1. */
        u32 height : 13;     /**< Height minus 1. */
        u32 stack_depth : 6; /**< Stack depth minus 1, ignored unless stacked, 5 for cube maps. */
      } size_2d;             /**< Dword 2, 2D, stacked or cube size. */
      struct {
        u32 width : 11;  /**< Width minus 1. */
        u32 height : 11; /**< Height minus 1. */
        u32 depth : 10;  /**< Depth minus 1. */
      } size_3d;         /**< Dword 2, 3D size. */
    };

    u32 num_format : 1;                   /**< Fraction or integer. */
    u32 swizzle : 12;                     /**< XYZW TextureSwizzle, 3 bits each. */
    i32 exp_adjust : 6;                   /**< Exponent adjustment. */
    TextureFilter mag_filter : 2;         /**< Magnification filter. */
    TextureFilter min_filter : 2;         /**< Minification filter. */
    TextureFilter mip_filter : 2;         /**< Mip filter. */
    AnisoFilter aniso_filter : 3;         /**< Anisotropic filter. */
    ArbitraryFilter arbitrary_filter : 3; /**< Arbitrary filter. */
    u32 border_size : 1;                  /**< Border size. */

    u32 vol_mag_filter : 1;    /**< Volume magnification filter. */
    u32 vol_min_filter : 1;    /**< Volume minification filter. */
    u32 mip_min_level : 4;     /**< Lowest mip level. */
    u32 mip_max_level : 4;     /**< Highest mip level. */
    u32 mag_aniso_walk : 1;    /**< Magnification anisotropic walk. */
    u32 min_aniso_walk : 1;    /**< Minification anisotropic walk. */
    i32 lod_bias : 10;         /**< LOD bias with 5 fractional bits. */
    i32 grad_exp_adjust_h : 5; /**< Horizontal gradient exponent adjustment. */
    i32 grad_exp_adjust_v : 5; /**< Vertical gradient exponent adjustment. */

    BorderColor border_color : 2; /**< Border color. */
    u32 force_bc_w_to_max : 1;    /**< Force block compression W to max. */
    u32 tri_clamp : 2;            /**< Trilinear clamp. */
    i32 aniso_bias : 4;           /**< Anisotropic bias. */
    DataDimension dimension : 2;  /**< Dimension. */
    u32 packed_mips : 1;          /**< Packed mips. */
    u32 mip_address : 20;         /**< Mip address >> 12. */
  };
};
REX_STATIC_ASSERT_SIZE(TextureFetch, sizeof(u32) * 6);

/** Group of 6 fetch constant dwords, as 1 texture fetch or 3 vertex fetches. */
union alignas(u32) FetchGroup {
  struct {
    u32 dword_0; /**< Raw dword 0. */
    u32 dword_1; /**< Raw dword 1. */
    u32 dword_2; /**< Raw dword 2. */
    u32 dword_3; /**< Raw dword 3. */
    u32 dword_4; /**< Raw dword 4. */
    u32 dword_5; /**< Raw dword 5. */
  };
  TextureFetch texture_fetch; /**< Texture fetch view. */
  struct {
    VertexFetch vertex_fetch_0; /**< Vertex fetch 0. */
    VertexFetch vertex_fetch_1; /**< Vertex fetch 1. */
    VertexFetch vertex_fetch_2; /**< Vertex fetch 2. */
  };
  struct {
    u32 type_0 : 2;    /**< Type of vertex fetch 0. */
    u32 data_0_a : 30; /**< Remaining bits of dword 0. */
    u32 data_0_b : 32; /**< Dword 1. */
    u32 type_1 : 2;    /**< Type of vertex fetch 1. */
    u32 data_1_a : 30; /**< Remaining bits of dword 2. */
    u32 data_1_b : 32; /**< Dword 3. */
    u32 type_2 : 2;    /**< Type of vertex fetch 2. */
    u32 data_2_a : 30; /**< Remaining bits of dword 4. */
    u32 data_2_b : 32; /**< Dword 5. */
  };
};
REX_STATIC_ASSERT_SIZE(FetchGroup, sizeof(u32) * 6);

/**
 * Memory export stream constant.
 *
 * A shader writes this constant plus an element offset to eA, usually with
 * `mad eA, r#, const0100, c#`, then writes up to 5 elements to eM0 to eM4. Each field is
 * packed as a normalized floating-point number so the Xenos ALU does not flush it.
 * - X holds the base address in dwords with 0b01 in the top 2 bits.
 * - Y holds the element offset added to 2^23.
 * - Z holds the format fields with an exponent of 23.
 * - W holds the element count with an exponent of 23.
 *
 * Exports are dropped if the index is above the maximum, the index sign bit is set, or the
 * index exponent is not 23. Alloc instructions and `serialize` end the current export.
 */
union alignas(u32) MemExportStream {
  struct {
    u32 dword_0; /**< Raw dword 0. */
    u32 dword_1; /**< Raw dword 1. */
    u32 dword_2; /**< Raw dword 2. */
    u32 dword_3; /**< Raw dword 3. */
  };
  struct {
    u32 base_address : 30; /**< Physical address >> 2. */
    u32 const_0x1 : 2;     /**< 0b01. */

    u32 const_0x4b000000; /**< Dword 1, 2^23 with the offset added by the shader. */

    Endian128 endianness : 3;           /**< Endian swap. */
    u32 unused_0 : 5;                   /**< Unused. */
    ColorFormat format : 6;             /**< Element format. */
    u32 unused_1 : 2;                   /**< Unused. */
    SurfaceNumberFormat num_format : 3; /**< Number format. */
    u32 red_blue_swap : 1;              /**< Swap red and blue. */
    u32 const_0x4b0 : 12;               /**< Exponent 23. */

    u32 index_count : 23; /**< Element count. */
    u32 const_0x96 : 9;   /**< Exponent 23. */
  };
};
REX_STATIC_ASSERT_SIZE(MemExportStream, sizeof(u32) * 4);

/**
 * Depth sample counters written by the GPU.
 *
 * Values are little endian. Direct3D sums the A and B halves and computes occlusion by
 * subtracting the begin counters from the end counters.
 */
struct alignas(u32) DepthSampleCounts {
  LE<u32> Total_A;       /**< Total samples, half A. */
  LE<u32> Total_B;       /**< Total samples, half B. */
  LE<u32> ZFail_A;       /**< Depth test failures, half A. */
  LE<u32> ZFail_B;       /**< Depth test failures, half B. */
  LE<u32> ZPass_A;       /**< Depth test passes, half A. */
  LE<u32> ZPass_B;       /**< Depth test passes, half B. */
  LE<u32> StencilFail_A; /**< Stencil test failures, half A. */
  LE<u32> StencilFail_B; /**< Stencil test failures, half B. */
};
REX_STATIC_ASSERT_SIZE(DepthSampleCounts, sizeof(u32) * 8);

/** Event values for VGT_EVENT_INITIATOR. */
enum Event : u8 {
  VS_DEALLOC = 0,                    /**< Vertex shader deallocation. */
  PS_DEALLOC = 1,                    /**< Pixel shader deallocation. */
  VS_DONE_TS = 2,                    /**< Vertex shader done timestamp. */
  PS_DONE_TS = 3,                    /**< Pixel shader done timestamp. */
  CACHE_FLUSH_TS = 4,                /**< Cache flush timestamp. */
  CONTEXT_DONE = 5,                  /**< Context done. */
  CACHE_FLUSH = 6,                   /**< Cache flush. */
  VIZQUERY_START = 7,                /**< Visibility query start. */
  VIZQUERY_END = 8,                  /**< Visibility query end. */
  SC_WAIT_WC = 9,                    /**< Scan converter wait. */
  MPASS_PS_CP_REFETCH = 10,          /**< Multipass pixel shader refetch. */
  MPASS_PS_RST_START = 11,           /**< Multipass pixel shader reset start. */
  MPASS_PS_INCR_START = 12,          /**< Multipass pixel shader increment start. */
  RST_PIX_CNT = 13,                  /**< Reset pixel count. */
  RST_VTX_CNT = 14,                  /**< Reset vertex count. */
  TILE_FLUSH = 15,                   /**< Tile flush. */
  CACHE_FLUSH_AND_INV_TS_EVENT = 20, /**< Cache flush and invalidate timestamp. */
  ZPASS_DONE = 21,                   /**< Z pass count done. */
  CACHE_FLUSH_AND_INV_EVENT = 22,    /**< Cache flush and invalidate. */
  PERFCOUNTER_START = 23,            /**< Performance counter start. */
  PERFCOUNTER_STOP = 24,             /**< Performance counter stop. */
  SCREEN_EXT_INIT = 25,              /**< Screen extent init. */
  SCREEN_EXT_RPT = 26,               /**< Screen extent report. */
  VS_FETCH_DONE_TS = 27,             /**< Vertex shader fetch done timestamp. */
};

// clang-format off
/** Type 3 packet opcodes (IT_OPCODE) in the ring buffer. */
enum Type3Opcode : u8 {
  PM4_ME_INIT               = 0x48, /**< Initialize the CP micro engine. */
  PM4_NOP                   = 0x10, /**< Skip N 32-bit words. */
  PM4_INDIRECT_BUFFER       = 0x3f, /**< Indirect buffer dispatch, prefetched. */
  PM4_INDIRECT_BUFFER_PFD   = 0x37, /**< Indirect buffer dispatch, pipelined init. */
  PM4_WAIT_FOR_IDLE         = 0x26, /**< Wait for the engine to go idle. */
  PM4_WAIT_REG_MEM          = 0x3c, /**< Wait for a register or memory value. */
  PM4_WAIT_REG_EQ           = 0x52, /**< Wait for a register to equal a value. */
  PM4_WAIT_REG_GTE          = 0x53, /**< Wait for a register to be >= a value. */
  PM4_WAIT_UNTIL_READ       = 0x5c, /**< Wait for a read to complete. */
  PM4_WAIT_IB_PFD_COMPLETE  = 0x5d, /**< Wait for IB_PFD base and size writes. */
  PM4_REG_RMW               = 0x21, /**< Register read, modify and write. */
  PM4_REG_TO_MEM            = 0x3e, /**< Copy a register to memory. */
  PM4_MEM_WRITE             = 0x3d, /**< Write N 32-bit words to memory. */
  PM4_MEM_WRITE_CNTR        = 0x4f, /**< Write CP_PROG_COUNTER to memory. */
  PM4_COND_EXEC             = 0x44, /**< Conditionally execute packets. */
  PM4_COND_WRITE            = 0x45, /**< Conditionally write memory or a register. */
  PM4_EVENT_WRITE           = 0x46, /**< Event that writes memory on completion. */
  PM4_EVENT_WRITE_SHD       = 0x58, /**< Vertex or pixel shader done event. */
  PM4_EVENT_WRITE_CFL       = 0x59, /**< Cache flush done event. */
  PM4_EVENT_WRITE_EXT       = 0x5a, /**< Screen extent event. */
  PM4_EVENT_WRITE_ZPD       = 0x5b, /**< Z pass done event. */
  PM4_DRAW_INDX             = 0x22, /**< Fetch the index buffer and draw. */
  PM4_DRAW_INDX_2           = 0x36, /**< Draw with indices in the packet. */
  PM4_DRAW_INDX_BIN         = 0x34, /**< Fetch the index buffer and bin IDs and draw. */
  PM4_DRAW_INDX_2_BIN       = 0x35, /**< Fetch bin IDs and draw with packet indices. */
  PM4_VIZ_QUERY             = 0x23, /**< Visibility query begin or end. */
  PM4_SET_STATE             = 0x25, /**< Fetch state and start shader code DMA. */
  PM4_SET_CONSTANT          = 0x2d, /**< Load a constant into the chip and memory. */
  PM4_SET_CONSTANT2         = 0x55, /**< Incremental state update. */
  PM4_SET_SHADER_CONSTANTS  = 0x56, /**< Incremental constant update. */
  PM4_LOAD_ALU_CONSTANT     = 0x2f, /**< Load constants from memory. */
  PM4_IM_LOAD               = 0x27, /**< Load instruction memory from a pointer. */
  PM4_IM_LOAD_IMMEDIATE     = 0x2b, /**< Load instruction memory from the packet. */
  PM4_LOAD_CONSTANT_CONTEXT = 0x2e, /**< Load constants from a memory location. */
  PM4_INVALIDATE_STATE      = 0x3b, /**< Invalidate selected state pointers. */
  PM4_SET_SHADER_BASES      = 0x4A, /**< Change the instruction memory partition. */
  PM4_SET_BIN_BASE_OFFSET   = 0x4B, /**< Offset added to BIN_BASE of binned draws. */
  PM4_SET_BIN_MASK          = 0x50, /**< Set the 64-bit BIN_MASK. */
  PM4_SET_BIN_SELECT        = 0x51, /**< Set the 64-bit BIN_SELECT. */
  PM4_CONTEXT_UPDATE        = 0x5e, /**< Update the current context if needed. */
  PM4_INTERRUPT             = 0x54, /**< Raise an interrupt from the command stream. */
  PM4_XE_SWAP               = 0x64, /**< Runtime opcode that VdSwap writes to present. */
  PM4_IM_STORE              = 0x2c, /**< Copy instruction memory to system memory. */
  PM4_SET_BIN_MASK_LO       = 0x60, /**< Set the low BIN_MASK dword for tiled rendering. */
  PM4_SET_BIN_MASK_HI       = 0x61, /**< Set the high BIN_MASK dword. */
  PM4_SET_BIN_SELECT_LO     = 0x62, /**< Set the low BIN_SELECT dword. */
  PM4_SET_BIN_SELECT_HI     = 0x63, /**< Set the high BIN_SELECT dword. */
};
// clang-format on

/**
 * Builds a type 0 packet header that writes consecutive registers.
 *
 * Layout ttcccccc cccccccc oiiiiiii iiiiiiii with type, count minus 1, 1 register flag and
 * register index.
 *
 * @param index First register index, up to 0x7FFF.
 * @param count Register count, 1 to 0x4000.
 * @param one_reg Writes every value to index.
 * @return Packet header.
 */
inline u32 BuildPacketType0(u16 index, u16 count, bool one_reg = false) {
  assert(index <= 0x7FFF);
  assert(count >= 1 && count <= 0x4000);
  return (0u << 30) | (((count - 1) & 0x3FFF) << 16) | (one_reg ? 1u << 15 : 0u) | (index & 0x7FFF);
}

/**
 * Builds a type 1 packet header that writes 2 registers.
 *
 * Layout tt?????? ??222222 22222111 11111111.
 *
 * @param index_1 First register index, up to 0x7FF.
 * @param index_2 Second register index, up to 0x7FF.
 * @return Packet header.
 */
inline u32 BuildPacketType1(u16 index_1, u16 index_2) {
  assert(index_1 <= 0x7FF);
  assert(index_2 <= 0x7FF);
  return (1u << 30) | ((index_2 & 0x7FF) << 11) | (index_1 & 0x7FF);
}

/**
 * Builds a type 2 packet, a single dword no-op.
 *
 * @return Packet header.
 */
constexpr u32 BuildPacketType2() {
  return (2u << 30);
}

/**
 * Builds a type 3 packet header.
 *
 * Layout ttcccccc cccccccc ?ooooooo ???????p with type, count minus 1, opcode and predicate.
 *
 * @param opcode Packet opcode.
 * @param count Payload dword count, 1 to 0x4000.
 * @param predicate Whether the packet is predicated.
 * @return Packet header.
 */
inline u32 BuildPacketType3(Type3Opcode opcode, u16 count, bool predicate = false) {
  assert(opcode <= 0x7F);
  assert(count >= 1 && count <= 0x4000);
  return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8) | (predicate ? 1 : 0);
}

/**
 * Converts a 32-bit value to the Xenos 16-bit floating-point encoding.
 *
 * The encoding uses the extended range instead of infinity and NaN, and saturates.
 *
 * @param value Value to convert.
 * @param preserve_denormal Keep denormalized results instead of flushing them to 0.
 * @param round_to_nearest_even Round to the nearest even value instead of towards 0.
 * @return 16-bit encoded value.
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
 * Converts a Xenos 16-bit floating-point value to 32 bits.
 *
 * @param value 16-bit encoded value.
 * @param preserve_denormal Keep denormalized inputs instead of flushing them to 0.
 * @return Converted value.
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
