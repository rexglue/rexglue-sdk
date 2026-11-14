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

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/assert.h>
#include "gpu/xenos/xenos_export.h"
#include "gpu/xenos/pipeline/shader/shader.h"
#include "gpu/xenos/xenos_settings.h"
#include <rex/xenos/register.h>
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>
#include <rex/memory.h>
#include <rex/guest.h>

namespace rex::gpu::draw_state {

/**
 * Reports whether the primitive type is a line.
 *
 * For patch types the major mode is always explicit, so the output path alone decides.
 */
constexpr bool IsPrimitiveLine(bool vgt_output_path_is_tessellation_enable,
                               xenos::PrimitiveType type) {
  if (vgt_output_path_is_tessellation_enable && type == xenos::PrimitiveType::kLinePatch) {
    return true;
  }
  switch (type) {
    case xenos::PrimitiveType::kLineList:
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kLineLoop:
    case xenos::PrimitiveType::k2DLineStrip:
      return true;
    default:
      break;
  }
  return false;
}

inline bool IsPrimitiveLine(const xenos::RegisterFile& regs) {
  return IsPrimitiveLine(regs.Get<xenos::reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
                             xenos::VGTOutputPath::kTessellationEnable,
                         regs.Get<xenos::reg::VGT_DRAW_INITIATOR>().prim_type);
}

/** Reports whether the primitive type is polygonal. */
constexpr bool IsPrimitivePolygonal(bool vgt_output_path_is_tessellation_enable,
                                    xenos::PrimitiveType type) {
  if (vgt_output_path_is_tessellation_enable &&
      (type == xenos::PrimitiveType::kTrianglePatch || type == xenos::PrimitiveType::kQuadPatch)) {
    return true;
  }
  switch (type) {
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kTriangleWithWFlags:
    case xenos::PrimitiveType::kQuadList:
    case xenos::PrimitiveType::kQuadStrip:
    case xenos::PrimitiveType::kPolygon:
      return true;
    default:
      break;
  }
  return false;
}

inline bool IsPrimitivePolygonal(const xenos::RegisterFile& regs) {
  return IsPrimitivePolygonal(regs.Get<xenos::reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
                                  xenos::VGTOutputPath::kTessellationEnable,
                              regs.Get<xenos::reg::VGT_DRAW_INITIATOR>().prim_type);
}

/** Reports whether any samples to rasterize can be generated with the current state. */
REX_GPU_XENOS_API bool IsRasterizationPotentiallyDone(const xenos::RegisterFile& regs,
                                                      bool primitive_polygonal);

/** Reports whether the depth test fails every sample and nothing else can write. */
REX_GPU_XENOS_API bool IsDepthTestNeverPassing(const xenos::RegisterFile& regs);

/**
 * Reports whether the draw rasterizes into the EDRAM render targets.
 *
 * A memexporting pixel shader still runs when the depth test never passes, as early depth testing
 * is off for it.
 */
REX_GPU_XENOS_API bool IsRasterizationDone(const xenos::RegisterFile& regs,
                                           bool primitive_polygonal, bool pixel_shader_memexports);

/** Direct3D 10.1 standard 2x sample positions in 1/16th of a pixel, also used by Vulkan. */
REX_GPU_XENOS_API extern const i8 kD3D10StandardSamplePositions2x[2][2];
/** Direct3D 10.1 standard 4x sample positions in 1/16th of a pixel, also used by Vulkan. */
REX_GPU_XENOS_API extern const i8 kD3D10StandardSamplePositions4x[4][2];

REX_GPU_XENOS_API xenos::reg::RB_DEPTHCONTROL GetNormalizedDepthControl(
    const xenos::RegisterFile& regs);

/**
 * Factor converting the guest absolute polygon offset to the host relative constant factor for
 * unorm24, including unorm24 emulated as host float32.
 */
constexpr f32 kD3D10PolygonOffsetFactorUnorm24 = static_cast<f32>((UINT32_C(1) << 24) - 1);

/**
 * Factor converting the guest absolute polygon offset to the host relative constant factor for
 * a host floating point depth buffer.
 */
constexpr f32 kD3D10PolygonOffsetFactorFloat24 = static_cast<f32>(UINT32_C(1) << (21 + 3));

/**
 * Converts the guest absolute polygon offset to the host integer polygon offset.
 *
 * The result rounds up, since any used offset must separate primitives in the depth buffer. For
 * float24 the result is a multiple of 8, as the conversion may truncate in translated pixel
 * shaders, and the rounding happens before converting float24 ULPs to float32 ULPs.
 *
 * @param polygon_offset Guest absolute polygon offset.
 */
inline i32 GetD3D10IntegerPolygonOffset(xenos::DepthRenderTargetFormat depth_format,
                                        f32 polygon_offset) {
  bool is_float24 = depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
  i32 polygon_offset_int = static_cast<i32>(std::ceil(
      std::abs(polygon_offset) * (is_float24 ? kD3D10PolygonOffsetFactorFloat24 * (1.0f / 8.0f)
                                             : kD3D10PolygonOffsetFactorUnorm24)));
  if (is_float24) {
    polygon_offset_int <<= 3;
  }
  return polygon_offset < 0 ? -polygon_offset_int : polygon_offset_int;
}

/**
 * Returns the polygon offset for hosts without separate front and back polygon offsets.
 *
 * The values keep the units of the original registers, with scale in 1/16
 * subpixels, so multiply by xenos::kPolygonOffsetScaleSubpixelUnit for pixels.
 *
 * @param scale_out Receives the polygon offset scale.
 * @param offset_out Receives the polygon offset.
 */
REX_GPU_XENOS_API void GetPreferredFacePolygonOffset(const xenos::RegisterFile& regs,
                                                     bool primitive_polygonal, f32& scale_out,
                                                     f32& offset_out);

inline bool IsCoverageAlphaDependent(xenos::reg::RB_COLORCONTROL rb_colorcontrol) {
  return (rb_colorcontrol.alpha_test_enable &&
          rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways) ||
         rb_colorcontrol.alpha_to_mask_enable;
}

/**
 * Reports whether the pixel shader is needed. Callers check IsRasterizationPotentiallyDone first.
 *
 * @param shader Pixel shader with analyzed ucode.
 * @param include_memory_export False to ignore memexport as a side effect.
 */
REX_GPU_XENOS_API bool IsPixelShaderNeededWithRasterization(const Shader& shader,
                                                            const xenos::RegisterFile& regs,
                                                            bool include_memory_export = true);

struct REX_GPU_XENOS_API HostViewport {
  /**
   * Offset of the viewport from the render target origin in pixels.
   */
  u32 xy_offset[2];
  /** Extent can be 0 for an empty viewport, which hosts without empty viewports scissor out. */
  u32 xy_extent[2];
  f32 z_min;
  f32 z_max;
  f32 ndc_scale[3];
  f32 ndc_offset[3];
};
/**
 * Converts the guest viewport, or a fake one when drawing without a viewport, to a host viewport
 * plus values to multiply add the returned position by, forcing the Direct3D clip space with 0...W
 * Z rather than -W...W.
 *
 * @param origin_bottom_left True when the host framebuffer origin is the bottom left.
 * @param x_max Maximum host viewport width.
 * @param y_max Maximum host viewport height.
 * @param allow_reverse_z False when the host disallows a reversed depth range.
 * @param normalized_depth_control Normalized RB_DEPTHCONTROL.
 * @param convert_z_to_float24 True to round the depth bounds to float24.
 * @param full_float24_in_0_to_1 True to remap float24 [0...2) to [0...1) for tile ownership.
 * @param pixel_shader_writes_depth True when the pixel shader writes depth.
 * @param viewport_info_out Receives the host viewport.
 */
REX_GPU_XENOS_API void GetHostViewport(const XenosSettings& settings,
                                       const xenos::RegisterFile& regs, u32 draw_resolution_scale_x,
                                       u32 draw_resolution_scale_y, bool origin_bottom_left,
                                       u32 x_max, u32 y_max, bool allow_reverse_z,
                                       xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                                       bool convert_z_to_float24, bool full_float24_in_0_to_1,
                                       bool pixel_shader_writes_depth,
                                       HostViewport& viewport_info_out);

struct REX_GPU_XENOS_API Scissor {
  /** Offset from the render target origin in pixels. */
  u32 offset[2];
  /** Extent can be 0. */
  u32 extent[2];
};
/**
 * Returns the window scissor intersected with the screen scissor.
 *
 * The screen scissor is always 0, 0 to 8192, 8192 in Direct3D 9.
 *
 * @param scissor_out Receives the scissor.
 * @param clamp_to_surface_pitch True to clamp the horizontal scissor to the surface pitch.
 */
REX_GPU_XENOS_API void GetScissor(const xenos::RegisterFile& regs, Scissor& scissor_out,
                                  bool clamp_to_surface_pitch = true);

/**
 * Returns the color component write mask for the draw command.
 *
 * Render targets not statically written by the pixel shader are excluded and take no ownership
 * transfers.
 *
 * @param pixel_shader_writes_color_targets Bit mask of color targets written by the pixel shader.
 */
REX_GPU_XENOS_API u32 GetNormalizedColorMask(const xenos::RegisterFile& regs,
                                             u32 pixel_shader_writes_color_targets);

/**
 * Converts a title 2x MSAA sample index to a host sample index.
 *
 * With native 2x MSAA the top left sample is 1 and the bottom right sample is 0. Otherwise the
 * top left sample 0 and the bottom right sample 3 of the guaranteed 4x MSAA are used.
 */
inline u32 GetD3D10SampleIndexForGuest2xMSAA(u32 guest_sample_index,
                                             bool native_2x_msaa_supported) {
  assert(guest_sample_index <= 1);
  if (native_2x_msaa_supported) {
    return guest_sample_index ? 0 : 1;
  }
  return guest_sample_index ? 3 : 0;
}

struct REX_GPU_XENOS_API MemExportRange {
  u32 base_address_dwords;
  u32 size_bytes;

  explicit MemExportRange(u32 base_address_dwords, u32 size_bytes)
      : base_address_dwords(base_address_dwords), size_bytes(size_bytes) {}
};

/**
 * Gathers the memory ranges involved in memexports of the shader using the float constants from
 * the registers.
 *
 * Stream constants that are not set up are skipped, as the export may not be on the control flow
 * path taken by the shader.
 *
 * @param shader Shader with analyzed ucode.
 * @param ranges_out Receives the ranges, extended in place when the base address repeats.
 */
REX_GPU_XENOS_API void AddMemExportRanges(const xenos::RegisterFile& regs, const Shader& shader,
                                          std::vector<MemExportRange>& ranges_out);

/**
 * Clamps a copy sample selection to values the shader understands, even though Direct3D 9 should
 * not pass others.
 *
 * Depth cannot be averaged.
 *
 * @param is_depth True when the source is a depth surface.
 * @return Sample selection valid for the shader.
 */
REX_GPU_XENOS_API xenos::CopySampleSelect SanitizeCopySampleSelect(
    xenos::CopySampleSelect copy_sample_select, xenos::MSAASamples msaa_samples, bool is_depth);

/** Packed, small enough to be passed to the shaders in root and push constants. */
union ResolveEDRAMLayout {
  u32 packed;
  struct {
    /** With 32bpp and 64bpp taken into account. */
    u32 pitch_tiles : xenos::kEDRAMPitchTilesBits;
    xenos::MSAASamples msaa_samples : xenos::kMSAASamplesBits;
    u32 is_depth : 1;
    /**
     * Tile offset of the region that edram_offset_x_div_8 and edram_offset_y_div_8 are relative
     * to.
     */
    u32 base_tiles : xenos::kEDRAMBaseTilesBits;
    u32 format : xenos::kRenderTargetFormatBits;
    u32 format_is_64bpp : 1;
    /**
     * Fills the half pixel offset gap on the left and top sides of the resolve region with the
     * first covered column and row when resolution scaling is active.
     */
    u32 fill_half_pixel_offset : 1;
  };
  ResolveEDRAMLayout() : packed(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(packed)); }
};

/** Packed, small enough to be passed to the shaders in root and push constants. */
union ResolveCoordinates {
  u32 packed;
  struct {
    /** In pixels relative to the origin of the EDRAM base tile, 0...9 for 0...72. */
    u32 edram_offset_x_div_8 : 4;
    /** In pixels, 0...1 for 0...8. */
    u32 edram_offset_y_div_8 : 1;

    /** In pixels. May be 0 for a malformed original rectangle, and the resolve must be skipped. */
    u32 width_div_8 : xenos::kResolveSizeBits - xenos::kResolveAlignmentPixelsLog2;

    /** Range 1 to 7. */
    u32 draw_resolution_scale_x : 3;
    u32 draw_resolution_scale_y : 3;
  };
  ResolveCoordinates() : packed(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(packed)); }
};

/**
 * Returns the tiles covered by a resolve area.
 *
 * The pitch between rows is edram_info.pitch_tiles, not the row length used.
 *
 * @param base_out Receives the first tile.
 * @param row_length_used_out Receives the width of the area in tiles.
 * @param rows_out Receives the height of the area in tiles.
 */
REX_GPU_XENOS_API void GetResolveEDRAMTileSpan(ResolveEDRAMLayout edram_info,
                                               ResolveCoordinates coordinate_info, u32 height_div_8,
                                               u32& base_out, u32& row_length_used_out,
                                               u32& rows_out);

union ResolveCopyDestCoordinates {
  u32 packed;
  struct {
    /** Range 0...16384/32. */
    u32 pitch_aligned_div_32 : xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 -
                               xenos::kTextureTileWidthHeightLog2;
    u32 height_aligned_div_32 : xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 -
                                xenos::kTextureTileWidthHeightLog2;

    /** Up to the maximum period of the texture tiled address function, 128x128 for 2D 1bpb. */
    u32 offset_x_div_8 : 7 - xenos::kResolveAlignmentPixelsLog2;
    u32 offset_y_div_8 : 7 - xenos::kResolveAlignmentPixelsLog2;

    xenos::CopySampleSelect copy_sample_select : 3;
  };
  ResolveCopyDestCoordinates() : packed(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(packed)); }
};

/** Host shaders for copying in resolve operations on drivers with Shader Model 5 like compute. */
enum class ResolveCopyShaderIndex : u8 {
  kFast32bpp1x2xMSAA,
  kFast32bpp4xMSAA,
  kFast64bpp1x2xMSAA,
  kFast64bpp4xMSAA,

  kFull8bpp,
  kFull16bpp,
  kFull32bpp,
  kFull64bpp,
  kFull128bpp,

  kCount,
  kUnknown = kCount,
};

struct REX_GPU_XENOS_API ResolveCopyShaderTraits {
  const char* debug_name;
  /**
   * True when the EDRAM source is bound as a raw buffer (ByteAddressBuffer in Direct3D). Hosts
   * without raw buffers use a typed buffer with source_bpe_log2 byte elements instead.
   */
  bool source_is_raw;
  /** Log2 of bytes per element of the EDRAM buffer type bound to the shader, at least 2. */
  u32 source_bpe_log2;
  /**
   * Log2 of bytes per element of the destination buffer type bound to the shader. At least 2
   * because the 128 megatexel minimum on Direct3D 10+ prevents binding the entire shared memory
   * buffer with smaller elements (D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP).
   */
  u32 dest_bpe_log2;
  /** Log2 of the pixel count in a thread group along X and Y, 64 threads per group preferred. */
  u32 group_size_x_log2, group_size_y_log2;
};

REX_GPU_XENOS_API extern const ResolveCopyShaderTraits
    kResolveCopyShaderInfo[static_cast<size_t>(ResolveCopyShaderIndex::kCount)];

struct REX_GPU_XENOS_API ResolveCopyShaderConstants {
  /** Passed alone when the destination base is not needed, to use fewer constants. */
  struct DestRelative {
    ResolveEDRAMLayout edram_info;
    ResolveCoordinates coordinate_info;
    xenos::reg::RB_COPY_DEST_INFO dest_info;
    ResolveCopyDestCoordinates dest_coordinate_info;
  };
  DestRelative dest_relative;
  u32 dest_base;
};

struct REX_GPU_XENOS_API ResolveClearShaderConstants {
  /** Differs between color and depth, the rest can stay in the root bindings. */
  struct RenderTargetSpecific {
    u32 clear_value[2];
    ResolveEDRAMLayout edram_info;
  };
  RenderTargetSpecific rt_specific;
  ResolveCoordinates coordinate_info;
};

struct REX_GPU_XENOS_API ResolveRequest {
  xenos::reg::RB_COPY_CONTROL rb_copy_control;

  /** Set up when copying or clearing depth, along with depth_original_base. */
  ResolveEDRAMLayout depth_edram_info;
  /** Set up when copying or clearing color, along with color_original_base. */
  ResolveEDRAMLayout color_edram_info;
  /**
   * Original bases, without adjustment to a 160x32 region for packed offsets, used to locate host
   * render targets for clears when they emulate EDRAM.
   */
  u32 depth_original_base;
  u32 color_original_base;

  ResolveCoordinates coordinate_info;
  /**
   * Like coordinate_info.width_div_8 but not needed for shaders. In pixels. May be 0 for a
   * malformed original rectangle, and the resolve must be skipped.
   */
  u32 height_div_8;

  xenos::reg::RB_COPY_DEST_INFO copy_dest_info;
  ResolveCopyDestCoordinates copy_dest_coordinate_info;

  /**
   * Address of the texture or the location within it that the offset_x/y_div_8 of
   * copy_dest_coordinate_info is relative to, the origin of the copy destination.
   */
  u32 copy_dest_base;
  /**
   * Memory range potentially modified by copying to the texture. The length may be 0 for a bad
   * destination, and clearing may still be done but copying must be skipped.
   */
  u32 copy_dest_extent_start;
  u32 copy_dest_extent_length;

  /** The clear shaders always write to a uint4 view of EDRAM. */
  u32 rb_depth_clear;
  u32 rb_color_clear;
  u32 rb_color_clear_lo;

  bool IsCopyingDepth() const {
    return rb_copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
  }

  /**
   * Returns the EDRAM tile span of the copy source, see GetResolveEDRAMTileSpan.
   *
   * @param base_out Receives the first tile.
   * @param row_length_used_out Receives the width of the area in tiles.
   * @param rows_out Receives the height of the area in tiles.
   * @param pitch_out Receives the pitch between rows in tiles.
   */
  void GetCopyEDRAMTileSpan(u32& base_out, u32& row_length_used_out, u32& rows_out,
                            u32& pitch_out) const {
    ResolveEDRAMLayout edram_info = IsCopyingDepth() ? depth_edram_info : color_edram_info;
    GetResolveEDRAMTileSpan(edram_info, coordinate_info, height_div_8, base_out,
                            row_length_used_out, rows_out);
    pitch_out = edram_info.pitch_tiles;
  }

  ResolveCopyShaderIndex GetCopyShader(u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
                                       ResolveCopyShaderConstants& constants_out,
                                       u32& group_count_x_out, u32& group_count_y_out) const;

  bool IsClearingDepth() const { return rb_copy_control.depth_clear_enable != 0; }

  bool IsClearingColor() const {
    return !IsCopyingDepth() && rb_copy_control.color_clear_enable != 0;
  }

  void GetDepthClearShaderConstants(ResolveClearShaderConstants& constants_out) const {
    REX_ASSERT(IsClearingDepth());
    constants_out.rt_specific.clear_value[0] = rb_depth_clear;
    constants_out.rt_specific.clear_value[1] = rb_depth_clear;
    constants_out.rt_specific.edram_info = depth_edram_info;
    constants_out.coordinate_info = coordinate_info;
  }

  /**
   * Fills the color clear shader constants.
   *
   * The -32...32 to -1...1 clamping of the k_16_16 and k_16_16_16_16 blending emulation on host
   * render targets is not applied, as it would be inconsistent with clearing with a depth quad.
   *
   * @param constants_out Receives the constants.
   */
  void GetColorClearShaderConstants(ResolveClearShaderConstants& constants_out) const {
    REX_ASSERT(IsClearingColor());
    constants_out.rt_specific.clear_value[0] = rb_color_clear;
    constants_out.rt_specific.clear_value[1] = rb_color_clear_lo;
    constants_out.rt_specific.edram_info = color_edram_info;
    constants_out.coordinate_info = coordinate_info;
  }

  /**
   * Returns the clear shader group counts along X and Y.
   *
   * Each invocation handles 8 guest MSAA samples.
   */
  std::pair<u32, u32> GetClearShaderGroupCount(u32 draw_resolution_scale_x,
                                               u32 draw_resolution_scale_y) const {
    u32 width_samples_div_8 = coordinate_info.width_div_8;
    u32 height_samples_div_8 = height_div_8;
    xenos::MSAASamples samples =
        IsCopyingDepth() ? depth_edram_info.msaa_samples : color_edram_info.msaa_samples;
    if (samples >= xenos::MSAASamples::k2X) {
      height_samples_div_8 <<= 1;
      if (samples >= xenos::MSAASamples::k4X) {
        width_samples_div_8 <<= 1;
      }
    }
    width_samples_div_8 *= draw_resolution_scale_x;
    height_samples_div_8 *= draw_resolution_scale_y;
    return std::make_pair((width_samples_div_8 + static_cast<u32>(7)) >> 3, height_samples_div_8);
  }
};

/**
 * Returns the resolve info from the registers. An invalid resolve leaves info_out empty. It returns
 * false when an error made the info invalid.
 *
 * @param fixed_rg16_truncated_to_minus_1_to_1 True when 16_16 is emulated as snorm in -1...1.
 * @param fixed_rgba16_truncated_to_minus_1_to_1 True when 16_16_16_16 is emulated as snorm.
 * @param info_out Receives the resolve info.
 */
REX_GPU_XENOS_API bool GetResolveInfo(const XenosSettings& settings,
                                      const xenos::RegisterFile& regs, IPhysicalMemory& memory,
                                      u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
                                      bool fixed_rg16_truncated_to_minus_1_to_1,
                                      bool fixed_rgba16_truncated_to_minus_1_to_1,
                                      ResolveRequest& info_out);

}  // namespace rex::gpu::draw_state
