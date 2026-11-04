/**
 * @file        xenos/state.h
 * @brief       Xenos pipeline state, shader and EDRAM enums
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

#include <utility>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex::xenos {

// NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
enum class ShaderType : u32 {
  kVertex = 0,
  kPixel = 1,
};

/** Number of low vertex index bits the GPU uses. */
constexpr u32 kVertexIndexBits = 24;
constexpr u32 kVertexIndexMask = (static_cast<u32>(1) << kVertexIndexBits) - 1;

/** Primitive type of a draw. Types from kExplicitMajorModeForceStart force explicit major mode. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PrimitiveType : u32 {
  kNone = 0x00,
  kPointList = 0x01,
  kLineList = 0x02,
  kLineStrip = 0x03,
  kTriangleList = 0x04,
  kTriangleFan = 0x05,
  kTriangleStrip = 0x06,
  kTriangleWithWFlags = 0x07,
  kRectangleList = 0x08, /**< Three vertices per rectangle. */
  kLineLoop = 0x0C,
  kQuadList = 0x0D,
  kQuadStrip = 0x0E,
  kPolygon = 0x0F,
  kExplicitMajorModeForceStart = 0x10,
  k2DCopyRectListV0 = 0x10,
  k2DCopyRectListV1 = 0x11,
  k2DCopyRectListV2 = 0x12,
  k2DCopyRectListV3 = 0x13,
  k2DFillRectList = 0x14,
  k2DLineStrip = 0x15,
  k2DTriStrip = 0x16,
  kLinePatch = 0x10,     /**< Used with tessellation enabled. */
  kTrianglePatch = 0x11, /**< Used with tessellation enabled. */
  kQuadPatch = 0x12,     /**< Used with tessellation enabled. */
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SampleLocation : u32 {
  kCentroid = 0,
  kCenter = 1,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class MSAASamples : u32 {
  k1X = 0,
  k2X = 1, /**< 1x2 samples per pixel. */
  k4X = 2, /**< 2x2 samples per pixel. */
};

constexpr u32 kMSAASamplesBits = 2;

constexpr u32 kColorRenderTargetIndexBits = 2;
constexpr u32 kMaxColorRenderTargets = 4;

/** Scale from PA_SU_POLY_OFFSET_FRONT/BACK_SCALE subpixel units to pixel units. */
constexpr f32 kPolygonOffsetScaleSubpixelUnit = 1.0f / 16.0f;

/** EDRAM tile width in 32bpp samples. */
constexpr u32 kEDRAMTileWidthSamples = 80;
/** EDRAM tile height in 32bpp samples. */
constexpr u32 kEDRAMTileHeightSamples = 16;
/** Tile addressing wraps around at this count. */
constexpr u32 kEDRAMTileCount = 2048;
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
 * @param msaa_samples Sample count of the surface.
 * @param is_64bpp True for 64 bits per sample.
 */
constexpr u32 GetSurfacePitchTiles(u32 pitch_pixels, MSAASamples msaa_samples, bool is_64bpp) {
  u32 pitch_samples = pitch_pixels << static_cast<u32>(msaa_samples >= MSAASamples::k4X);
  u32 pitch_tiles = (pitch_samples + (kEDRAMTileWidthSamples - 1)) / kEDRAMTileWidthSamples;
  if (is_64bpp) {
    pitch_tiles <<= 1;
  }
  return pitch_tiles;
}

/** Bits needed for the largest GetSurfacePitchTiles result with a 16383 pixel pitch. */
constexpr u32 kEDRAMPitchTilesBits = 10;

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class CompareFunction : u32 {
  kNever = 0b000,
  kLess = 0b001,
  kEqual = 0b010,
  kLessEqual = 0b011,
  kGreater = 0b100,
  kNotEqual = 0b101,
  kGreaterEqual = 0b110,
  kAlways = 0b111,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class StencilOp : u32 {
  kKeep = 0,
  kZero = 1,
  kReplace = 2,
  kIncrementClamp = 3,
  kDecrementClamp = 4,
  kInvert = 5,
  kIncrementWrap = 6,
  kDecrementWrap = 7,
};

/** Blend factor (adreno_rb_blend_factor). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class BlendFactor : u32 {
  kZero = 0,
  kOne = 1,
  kSrcColor = 4,
  kOneMinusSrcColor = 5,
  kSrcAlpha = 6,
  kOneMinusSrcAlpha = 7,
  kDstColor = 8,
  kOneMinusDstColor = 9,
  kDstAlpha = 10,
  kOneMinusDstAlpha = 11,
  kConstantColor = 12,
  kOneMinusConstantColor = 13,
  kConstantAlpha = 14,
  kOneMinusConstantAlpha = 15,
  kSrcAlphaSaturate = 16,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class BlendOp : u32 {
  kAdd = 0,
  kSubtract = 1,
  kMin = 2,
  kMax = 3,
  kRevSubtract = 4, /**< Destination minus source. */
};

enum class InvalidateMask : u16 {
  kVertexShader = 1 << 8,
  kPixelShader = 1 << 9,
  kAll = 0x7FFF,
};

/** Index source of a draw (VGT_DRAW_INITIATOR::DI_SRC_SEL). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SourceSelect : u32 {
  kDMA, /**< Indices fetched from memory. */
  kImmediate,
  kAutoIndex, /**< Indices generated by the GPU. */
};

/** Major mode of a draw (VGT_DRAW_INITIATOR::DI_MAJOR_MODE). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class MajorMode : u32 {
  kImplicit,
  kExplicit,
};

/**
 * Reports whether a draw uses explicit major mode.
 *
 * @return True when the mode is explicit or the primitive type forces it.
 */
inline bool IsMajorModeExplicit(MajorMode major_mode, PrimitiveType primitive_type) {
  return major_mode != MajorMode::kImplicit ||
         primitive_type >= PrimitiveType::kExplicitMajorModeForceStart;
}

/** Signed normalized value mapping. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SignedRepeatingFractionMode : u32 {
  kZeroClampMinusOne, /**< Two representations of -1, one of them clamped. */
  kNoZero,            /**< Alternate mapping with no representation for 0. */
};

/** Arbitrary texture filter kernel. Deprecated in the R400 documentation. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ArbitraryFilter : u32 {
  k2x4Sym = 0,
  k2x4Asym = 1,
  k4x2Sym = 2,
  k4x2Asym = 3,
  k4x4Sym = 4,
  k4x4Asym = 5,
  kUseFetchConst = 7, /**< Use the kernel from the fetch constant. */
};

constexpr u32 kMaxShaderTempRegistersLog2 = 6;
constexpr u32 kMaxShaderTempRegisters = UINT32_C(1) << kMaxShaderTempRegistersLog2;

/** Vertex shader export mode (a2xx_sq_ps_vtx_mode). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexShaderExportMode : u32 {
  kPosition1Vector = 0,
  kPosition2VectorsSprite = 2,
  kPosition2VectorsEdge = 3,
  kPosition2VectorsKill = 4,
  kPosition2VectorsSpriteKill = 5,
  kPosition2VectorsEdgeKill = 6,
  kMultipass = 7, /**< Outputs ignored and all primitives killed. */
};

constexpr u32 kMaxInterpolators = 16;

/** Interpolation sample location control (SQ_CONTEXT_MISC::sc_sample_cntl). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class SampleControl : u32 {
  kCentroidsOnly = 0,
  kCentersOnly = 1,
  kCentroidsAndCenters = 2, /**< Per interpolator, from the sampling pattern. */
};

/**
 * Returns one SampleLocation bit per interpolator, set for kCenter.
 *
 * @param msaa_samples RB_SURFACE_INFO::msaa_samples.
 * @param sample_control SQ_CONTEXT_MISC::sc_sample_cntl.
 * @param interpolator_control_sampling_pattern SQ_INTERPOLATOR_CNTL::sampling_pattern.
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
  kVertexReuse = 0,
  kTessellationEnable = 1,
  kPassthru = 2,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class TessellationMode : u32 {
  kDiscrete = 0,
  kContinuous = 1,
  kAdaptive = 2,
};

/** Polygon mode enable. Only kDualMode applies the front and back polygon types. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PolygonModeEnable : u32 {
  kDisabled = 0, /**< Renders filled triangles. */
  kDualMode = 1,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PolygonType : u32 {
  kPoints = 0,
  kLines = 1,
  kTriangles = 2,
};

/** Pixel center convention (PA_SU_VTX_CNTL::pix_center). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class PixelCenter : u32 {
  kD3DZero = 0, /**< Center at .0, as in Direct3D 9. */
  kOGLHalf = 1, /**< Center at .5, as in OpenGL. */
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexRounding : u32 {
  kTruncate = 0, /**< Truncate, as in OpenGL. */
  kRound = 1,
  kRoundToEven = 2, /**< Round to even, as in Direct3D. */
  kRoundToOdd = 3,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class VertexQuantization : u32 {
  k_1_16th = 0,
  k_1_8th = 1,
  k_1_4th = 2,
  k_1_2 = 3,
  k_1 = 4,
};

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class EDRAMMode : u32 {
  kNoOperation = 0,
  kColorDepth = 4,
  kDepthOnly = 5, /**< The pixel shader is treated as unused. */
  kCopy = 6,
};

constexpr u32 kResolveAlignmentPixelsLog2 = 3;
constexpr u32 kResolveAlignmentPixels = 1 << kResolveAlignmentPixelsLog2;

/** Width of RB_SURFACE_INFO::surface_pitch and the RB_COPY_DEST_PITCH fields. */
constexpr u32 kResolveSizeBits = 14;
constexpr u32 kMaxResolveSize = (1 << kResolveSizeBits) - kResolveAlignmentPixels;

// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class CopyCommand : u32 {
  kRaw = 0,
  kConvert = 1,
  kConstantOne = 2,
  kNull = 3,
};

/** Samples a resolve reads (a2xx_rb_copy_sample_select). */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class CopySampleSelect : u32 {
  k0,
  k1,
  k2,
  k3,
  k01,   /**< Average of samples 0 and 1. */
  k23,   /**< Average of samples 2 and 3. */
  k0123, /**< Average of samples 0 to 3. */
};

/**
 * Reports whether a resolve reads a single sample.
 *
 * @return True when the selection is k0, k1, k2 or k3.
 */
constexpr bool IsSingleCopySampleSelected(CopySampleSelect copy_sample_select) {
  return copy_sample_select >= CopySampleSelect::k0 && copy_sample_select <= CopySampleSelect::k3;
}

/** Depth sample counters written by the GPU. Direct3D sums the A and B halves. */
struct alignas(u32) DepthSampleCounts {
  LE<u32> Total_A;
  LE<u32> Total_B;
  LE<u32> ZFail_A;
  LE<u32> ZFail_B;
  LE<u32> ZPass_A;
  LE<u32> ZPass_B;
  LE<u32> StencilFail_A;
  LE<u32> StencilFail_B;
};
REX_STATIC_ASSERT_SIZE(DepthSampleCounts, sizeof(u32) * 8);

}  // namespace rex::xenos
