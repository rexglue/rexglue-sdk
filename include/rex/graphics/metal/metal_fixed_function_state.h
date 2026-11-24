/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_FIXED_FUNCTION_STATE_H_
#define XENIA_GPU_METAL_METAL_FIXED_FUNCTION_STATE_H_

#include "gpu/xenos/draw_state.h"
#include <algorithm>
#include <array>
#include <cstdint>

#include <gpu/xenos/metal/metal_draw_util.h>

namespace rex {
namespace graphics {
namespace metal {

// Device-independent form of MTLScissorRect. Kept free of Metal types so the
// exact production attachment-clamp logic is covered by the L0 conformance
// binary without requiring a device or Objective-C++.
struct MetalScissorRect {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;

  bool empty() const { return width == 0 || height == 0; }
};

// Device-independent representation of Metal's dynamic face-raster state.
// kAll is a draw no-op because Metal has no front-and-back cull enum.
enum class MetalCullMode : uint32_t {
  kNone,
  kFront,
  kBack,
  kAll,
};

// Metal clips depth to its host clip volume by default. Xenos
// PA_CL_CLIP_CNTL::clip_disable instead requests depth clamping, allowing
// slightly out-of-range Z (common in screen-space rectangles) to rasterize.
enum class MetalDepthClipMode : uint32_t {
  kClip,
  kClamp,
};

inline MetalDepthClipMode GetMetalDepthClipMode(reg::PA_CL_CLIP_CNTL mode) {
  return mode.clip_disable ? MetalDepthClipMode::kClamp
                           : MetalDepthClipMode::kClip;
}

struct MetalCullFrontFaceState {
  MetalCullMode cull_mode;
  bool front_face_clockwise;

  bool culls_all() const { return cull_mode == MetalCullMode::kAll; }
};

// Device-independent representation of Metal's dynamic triangle fill and
// depth-bias state. Metal has no point polygon fill mode, so Xenos point fill
// is conservatively represented as lines, matching the shared D3D12 fallback.
enum class MetalTriangleFillMode : uint32_t {
  kFill,
  kLines,
};

struct MetalFillDepthBiasState {
  MetalTriangleFillMode triangle_fill_mode;
  float depth_bias_constant;
  float depth_bias_slope;
};

// Device-independent representation of Metal's per-draw blend color and
// stencil reference state. References are zeroed while stencil is disabled so
// the helper has a deterministic safe state; an applicable stencil draw always
// establishes its complete front/back values on the encoder.
struct MetalBlendStencilState {
  float blend_color_red;
  float blend_color_green;
  float blend_color_blue;
  float blend_color_alpha;
  bool stencil_enabled;
  uint32_t stencil_reference_front;
  uint32_t stencil_reference_back;
};

// Device-independent color-attachment blend state. RGB and alpha factors are
// normalized separately because Xenos color factors in an alpha equation refer
// to the corresponding alpha component, matching the D3D12 pipeline contract.
// Unknown guest factors fail closed to zero; unknown operations fall back to
// add. The normalized write-mask nibble is the sole write-eligibility source.
enum class MetalBlendFactor : uint32_t {
  kZero,
  kOne,
  kSourceColor,
  kOneMinusSourceColor,
  kSourceAlpha,
  kOneMinusSourceAlpha,
  kDestinationColor,
  kOneMinusDestinationColor,
  kDestinationAlpha,
  kOneMinusDestinationAlpha,
  kBlendColor,
  kOneMinusBlendColor,
  kBlendAlpha,
  kOneMinusBlendAlpha,
  kSourceAlphaSaturated,
};

enum class MetalBlendOperation : uint32_t {
  kAdd,
  kSubtract,
  kMin,
  kMax,
  kReverseSubtract,
};

struct MetalBlendEquationState {
  MetalBlendFactor source_factor;
  MetalBlendFactor destination_factor;
  MetalBlendOperation operation;

  bool is_opaque_replacement() const {
    return source_factor == MetalBlendFactor::kOne &&
           destination_factor == MetalBlendFactor::kZero &&
           operation == MetalBlendOperation::kAdd;
  }
};

struct MetalColorAttachmentBlendState {
  uint32_t write_mask;
  MetalBlendEquationState rgb;
  MetalBlendEquationState alpha;
  bool blending_required;
};

inline MetalBlendFactor GetMetalRgbBlendFactor(xenos::BlendFactor factor) {
  switch (factor) {
    case xenos::BlendFactor::kZero:
      return MetalBlendFactor::kZero;
    case xenos::BlendFactor::kOne:
      return MetalBlendFactor::kOne;
    case xenos::BlendFactor::kSrcColor:
      return MetalBlendFactor::kSourceColor;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return MetalBlendFactor::kOneMinusSourceColor;
    case xenos::BlendFactor::kSrcAlpha:
      return MetalBlendFactor::kSourceAlpha;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return MetalBlendFactor::kOneMinusSourceAlpha;
    case xenos::BlendFactor::kDstColor:
      return MetalBlendFactor::kDestinationColor;
    case xenos::BlendFactor::kOneMinusDstColor:
      return MetalBlendFactor::kOneMinusDestinationColor;
    case xenos::BlendFactor::kDstAlpha:
      return MetalBlendFactor::kDestinationAlpha;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return MetalBlendFactor::kOneMinusDestinationAlpha;
    case xenos::BlendFactor::kConstantColor:
      return MetalBlendFactor::kBlendColor;
    case xenos::BlendFactor::kOneMinusConstantColor:
      return MetalBlendFactor::kOneMinusBlendColor;
    case xenos::BlendFactor::kConstantAlpha:
      return MetalBlendFactor::kBlendAlpha;
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return MetalBlendFactor::kOneMinusBlendAlpha;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return MetalBlendFactor::kSourceAlphaSaturated;
    default:
      return MetalBlendFactor::kZero;
  }
}

inline MetalBlendFactor GetMetalAlphaBlendFactor(xenos::BlendFactor factor) {
  switch (factor) {
    case xenos::BlendFactor::kZero:
      return MetalBlendFactor::kZero;
    case xenos::BlendFactor::kOne:
      return MetalBlendFactor::kOne;
    case xenos::BlendFactor::kSrcColor:
    case xenos::BlendFactor::kSrcAlpha:
      return MetalBlendFactor::kSourceAlpha;
    case xenos::BlendFactor::kOneMinusSrcColor:
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return MetalBlendFactor::kOneMinusSourceAlpha;
    case xenos::BlendFactor::kDstColor:
    case xenos::BlendFactor::kDstAlpha:
      return MetalBlendFactor::kDestinationAlpha;
    case xenos::BlendFactor::kOneMinusDstColor:
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return MetalBlendFactor::kOneMinusDestinationAlpha;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha:
      return MetalBlendFactor::kBlendAlpha;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return MetalBlendFactor::kOneMinusBlendAlpha;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return MetalBlendFactor::kSourceAlphaSaturated;
    default:
      return MetalBlendFactor::kZero;
  }
}

inline MetalBlendOperation GetMetalBlendOperation(xenos::BlendOp operation) {
  switch (operation) {
    case xenos::BlendOp::kAdd:
      return MetalBlendOperation::kAdd;
    case xenos::BlendOp::kSubtract:
      return MetalBlendOperation::kSubtract;
    case xenos::BlendOp::kMin:
      return MetalBlendOperation::kMin;
    case xenos::BlendOp::kMax:
      return MetalBlendOperation::kMax;
    case xenos::BlendOp::kRevSubtract:
      return MetalBlendOperation::kReverseSubtract;
    default:
      return MetalBlendOperation::kAdd;
  }
}

inline MetalColorAttachmentBlendState GetMetalColorAttachmentBlendState(
    uint32_t blend_control, uint32_t normalized_color_mask_nibble) {
  reg::RB_BLENDCONTROL control = {};
  control.value = blend_control;
  MetalColorAttachmentBlendState state = {
      normalized_color_mask_nibble & 0xFu,
      {GetMetalRgbBlendFactor(control.color_srcblend),
       GetMetalRgbBlendFactor(control.color_destblend),
       GetMetalBlendOperation(control.color_comb_fcn)},
      {GetMetalAlphaBlendFactor(control.alpha_srcblend),
       GetMetalAlphaBlendFactor(control.alpha_destblend),
       GetMetalBlendOperation(control.alpha_comb_fcn)},
      false,
  };
  state.blending_required =
      state.write_mask != 0 && (!state.rgb.is_opaque_replacement() ||
                                !state.alpha.is_opaque_replacement());
  return state;
}

// Pipeline-compatible sample state plus the dynamic shader constants used for
// Xenos alpha-to-mask. The translated pixel shader writes the exact guest
// coverage mask (including the per-pixel offsets), so Metal hardware
// alpha-to-coverage must remain disabled and alpha state must not fragment the
// pipeline cache. Only raster_sample_count is pipeline-bound.
struct MetalPipelineRasterKey {
  uint32_t raster_sample_count;

  bool operator==(const MetalPipelineRasterKey& other) const {
    return raster_sample_count == other.raster_sample_count;
  }
  bool operator!=(const MetalPipelineRasterKey& other) const {
    return !(*this == other);
  }
};

struct MetalPipelineRasterState {
  uint32_t raster_sample_count;
  uint32_t sample_count_log2_x;
  uint32_t sample_count_log2_y;
  uint32_t alpha_to_mask;

  MetalPipelineRasterKey pipeline_key() const { return {raster_sample_count}; }
};

// Guest-derived inputs consumed by Metal's translated-shader system constants.
// Keeping this seam free of Metal types gives the conformance target coverage
// of the exact shared primitive classification and typed register reads.
struct MetalSystemConstantGuestState {
  bool primitive_polygonal;
  bool depth_float24;
  uint32_t vertex_index_offset;
  uint32_t vertex_index_min;
  uint32_t vertex_index_max;
};

inline MetalSystemConstantGuestState GetMetalSystemConstantGuestState(
    const RegisterFile& regs) {
  return {
      ::rex::gpu::draw_state::IsPrimitivePolygonal(regs),
      regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
          xenos::DepthRenderTargetFormat::kD24FS8,
      regs.Get<reg::VGT_INDX_OFFSET>().indx_offset,
      regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx,
      regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx,
  };
}

// Metal's canonical depth transfer stores the full guest float24 [0, 2)
// domain in host [0, 1]. These arguments keep interpolated raster depth and
// explicit pixel-shader depth in that same domain.
// `scale_x`/`scale_y` map guest pixels to host pixels: the draw resolution
// scale times the scale of a fast-clear folded into the 1x view (see
// ::rex::graphics::metal::GetMetalSettings().metal_msaa_clear_fold.Get().h). The maximum host viewport extent is the guest
// maximum at the draw resolution scale alone, within Metal's 16384.
inline draw_util::GetViewportInfoArgs GetMetalViewportInfoArgs(
    const RegisterFile& regs,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    bool pixel_shader_writes_depth, uint32_t scale_x = 1,
    uint32_t scale_y = 1, uint32_t draw_resolution_scale_x = 1,
    uint32_t draw_resolution_scale_y = 1,
    bool window_offset_in_edram = false) {
  draw_util::GetViewportInfoArgs args;
  args.Setup(scale_x, scale_y, divisors::MagicDiv(scale_x),
             divisors::MagicDiv(scale_y),
             /*origin_bottom_left=*/true,
             std::min(xenos::kTexture2DCubeMaxWidthHeight *
                          draw_resolution_scale_x,
                      uint32_t(16384)),
             std::min(xenos::kTexture2DCubeMaxWidthHeight *
                          draw_resolution_scale_y,
                      uint32_t(16384)),
             /*allow_reverse_z=*/true, normalized_depth_control,
             /*convert_z_to_float24=*/false,
             /*full_float24_in_0_to_1=*/true, pixel_shader_writes_depth);
  args.SetupRegisterValues(regs, window_offset_in_edram);
  return args;
}

inline MetalPipelineRasterState GetMetalPipelineRasterState(
    uint32_t raster_sample_count, reg::RB_COLORCONTROL color_control) {
  // Render-target selection is expected to provide a Metal-supported count.
  // Fail closed to single-sample if a malformed value reaches this pure seam.
  if (raster_sample_count != 2u && raster_sample_count != 4u) {
    raster_sample_count = 1u;
  }
  uint32_t alpha_to_mask =
      color_control.alpha_to_mask_enable
          ? (color_control.value >> 24u) | (1u << 8u)
          : 0u;
  return {
      raster_sample_count,
      raster_sample_count == 4u ? 1u : 0u,
      raster_sample_count >= 2u ? 1u : 0u,
      alpha_to_mask,
  };
}

// The normalized color mask contains one RGBA nibble per Xenos render target.
// A draw writing only RT1-RT3 is still a color draw and must not fall into the
// attachment-less / memexport path.
inline bool HasMetalColorTarget(uint32_t normalized_color_mask) {
  constexpr uint32_t kAllColorTargetChannels =
      (1u << (4u * xenos::kMaxColorRenderTargets)) - 1u;
  return (normalized_color_mask & kAllColorTargetChannels) != 0;
}

// Returns one bit per render target whose pixel-shader output must be converted
// from linear to the Xenos 8:8:8:8 gamma encoding. Keeping this independent of
// RT0 mirrors the D3D12/Vulkan system-constant contract.
inline uint32_t GetMetalGammaRenderTargetMask(
    const std::array<reg::RB_COLOR_INFO, xenos::kMaxColorRenderTargets>&
        color_infos) {
  uint32_t gamma_mask = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (color_infos[i].color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      gamma_mask |= 1u << i;
    }
  }
  return gamma_mask;
}

// Xenos culling and faceness apply only to polygonal primitives. Lines,
// points, and rectangle-list geometry must establish safe no-cull state on
// every draw so a preceding triangle draw can't leak encoder state into them.
inline MetalCullFrontFaceState GetMetalCullFrontFaceState(
    reg::PA_SU_SC_MODE_CNTL mode, bool primitive_polygonal) {
  if (!primitive_polygonal) {
    return {MetalCullMode::kNone, false};
  }
  MetalCullMode cull_mode = MetalCullMode::kNone;
  if (mode.cull_front && mode.cull_back) {
    cull_mode = MetalCullMode::kAll;
  } else if (mode.cull_front) {
    cull_mode = MetalCullMode::kFront;
  } else if (mode.cull_back) {
    cull_mode = MetalCullMode::kBack;
  }
  return {cull_mode, mode.face != 0};
}

// Xenos dual polygon mode may select fill independently for front and back
// faces. Metal has one fill mode for the draw, so use the most conservative
// non-culled face: either points or lines becomes line fill. Reserved/disabled
// polygon modes and non-polygon primitives explicitly restore solid fill.
inline MetalTriangleFillMode GetMetalTriangleFillMode(
    reg::PA_SU_SC_MODE_CNTL mode, bool primitive_polygonal) {
  if (!primitive_polygonal ||
      mode.poly_mode != xenos::PolygonModeEnable::kDualMode) {
    return MetalTriangleFillMode::kFill;
  }
  xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
  if (!mode.cull_front) {
    polygon_type = std::min(polygon_type, mode.polymode_front_ptype);
  }
  if (!mode.cull_back) {
    polygon_type = std::min(polygon_type, mode.polymode_back_ptype);
  }
  return polygon_type == xenos::PolygonType::kTriangles
             ? MetalTriangleFillMode::kFill
             : MetalTriangleFillMode::kLines;
}

// Converts the shared preferred-face polygon offset to a complete Metal value.
// The caller obtains scale/offset through GetPreferredFacePolygonOffset so
// culling and the distinct non-polygon enable have one guest-semantics owner.
// The shared D3D10 conversion preserves useful guest depth separation for
// unorm24 and float24 host depth representations.
inline MetalFillDepthBiasState GetMetalFillDepthBiasState(
    reg::PA_SU_SC_MODE_CNTL mode, bool primitive_polygonal,
    xenos::DepthRenderTargetFormat depth_format, float polygon_offset_scale,
    float polygon_offset, uint32_t draw_resolution_scale = 1) {
  return {
      GetMetalTriangleFillMode(mode, primitive_polygonal),
      float(::rex::gpu::draw_state::GetD3D10IntegerPolygonOffset(
          depth_format, polygon_offset)),
      polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
          float(std::max(draw_resolution_scale, uint32_t(1))),
  };
}

// Metal supports separate front/back stencil references. Xenos back-face
// stencil state applies only to polygonal primitives when enabled in the
// normalized depth control; lines, points, and other non-polygons use the
// front reference even if the raw back-face-enable bit was set.
inline MetalBlendStencilState GetMetalBlendStencilState(
    float blend_color_red, float blend_color_green, float blend_color_blue,
    float blend_color_alpha,
    reg::RB_DEPTHCONTROL normalized_depth_control, bool primitive_polygonal,
    reg::RB_STENCILREFMASK stencil_ref_mask,
    reg::RB_STENCILREFMASK stencil_ref_mask_back) {
  bool stencil_enabled = normalized_depth_control.stencil_enable != 0;
  uint32_t stencil_reference_front =
      stencil_enabled ? stencil_ref_mask.stencilref : 0;
  uint32_t stencil_reference_back = stencil_reference_front;
  if (stencil_enabled && primitive_polygonal &&
      normalized_depth_control.backface_enable) {
    stencil_reference_back = stencil_ref_mask_back.stencilref;
  }
  return {blend_color_red,
          blend_color_green,
          blend_color_blue,
          blend_color_alpha,
          stencil_enabled,
          stencil_reference_front,
          stencil_reference_back};
}

// Intersects the shared guest-normalized scissor with the active Metal render
// extent. Widened end arithmetic is required because a malformed or extreme
// guest rectangle must collapse/clip, never wrap and expose unrelated pixels.
inline MetalScissorRect ClampScissorToRenderExtent(
    const ::rex::gpu::draw_state::Scissor& scissor, uint32_t render_width,
    uint32_t render_height) {
  uint64_t x_begin = std::min<uint64_t>(scissor.offset[0], render_width);
  uint64_t y_begin = std::min<uint64_t>(scissor.offset[1], render_height);
  uint64_t x_end =
      std::min<uint64_t>(uint64_t(scissor.offset[0]) + scissor.extent[0],
                         render_width);
  uint64_t y_end =
      std::min<uint64_t>(uint64_t(scissor.offset[1]) + scissor.extent[1],
                         render_height);
  x_end = std::max(x_end, x_begin);
  y_end = std::max(y_end, y_begin);
  return {uint32_t(x_begin), uint32_t(y_begin), uint32_t(x_end - x_begin),
          uint32_t(y_end - y_begin)};
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_FIXED_FUNCTION_STATE_H_
