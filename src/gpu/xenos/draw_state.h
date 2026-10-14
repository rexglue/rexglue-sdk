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

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/register_file.h>
#include <rex/xenos/registers.h>
#include <rex/xenos/xenos.h>
#include <rex/memory.h>
#include <rex/runtime/xmemory.h>

namespace rex::gpu::draw_state {

/**
 * Compares 2 runs of floats by bit pattern, so a NaN equals itself.
 *
 * @param a The first run.
 * @param b The second run.
 * @return True when both runs have the same length and the same bits.
 */
inline bool AreBitsEqual(std::span<const f32> a, std::span<const f32> b) {
  return std::ranges::equal(
      a, b, [](f32 x, f32 y) { return std::bit_cast<u32>(x) == std::bit_cast<u32>(y); });
}

/**
 * Reports whether the primitive type is a line.
 *
 * For patch primitive types the major mode is always explicit, so checking whether
 * VGT_OUTPUT_PATH_CNTL::path_select is kTessellationEnable is enough.
 *
 * @param vgt_output_path_is_tessellation_enable True when the output path is tessellation.
 * @param type Primitive type.
 * @return True when the primitive type is a line.
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

/**
 * Reports whether the primitive type is polygonal.
 *
 * Polygonal types, which exclude points and lines, are rasterized as triangles, have front and
 * back faces, and support face culling and fill modes (polymode_front_ptype,
 * polymode_back_ptype). Other types are always front, are not affected by front and back face
 * culling in OpenGL and Vulkan even with glCullFace set to GL_FRONT_AND_BACK, and may use the
 * para registers such as poly_offset_para_enable instead of the front or back ones. For patch
 * primitive types the major mode is always explicit, so checking whether
 * VGT_OUTPUT_PATH_CNTL::path_select is kTessellationEnable is enough.
 *
 * @param vgt_output_path_is_tessellation_enable True when the output path is tessellation.
 * @param type Primitive type.
 * @return True when the primitive type is polygonal.
 */
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

/**
 * Reports whether any samples to rasterize can be generated with the current state.
 *
 * Samples count for any reason, including sample counting and pixel shader memexport, not only
 * writing to a render target. Draws can be dropped only when the vertex shader does not
 * memexport. Checks mostly special cases, not whether the viewport or scissor are empty.
 *
 * @param regs Register file.
 * @param primitive_polygonal True when the primitive type is polygonal.
 * @return True when rasterization can produce samples.
 */
bool IsRasterizationPotentiallyDone(const xenos::RegisterFile& regs, bool primitive_polygonal);

/** Direct3D 10.1 standard 2x sample positions in 1/16th of a pixel, also used by Vulkan. */
extern const i8 kD3D10StandardSamplePositions2x[2][2];
/** Direct3D 10.1 standard 4x sample positions in 1/16th of a pixel, also used by Vulkan. */
extern const i8 kD3D10StandardSamplePositions4x[4][2];

xenos::reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const xenos::RegisterFile& regs);

/**
 * Factor converting the guest absolute polygon offset to the host relative constant factor for
 * unorm24, including unorm24 emulated as host float32.
 *
 * Direct3D 9 and Xenos use an absolute offset of slope * slope factor + constant offset.
 * Direct3D 10, Metal, OpenGL and Vulkan use slope * slope factor + maximum resolvable difference
 * * constant factor. The maximum resolvable difference is the minimum representable non zero
 * value for a fixed point depth buffer, 1 / (2^24 - 1) for unorm24, and 2 ^ (exponent of the
 * maximum Z in the primitive - explicitly stored mantissa bits) for a floating point one, with
 * 23 bits for float32 and 20 bits for float24. For native unorm24 the factor is the inverse of
 * the minimum representable non zero value. For unorm24 emulated as float32 that minimum for a
 * primitive in the [0.5, 1) range is 2^-24, so the factor is almost 2^24.
 *
 * The host translation must keep values separating coplanar geometry on the guest separating it
 * on the host.
 */
constexpr f32 kD3D10PolygonOffsetFactorUnorm24 = static_cast<f32>((UINT32_C(1) << 24) - 1);

/**
 * Factor converting the guest absolute polygon offset to the host relative constant factor for
 * a host floating point depth buffer.
 *
 * The integer depth bias is roughly the ULP count separating primitives. Float24 has 3 fewer
 * mantissa bits than float32, so 1 float24 ULP is 8 float32 ULPs and each layer of the title
 * value needs an offset of 8 with a float32 depth buffer. The scale is set by the [0.5, 1) range,
 * where Z has exponent -1 and the float24 offset is 2 ^ (-1 - 20) * constant factor, so the
 * absolute offset is multiplied by 2^21. Float32 depth may be snapped to float24 in translated
 * pixel shaders by truncation, so the host offset is calculated as for float24 depth or it is
 * lost. When the 0...0.5 host range represents the 0...1 guest range, the offset needs no change
 * since it is calculated after the viewport transformation, which lowers the exponent by 1.
 */
constexpr f32 kD3D10PolygonOffsetFactorFloat24 = static_cast<f32>(UINT32_C(1) << (21 + 3));

/**
 * Converts the guest absolute polygon offset to the host integer polygon offset.
 *
 * The result rounds up, since any used offset must separate primitives in the depth buffer. For
 * float24 the conversion may truncate in translated pixel shaders, so the result is a multiple of
 * 2^3 and the rounding happens before changing units from float24 ULPs to float32 ULPs.
 *
 * @param depth_format Depth render target format.
 * @param polygon_offset Guest absolute polygon offset.
 * @return Host integer polygon offset.
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
 * Picks the front face offset when enabled and not culled, otherwise the back face offset when
 * enabled and not culled. The front offset is
 * preferred since front faces are usually rendered, except for shadow volumes. Non polygonal
 * primitives use the front offset toggled by poly_offset_para_enable. The values have the units
 * of the original registers, with scale in 1/16 subpixels, so multiply by
 * xenos::kPolygonOffsetScaleSubpixelUnit for pixels.
 *
 * @param regs Register file.
 * @param primitive_polygonal True when the primitive type is polygonal.
 * @param scale_out Receives the polygon offset scale.
 * @param offset_out Receives the polygon offset.
 */
void GetPreferredFacePolygonOffset(const xenos::RegisterFile& regs, bool primitive_polygonal,
                                   f32& scale_out, f32& offset_out);

inline bool DoesCoverageDependOnAlpha(xenos::reg::RB_COLORCONTROL rb_colorcontrol) {
  return (rb_colorcontrol.alpha_test_enable &&
          rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways) ||
         rb_colorcontrol.alpha_to_mask_enable;
}

/**
 * Reports whether the pixel shader is needed, so it cannot be disabled on the host to speed up
 * depth prepasses and shadowmaps.
 *
 * The result is false and the function is not called when IsRasterizationPotentiallyDone is
 * false. Discarding, depth output and memexport count as side effects, and depth output is
 * treated as always mattering. Rejecting the pixel shader covers memexport draws and 1 point
 * draws covering nothing that run with a leftover pixel shader and destroyed SQ_PROGRAM_CNTL,
 * which avoids unneeded translations of random shaders with different host modification bits such
 * as register count and depth format.
 *
 * @param shader Pixel shader with analyzed ucode.
 * @param regs Register file.
 * @param include_memory_export False to ignore memexport as a side effect.
 * @return True when the pixel shader is needed.
 */
bool IsPixelShaderNeededWithRasterization(const Shader& shader, const xenos::RegisterFile& regs,
                                          bool include_memory_export = true);

struct ViewportInfo {
  /**
   * Offset from render target UV = 0 to +UV.
   *
   * Viewport bounds are integers and non negative, which makes cropping to the host maximum size
   * simple, matches Direct3D 12 clipping and scissoring of a fractional viewport to
   * floor(TopLeftXY) ... floor(TopLeftXY + WidthHeight), hides the difference between 0 and 8+
   * viewportSubPixelBits on Vulkan, and avoids numerical error in host bounds checks. All
   * fractional offsetting, including the half-pixel offset, and cropping go through ndc_scale and
   * ndc_offset.
   */
  u32 xy_offset[2];
  /** Extent can be 0 for an empty viewport, which hosts without empty viewports scissor out. */
  u32 xy_extent[2];
  f32 z_min;
  f32 z_max;
  /** The scale is applied before the offset, as in a multiply add. */
  f32 ndc_scale[3];
  f32 ndc_offset[3];
};
/**
 * Converts the guest viewport, or a fake one when drawing without a viewport, to a host viewport
 * plus values to multiply add the returned position by, forcing the Direct3D clip space with 0...W
 * Z rather than -W...W.
 *
 * A position goes through clip space, clipping, division by W, NDC, viewport scaling, and the
 * viewport, window and half-pixel offsetting. NDC XY point toward +XY on the render target. The
 * integer.0 and integer.5 offsets are summed separately at full precision. The viewport scale
 * sign maps the Direct3D 9 Y direction into the NDC to pixel transformation. With resolution
 * scaling guest pixels are treated as a whole.
 *
 * Host APIs always clip, and only Z clipping can be replaced with depth clamping.
 *
 * With clipping, the window and half-pixel offsets apply after clipping. Host GPUs other than
 * WARP do not truly clip to -W...W. Primitives overflow the fractional rectangle, and the
 * viewport scissor is floor(TopLeftXY) ... floor(TopLeftXY + WidthHeight) in float32, where
 * TopLeftXY 0x3F7FFFFF covers the top row and left column samples and 0x3F800000 covers none.
 * The result matches passing the guest values directly to Direct3D 12. The integer viewport is
 * returned and the fractional offset goes to the vertex shaders through ndc_scale and
 * ndc_offset. Offsetting in W scaled rather than W divided units keeps the exponent ratios, and
 * rasterization is 16.8 fixed point on most hosts, so coverage is unaffected. Xenos has no
 * viewport scissor. The title driver writes the intersection of the viewport and the scissor to
 * PA_SC_WINDOW_SCISSOR, so the scissor of a fractional viewport is up to the title. The title
 * clip space may be 0...W or -W...W and is always remapped to 0...W, with 0 moved to 0.5 and so
 * not exact. For -W...W the host depth range is offset - scale and scale * 2, with ndc_scale 0.5
 * and ndc_offset 0.5 for Z.
 *
 * Without clipping, the vertex shader returns a position in pixels pre-divided by W, the viewport
 * is usually disabled through PA_CL_VTE_CNTL VPORT_X/Y/Z_SCALE/OFFSET_ENA, which equals scale
 * (1, 1, 1) and offset (0, 0, 0), and the NDC is treated as pixel coordinates before the window
 * and half-pixel offsets. Direct3D 10 hosts clamp Z to the viewport depth range in the output
 * merger, so the host depth range is the full 0...1 and the guest Z scale and offset go through
 * ndc_scale and ndc_offset, leaving Z unclamped to the guest viewport Z range. The host viewport
 * is at least the scissor region, with Z clipping changed to Z clamping where the host allows it,
 * ideally a power of 2 size so scaling only biases the exponent, and the vertex shader rescales
 * from the guest clip space.
 *
 * When the pixel shader writes depth the host depth range is 0...1, as guest pixel shaders have
 * no access to the viewport Z. Otherwise it is clamped to 0...1 since Direct3D allows no
 * unrestricted range. With convert_z_to_float24 the bounds are rounded to the nearest even
 * float24 value, as the pixel shader clamps converted depth to them, and vertex Z is not adjusted
 * so polygons at Z = 0 or Z = W are not clipped.
 *
 * @param regs Register file.
 * @param draw_resolution_scale_x Resolution scale along X.
 * @param draw_resolution_scale_y Resolution scale along Y.
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
void GetHostViewportInfo(const xenos::RegisterFile& regs, u32 draw_resolution_scale_x,
                         u32 draw_resolution_scale_y, bool origin_bottom_left, u32 x_max, u32 y_max,
                         bool allow_reverse_z, xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                         bool convert_z_to_float24, bool full_float24_in_0_to_1,
                         bool pixel_shader_writes_depth, ViewportInfo& viewport_info_out);

struct Scissor {
  /** Offset from render target UV = 0 to +UV. */
  u32 offset[2];
  /** Extent can be 0. */
  u32 extent[2];
};
/**
 * Returns the window scissor intersected with the screen scissor.
 *
 * The screen scissor is always 0, 0 to 8192, 8192 in Direct3D 9. A rectangle with a negative
 * extent collapses to 0 size rather than reordering its bounds, which would reveal samples not
 * meant to be covered, and negative offsets are dropped. Clamping the horizontal scissor to the
 * surface pitch prevents overflow in hosts that rasterize independently of the render target
 * width or use render targets padded to EDRAM tiles, and titles do not clamp it when drawing
 * without clipping.
 *
 * @param regs Register file.
 * @param scissor_out Receives the scissor.
 * @param clamp_to_surface_pitch True to clamp the horizontal scissor to the surface pitch.
 */
void GetScissor(const xenos::RegisterFile& regs, Scissor& scissor_out,
                bool clamp_to_surface_pitch = true);

/**
 * Returns the color component write mask for the draw command.
 *
 * Render targets not statically written by the pixel shader are excluded and take no ownership
 * transfers. Render targets with only non existent components written are skipped. Non existent
 * components are forced to written when some existing components of the render target are used,
 * so the host driver takes no slow path that reads and mixes disabled components.
 *
 * @param regs Register file.
 * @param pixel_shader_writes_color_targets Bit mask of color targets written by the pixel shader.
 * @return Normalized color write mask.
 */
u32 GetNormalizedColorMask(const xenos::RegisterFile& regs, u32 pixel_shader_writes_color_targets);

/**
 * Converts a title 2x MSAA sample index to a host sample index.
 *
 * The conversion is never an identity, so shaders can always use conditional moves that become
 * no ops. Multisampled render targets are stored
 * like 1x2 single sampled pixels at 2x and 2x2 at 4x, so sample 0 is assumed to be the top sample
 * and sample 1 the bottom one. With native 2x MSAA on Direct3D 10.1 the top-left sample is 1 and
 * the bottom-right sample is 0. Without it, the top-left sample 0 and the bottom-right sample 3
 * of the guaranteed 4x MSAA are used.
 *
 * @param guest_sample_index Guest sample index, 0 or 1.
 * @param native_2x_msaa_supported True when the host supports native 2x MSAA.
 * @return Host sample index.
 */
inline u32 GetD3D10SampleIndexForGuest2xMSAA(u32 guest_sample_index,
                                             bool native_2x_msaa_supported) {
  assert(guest_sample_index <= 1);
  if (native_2x_msaa_supported) {
    return guest_sample_index ? 0 : 1;
  }
  return guest_sample_index ? 3 : 0;
}

struct MemExportRange {
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
 * path taken by the shader. They are detected through the constant bit fields and the index count
 * but not through the Y component, since the index may be constructed arbitrarily. The hardware
 * validates the upper bits of eA.
 *
 * @param regs Register file.
 * @param shader Shader with analyzed ucode.
 * @param ranges_out Receives the ranges, extended in place when the base address repeats.
 */
void AddMemExportRanges(const xenos::RegisterFile& regs, const Shader& shader,
                        std::vector<MemExportRange>& ranges_out);

/**
 * Clamps a copy sample selection to values the shader understands, even though Direct3D 9 should
 * not pass others.
 *
 * Depth cannot be averaged.
 *
 * @param copy_sample_select Requested sample selection.
 * @param msaa_samples Sample count of the surface.
 * @param is_depth True when the source is a depth surface.
 * @return Sample selection valid for the shader.
 */
xenos::CopySampleSelect SanitizeCopySampleSelect(xenos::CopySampleSelect copy_sample_select,
                                                 xenos::MSAASamples msaa_samples, bool is_depth);

/** Packed, small enough to be passed to the shaders in root and push constants. */
union ResolveEDRAMInfo {
  u32 packed;
  struct {
    /** With 32bpp and 64bpp taken into account. */
    u32 pitch_tiles : xenos::kEDRAMPitchTilesBits;
    xenos::MSAASamples msaa_samples : xenos::kMSAASamplesBits;
    u32 is_depth : 1;
    /** With offset to the region that edram_offset_x/y_div_8 are relative to. */
    u32 base_tiles : xenos::kEDRAMBaseTilesBits;
    u32 format : xenos::kRenderTargetFormatBits;
    u32 format_is_64bpp : 1;
    /**
     * Fills the half-pixel offset gap on the left and top sides of the resolve region with the
     * first covered column and row when resolution scaling.
     */
    u32 fill_half_pixel_offset : 1;
  };
  ResolveEDRAMInfo() : packed(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(packed)); }
};

/** Packed, small enough to be passed to the shaders in root and push constants. */
union ResolveCoordinateInfo {
  u32 packed;
  struct {
    /** In pixels relative to the origin of the EDRAM base tile, 0...9 for 0...72. */
    u32 edram_offset_x_div_8 : 4;
    /** 0...1 for 0...8. */
    u32 edram_offset_y_div_8 : 1;

    /** In pixels. May be 0 for a malformed original rectangle, and the resolve must be dropped. */
    u32 width_div_8 : xenos::kResolveSizeBits - xenos::kResolveAlignmentPixelsLog2;

    /** 1 to 7. */
    u32 draw_resolution_scale_x : 3;
    u32 draw_resolution_scale_y : 3;
  };
  ResolveCoordinateInfo() : packed(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(packed)); }
};

/**
 * Returns the tiles covered by a resolve area.
 *
 * The row length used is the width of the area in tiles, while the pitch between rows is
 * edram_info.pitch_tiles. The offset is always taken into account, as it is not assumed to be
 * limited to (80 - 8, 8 - 8) with 2x MSAA and (40 - 8, 8 - 8) with 4x MSAA, and 64bpp changes it.
 *
 * @param edram_info Packed EDRAM info.
 * @param coordinate_info Packed coordinate info.
 * @param height_div_8 Height of the area divided by 8.
 * @param base_out Receives the first tile.
 * @param row_length_used_out Receives the width of the area in tiles.
 * @param rows_out Receives the height of the area in tiles.
 */
void GetResolveEDRAMTileSpan(ResolveEDRAMInfo edram_info, ResolveCoordinateInfo coordinate_info,
                             u32 height_div_8, u32& base_out, u32& row_length_used_out,
                             u32& rows_out);

union ResolveCopyDestCoordinateInfo {
  u32 packed;
  struct {
    /** 0...16384/32. */
    u32 pitch_aligned_div_32 : xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 -
                               xenos::kTextureTileWidthHeightLog2;
    u32 height_aligned_div_32 : xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 -
                                xenos::kTextureTileWidthHeightLog2;

    /** Up to the maximum period of the texture tiled address function, 128x128 for 2D 1bpb. */
    u32 offset_x_div_8 : 7 - xenos::kResolveAlignmentPixelsLog2;
    u32 offset_y_div_8 : 7 - xenos::kResolveAlignmentPixelsLog2;

    xenos::CopySampleSelect copy_sample_select : 3;
  };
  ResolveCopyDestCoordinateInfo() : packed(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(packed)); }
};

/** Host shaders for copying in resolve operations on backends with Shader Model 5 like compute. */
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

struct ResolveCopyShaderInfo {
  /** Debug name of the pipeline state object with this shader. */
  const char* debug_name;
  /**
   * True when the EDRAM source is bound as a raw buffer (ByteAddressBuffer in Direct3D), since
   * it can load different numbers of 32 bit values at once. Hosts without raw buffers use a typed
   * buffer with source_bpe_log2 byte elements instead.
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

extern const ResolveCopyShaderInfo
    kResolveCopyShaderInfo[static_cast<size_t>(ResolveCopyShaderIndex::kCount)];

struct ResolveCopyShaderConstants {
  /**
   * When the destination base is not needed, as when not binding the entire shared memory buffer,
   * only this part may be passed to the shader to use fewer constants.
   */
  struct DestRelative {
    ResolveEDRAMInfo edram_info;
    ResolveCoordinateInfo coordinate_info;
    xenos::reg::RB_COPY_DEST_INFO dest_info;
    ResolveCopyDestCoordinateInfo dest_coordinate_info;
  };
  DestRelative dest_relative;
  u32 dest_base;
};

struct ResolveClearShaderConstants {
  /**
   * Differs for color and depth, while the rest is the same and can be preserved in the root
   * bindings when going from depth to color.
   */
  struct RenderTargetSpecific {
    u32 clear_value[2];
    ResolveEDRAMInfo edram_info;
  };
  RenderTargetSpecific rt_specific;
  ResolveCoordinateInfo coordinate_info;
};

struct ResolveInfo {
  xenos::reg::RB_COPY_CONTROL rb_copy_control;

  /**
   * Set up when copying or clearing depth, along with depth_original_base, according to
   * RB_COPY_CONTROL.
   */
  ResolveEDRAMInfo depth_edram_info;
  /**
   * Set up when copying or clearing color, along with color_original_base, according to
   * RB_COPY_CONTROL.
   */
  ResolveEDRAMInfo color_edram_info;
  /**
   * Original bases, without adjustment to a 160x32 region for packed offsets, used to locate host
   * render targets for clears when they emulate EDRAM. They equal the base the render target
   * is drawn with next, which avoids unneeded tile ownership transfers between clears and
   * first usage when clearing a subregion.
   */
  u32 depth_original_base;
  u32 color_original_base;

  ResolveCoordinateInfo coordinate_info;
  /**
   * Like coordinate_info.width_div_8 but not needed for shaders. In pixels. May be 0 for a
   * malformed original rectangle, and the resolve must be dropped.
   */
  u32 height_div_8;

  xenos::reg::RB_COPY_DEST_INFO copy_dest_info;
  ResolveCopyDestCoordinateInfo copy_dest_coordinate_info;

  /**
   * Address of the texture or the location within it that the offset_x/y_div_8 of
   * copy_dest_coordinate_info is relative to, the origin of the copy destination.
   */
  u32 copy_dest_base;
  /**
   * Memory range potentially modified by copying to the texture. The length may be 0 for a bad
   * destination, and clearing may still be done but copying must be dropped.
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
    ResolveEDRAMInfo edram_info = IsCopyingDepth() ? depth_edram_info : color_edram_info;
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
   * The -32...32 to -1...1 clamping used by the k_16_16 and k_16_16_16_16 blending emulation on
   * host render targets is not applied, as it would be inconsistent with clearing with a depth
   * quad.
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
   *
   * @param draw_resolution_scale_x Resolution scale along X.
   * @param draw_resolution_scale_y Resolution scale along Y.
   * @return Group counts along X and Y.
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
 * Returns the resolve info from the registers.
 *
 * The resolve extent comes from the vertices in vf0, which are written by the CPU, rasterized
 * with the top-left rule. Most vertices have a negative half-pixel offset that is reversed. The
 * rectangle is expanded to the resolve alignment, as D3DDevice_Resolve rounds the right and
 * bottom up to 8 and the left and top are assumed to expand too. Depth destinations pass the
 * k_24_8 or k_24_8_FLOAT format to the shader instead of the Direct3D 9 k_8_8_8_8 uint format so
 * no conversion is done. EDRAM tile bases may wrap, and where modulo xenos::kEDRAMTileCount is
 * applied does not matter. For k_16_16 and k_16_16_16_16 targets emulated with the range
 * truncated to -1...1, where 0x8001 = -1 and 0x7FFF = 1, textures expect 0x8001 = -32 and
 * 0x7FFF = 32, so the exponent bias is raised by 5, which is incorrect for requested biases of 27
 * or above. The 3D destination base pointer is already adjusted to Z / 8, as copy_dest_layer is
 * 3 bits. An invalid resolve leaves info_out empty.
 *
 * @param regs Register file.
 * @param memory Guest memory.
 * @param draw_resolution_scale_x Resolution scale along X.
 * @param draw_resolution_scale_y Resolution scale along Y.
 * @param fixed_rg16_truncated_to_minus_1_to_1 True when 16_16 is emulated as snorm in -1...1.
 * @param fixed_rgba16_truncated_to_minus_1_to_1 True when 16_16_16_16 is emulated as snorm.
 * @param info_out Receives the resolve info.
 * @return False when an error made the info invalid.
 */
bool GetResolveInfo(const xenos::RegisterFile& regs, const runtime::Memory& memory,
                    u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
                    bool fixed_rg16_truncated_to_minus_1_to_1,
                    bool fixed_rgba16_truncated_to_minus_1_to_1, ResolveInfo& info_out);

}  // namespace rex::gpu::draw_state
