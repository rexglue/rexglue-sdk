/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_MSAA_CLEAR_FOLD_H_
#define XENIA_GPU_METAL_METAL_MSAA_CLEAR_FOLD_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/xenos/register.h>
#include "rex/graphics/shader_interpreter.h"
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// MSAA fast-clear fold (openspec fold-metal-msaa-fast-clears). The 360 D3D
// runtime clears a 2x/4x surface with a screen-space rectangle drawn at that
// MSAA level. An EDRAM tile holds the same words at every MSAA level: with
// the MSAA shifts (sx, sy) = (1, 1) for 4x and (0, 1) for 2x, guest sample s
// of MSAA pixel (x, y) is the word of 1x pixel
// ((x << sx) + (((s >> 1) & 1) if sx), (y << sy) + (s & 1)) per
// xe_edram_offset_bytes. Such a clear can therefore be drawn through the 1x
// view of the same base, format and tile pitch with screen coordinates scaled
// by (1 << sx, 1 << sy) instead of transferring the tiles into the MSAA view
// and back. This header is the pure, device-free
// qualification; anything it refuses takes the ordinary transfer path.

// The runtime clear pair found on every 1x<->4x flip of Gears 3 and JC2
// (phase 0, xenia-mac-evidence/msaa-clear-phase0-20260930/): VS passes the
// fetched position and colour through, PS outputs the interpolated colour.
constexpr uint64_t kMsaaClearFoldVertexShader = 0x1E6883FCCDE1F688ull;
constexpr uint64_t kMsaaClearFoldPixelShader = 0xA4A965C189287B99ull;
// The same pass-through pair in Iron Man (534507F2): VS fetches position and
// colour, PS writes oC0 = interpolator 0 (ucode dumped 2026-11-03).
constexpr uint64_t kMsaaClearFoldVertexShaderB = 0x0A6D1DD7767FDF27ull;
constexpr uint64_t kMsaaClearFoldPixelShaderB = 0x2E372EA28CC404B7ull;

enum class MsaaClearFoldRefusal : uint32_t {
  kNone,
  kShaderPair,
  kPrimitive,
  kSingleSample,
  kViewportTransform,
  kEdramMode,
  kDepthFunction,
  kStencil,
  kBlending,
  kAlphaTest,
  kPitch,
  kVertexEvaluation,
  kHomogeneousW,
  kDepthVaries,
  kColourVaries,
  kEdgeAlignment,
  kDepthRange,
  kColourFormat,
  // Depth clear into the colour view (QualifyMsaaDepthClearColourFold).
  kColourAlias,
  kWordMismatch,
  kCount,
};

inline const char* MsaaClearFoldRefusalName(MsaaClearFoldRefusal refusal) {
  switch (refusal) {
    case MsaaClearFoldRefusal::kNone:
      return "folded";
    case MsaaClearFoldRefusal::kShaderPair:
      return "shader-pair";
    case MsaaClearFoldRefusal::kPrimitive:
      return "primitive";
    case MsaaClearFoldRefusal::kSingleSample:
      return "single-sample";
    case MsaaClearFoldRefusal::kViewportTransform:
      return "viewport-transform";
    case MsaaClearFoldRefusal::kEdramMode:
      return "edram-mode";
    case MsaaClearFoldRefusal::kDepthFunction:
      return "depth-function";
    case MsaaClearFoldRefusal::kStencil:
      return "stencil";
    case MsaaClearFoldRefusal::kBlending:
      return "blending";
    case MsaaClearFoldRefusal::kAlphaTest:
      return "alpha-test";
    case MsaaClearFoldRefusal::kPitch:
      return "pitch";
    case MsaaClearFoldRefusal::kVertexEvaluation:
      return "vertex-evaluation";
    case MsaaClearFoldRefusal::kHomogeneousW:
      return "homogeneous-w";
    case MsaaClearFoldRefusal::kDepthVaries:
      return "depth-varies";
    case MsaaClearFoldRefusal::kColourVaries:
      return "colour-varies";
    case MsaaClearFoldRefusal::kEdgeAlignment:
      return "edge-alignment";
    case MsaaClearFoldRefusal::kDepthRange:
      return "depth-range";
    case MsaaClearFoldRefusal::kColourFormat:
      return "colour-format";
    case MsaaClearFoldRefusal::kColourAlias:
      return "colour-alias";
    case MsaaClearFoldRefusal::kWordMismatch:
      return "word-mismatch";
    case MsaaClearFoldRefusal::kCount:
      break;
  }
  return "?";
}

struct MsaaClearFoldDecision {
  MsaaClearFoldRefusal refusal = MsaaClearFoldRefusal::kShaderPair;
  // Log2 of the screen-coordinate scale into the 1x view.
  uint32_t scale_x_log2 = 0;
  uint32_t scale_y_log2 = 0;
  // RB_SURFACE_INFO presenting the draw as 1x over the same tiles.
  uint32_t surface_info_1x = 0;
  bool fold() const { return refusal == MsaaClearFoldRefusal::kNone; }
};

inline bool MsaaClearFoldStencilOpKeepsConstant(xenos::StencilOp op) {
  return op == xenos::StencilOp::kKeep || op == xenos::StencilOp::kReplace;
}

// Colour formats whose host attachment holds exactly the guest EDRAM encoding,
// so the canonical pack/materialize round trip of a cleared value is the
// identity and a clear written straight into the 1x view matches the transfer
// path. Gamma and the float-encoded formats keep more host precision than the
// guest encoding (the round trip quantizes), so they do not fold.
inline bool MsaaClearFoldColourFormatIsExact(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

inline bool MsaaClearFoldBlendIsReplace(reg::RB_BLENDCONTROL blend) {
  return blend.color_srcblend == xenos::BlendFactor::kOne &&
         blend.color_destblend == xenos::BlendFactor::kZero &&
         blend.color_comb_fcn == xenos::BlendOp::kAdd &&
         blend.alpha_srcblend == xenos::BlendFactor::kOne &&
         blend.alpha_destblend == xenos::BlendFactor::kZero &&
         blend.alpha_comb_fcn == xenos::BlendOp::kAdd;
}

// `normalized_depth_control` and `normalized_color_mask` are the draw's
// draw_util-normalized values (depth/stencil disabled outside EDRAM modes
// that use them; colour mask already folded with the shader's writes).
inline MsaaClearFoldDecision QualifyMsaaClearFold(
    const RegisterFile& regs, uint64_t vertex_shader_hash,
    uint64_t pixel_shader_hash, xenos::PrimitiveType primitive_type,
    uint32_t index_count, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  MsaaClearFoldDecision decision;
  if (vertex_shader_hash != kMsaaClearFoldVertexShader ||
      pixel_shader_hash != kMsaaClearFoldPixelShader) {
    decision.refusal = MsaaClearFoldRefusal::kShaderPair;
    return decision;
  }
  if (primitive_type != xenos::PrimitiveType::kRectangleList ||
      index_count != 3) {
    decision.refusal = MsaaClearFoldRefusal::kPrimitive;
    return decision;
  }
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (surface_info.msaa_samples == xenos::MSAASamples::k1X ||
      surface_info.msaa_samples > xenos::MSAASamples::k4X) {
    decision.refusal = MsaaClearFoldRefusal::kSingleSample;
    return decision;
  }
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (vte.vport_x_scale_ena || vte.vport_x_offset_ena ||
      vte.vport_y_scale_ena || vte.vport_y_offset_ena) {
    decision.refusal = MsaaClearFoldRefusal::kViewportTransform;
    return decision;
  }
  xenos::EDRAMMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EDRAMMode::kColorDepth &&
      edram_mode != xenos::EDRAMMode::kDepthOnly) {
    decision.refusal = MsaaClearFoldRefusal::kEdramMode;
    return decision;
  }
  // A depth test against existing content would read the destination.
  if (normalized_depth_control.z_enable &&
      normalized_depth_control.zfunc != xenos::CompareFunction::kAlways) {
    decision.refusal = MsaaClearFoldRefusal::kDepthFunction;
    return decision;
  }
  if (normalized_depth_control.stencil_enable) {
    bool front_ok =
        normalized_depth_control.stencilfunc ==
            xenos::CompareFunction::kAlways &&
        MsaaClearFoldStencilOpKeepsConstant(
            normalized_depth_control.stencilfail) &&
        MsaaClearFoldStencilOpKeepsConstant(
            normalized_depth_control.stencilzpass) &&
        MsaaClearFoldStencilOpKeepsConstant(
            normalized_depth_control.stencilzfail);
    bool back_ok =
        !normalized_depth_control.backface_enable ||
        (normalized_depth_control.stencilfunc_bf ==
             xenos::CompareFunction::kAlways &&
         MsaaClearFoldStencilOpKeepsConstant(
             normalized_depth_control.stencilfail_bf) &&
         MsaaClearFoldStencilOpKeepsConstant(
             normalized_depth_control.stencilzpass_bf) &&
         MsaaClearFoldStencilOpKeepsConstant(
             normalized_depth_control.stencilzfail_bf));
    if (!front_ok || !back_ok) {
      decision.refusal = MsaaClearFoldRefusal::kStencil;
      return decision;
    }
  }
  if (normalized_color_mask) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!((normalized_color_mask >> (i * 4)) & 0xF)) {
        continue;
      }
      if (!MsaaClearFoldBlendIsReplace(regs.Get<reg::RB_BLENDCONTROL>(
              xenos::compat::BlendControlRegisterIndex(i)))) {
        decision.refusal = MsaaClearFoldRefusal::kBlending;
        return decision;
      }
      if (!MsaaClearFoldColourFormatIsExact(
              regs.Get<reg::RB_COLOR_INFO>(
                      xenos::compat::ColorInfoRegisterIndex(i))
                  .color_format)) {
        decision.refusal = MsaaClearFoldRefusal::kColourFormat;
        return decision;
      }
    }
  }
  auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  if ((color_control.alpha_test_enable &&
       color_control.alpha_func != xenos::CompareFunction::kAlways) ||
      color_control.alpha_to_mask_enable) {
    decision.refusal = MsaaClearFoldRefusal::kAlphaTest;
    return decision;
  }
  // 4x doubles both axes; 2x doubles Y only (xe_edram_msaa_shifts).
  uint32_t x_log2 =
      surface_info.msaa_samples >= xenos::MSAASamples::k4X ? 1u : 0u;
  uint32_t y_log2 = 1u;
  uint32_t pitch_1x = uint32_t(surface_info.surface_pitch) << x_log2;
  if (!surface_info.surface_pitch ||
      pitch_1x > xenos::kTexture2DCubeMaxWidthHeight) {
    decision.refusal = MsaaClearFoldRefusal::kPitch;
    return decision;
  }
  reg::RB_SURFACE_INFO surface_info_1x = surface_info;
  surface_info_1x.msaa_samples = xenos::MSAASamples::k1X;
  surface_info_1x.surface_pitch = pitch_1x;
  decision.refusal = MsaaClearFoldRefusal::kNone;
  decision.scale_x_log2 = x_log2;
  decision.scale_y_log2 = y_log2;
  decision.surface_info_1x = surface_info_1x.value;
  return decision;
}

// Depth fast clear into the colour view. Iron Man's (534507F2) runtime Clear
// of a 1280x720 k_8_8_8_8 colour surface clears its tile-aligned blocks as
// 4x depth-only rectangles over the colour surface's own EDRAM base (depth
// ALWAYS + write, stencil REPLACE, the colour encoded in depth and stencil)
// and the remaining strips as 1x colour rectangles, interleaved: ~65 pairs a
// clear, and every rectangle flipped the tiles between the 4x D24S8 view and
// the 1x colour view (pack + materialize each way, 38-44 fps menus on the M1
// Max). Each depth rectangle writes one constant word to every sample it
// covers; when that word equals the k_8_8_8_8 encoding of the constant
// colour the pass-through pixel shader outputs (the runtime picks the depth
// so that it does), the rectangle is drawn as a 1x colour clear of the same
// tiles instead (RB_SURFACE_INFO as for the fold, colour target 0 enabled,
// depth and stencil off): the same canonical words, no ownership change. The
// word comparison is the caller's (it needs the evaluated vertices); this is
// the register-only qualification. The colour target must already be the
// game's: RB_COLOR_INFO 0 at the depth base, k_8_8_8_8 (exact host encoding).
inline MsaaClearFoldDecision QualifyMsaaDepthClearColourFold(
    const RegisterFile& regs, uint64_t vertex_shader_hash,
    uint64_t pixel_shader_hash, xenos::PrimitiveType primitive_type,
    uint32_t index_count, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  MsaaClearFoldDecision decision;
  if (!((vertex_shader_hash == kMsaaClearFoldVertexShader &&
         pixel_shader_hash == kMsaaClearFoldPixelShader) ||
        (vertex_shader_hash == kMsaaClearFoldVertexShaderB &&
         pixel_shader_hash == kMsaaClearFoldPixelShaderB))) {
    decision.refusal = MsaaClearFoldRefusal::kShaderPair;
    return decision;
  }
  if (primitive_type != xenos::PrimitiveType::kRectangleList ||
      index_count != 3) {
    decision.refusal = MsaaClearFoldRefusal::kPrimitive;
    return decision;
  }
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (surface_info.msaa_samples == xenos::MSAASamples::k1X ||
      surface_info.msaa_samples > xenos::MSAASamples::k4X) {
    decision.refusal = MsaaClearFoldRefusal::kSingleSample;
    return decision;
  }
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (vte.vport_x_scale_ena || vte.vport_x_offset_ena ||
      vte.vport_y_scale_ena || vte.vport_y_offset_ena) {
    decision.refusal = MsaaClearFoldRefusal::kViewportTransform;
    return decision;
  }
  // Depth only: no colour written by the draw itself.
  xenos::EDRAMMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if ((edram_mode != xenos::EDRAMMode::kColorDepth &&
       edram_mode != xenos::EDRAMMode::kDepthOnly) ||
      normalized_color_mask) {
    decision.refusal = MsaaClearFoldRefusal::kEdramMode;
    return decision;
  }
  // Every covered sample gets the same complete (d24 << 8) | stencil word.
  auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (!normalized_depth_control.z_enable ||
      !normalized_depth_control.z_write_enable ||
      normalized_depth_control.zfunc != xenos::CompareFunction::kAlways ||
      depth_info.depth_format != xenos::DepthRenderTargetFormat::kD24S8) {
    decision.refusal = MsaaClearFoldRefusal::kDepthFunction;
    return decision;
  }
  if (!normalized_depth_control.stencil_enable ||
      normalized_depth_control.backface_enable ||
      normalized_depth_control.stencilfunc !=
          xenos::CompareFunction::kAlways ||
      normalized_depth_control.stencilzpass != xenos::StencilOp::kReplace ||
      regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask != 0xFF) {
    decision.refusal = MsaaClearFoldRefusal::kStencil;
    return decision;
  }
  auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  if ((color_control.alpha_test_enable &&
       color_control.alpha_func != xenos::CompareFunction::kAlways) ||
      color_control.alpha_to_mask_enable) {
    decision.refusal = MsaaClearFoldRefusal::kAlphaTest;
    return decision;
  }
  // An exponent bias would scale the pixel shader's colour on the way in.
  auto color_info = regs.Get<reg::RB_COLOR_INFO>();
  if (color_info.color_base != depth_info.depth_base ||
      color_info.color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      color_info.color_exp_bias) {
    decision.refusal = MsaaClearFoldRefusal::kColourAlias;
    return decision;
  }
  uint32_t x_log2 =
      surface_info.msaa_samples >= xenos::MSAASamples::k4X ? 1u : 0u;
  uint32_t pitch_1x = uint32_t(surface_info.surface_pitch) << x_log2;
  if (!surface_info.surface_pitch ||
      pitch_1x > xenos::kTexture2DCubeMaxWidthHeight) {
    decision.refusal = MsaaClearFoldRefusal::kPitch;
    return decision;
  }
  reg::RB_SURFACE_INFO surface_info_1x = surface_info;
  surface_info_1x.msaa_samples = xenos::MSAASamples::k1X;
  surface_info_1x.surface_pitch = pitch_1x;
  decision.refusal = MsaaClearFoldRefusal::kNone;
  decision.scale_x_log2 = x_log2;
  decision.scale_y_log2 = 1u;
  decision.surface_info_1x = surface_info_1x.value;
  return decision;
}

// The canonical word a depth view holding `host_depth` packs for kD24S8
// with stencil `stencil` (CPU mirror of xe_f32_to_unorm24 and
// xe_pack_depth_stencil, metal_edram_msl.h).
inline uint32_t MsaaClearFoldD24S8Word(float host_depth, uint32_t stencil) {
  float clamped = std::min(std::max(host_depth, 0.0f), 1.0f);
  uint32_t t = std::min(uint32_t(std::nearbyint(clamped * 16777216.0f)),
                        uint32_t(16777216u));
  uint32_t n24 = (t > 8388608u ? t - 1u : t) & 0xFFFFFFu;
  return (n24 << 8) | (stencil & 0xFFu);
}

// The k_8_8_8_8 word (R in the low byte) of a constant colour, when every
// channel is an exact multiple of 1/255 in [0, 1] (so no rounding rule is
// involved); false otherwise.
inline bool MsaaClearFoldColour8888Word(const float colour[4],
                                        uint32_t& word_out) {
  uint32_t word = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    float scaled = colour[i] * 255.0f;
    if (!(scaled >= 0.0f && scaled <= 255.0f) ||
        scaled != std::floor(scaled)) {
      return false;
    }
    word |= uint32_t(scaled) << (8 * i);
  }
  word_out = word;
  return true;
}

// Post-vertex-shader values of the clear rectangle's three vertices, from the
// shared ShaderInterpreter (the same CPU execution DrawExtentEstimator uses
// for screen-space draws).
struct MsaaClearFoldVertices {
  float position[3][4] = {};
  float interpolator0[3][4] = {};
  bool has_interpolator0 = false;
};

class MsaaClearFoldExportSink : public ::rex::gpu::ShaderInterpreter::ExportSink {
 public:
  void Reset() {
    has_position_ = false;
    has_interpolator0_ = false;
  }
  void Export(xenos::ucode::ExportRegister export_register, const float* value,
              uint32_t value_mask) override {
    float* target = nullptr;
    if (export_register == xenos::ucode::ExportRegister::kVSPosition) {
      target = position_;
      has_position_ = true;
    } else if (export_register == xenos::ucode::ExportRegister::kVSInterpolator0) {
      target = interpolator0_;
      has_interpolator0_ = true;
    } else {
      return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (value_mask & (1u << i)) {
        target[i] = value[i];
      }
    }
  }
  bool has_position_ = false;
  bool has_interpolator0_ = false;
  float position_[4] = {};
  float interpolator0_[4] = {};
};

// Runs the clear's vertex shader for its three auto-generated indices.
// Returns false (do not fold) if the draw is index-buffer sourced, the shader
// cannot be interpreted, or a vertex exports no position.
inline bool EvaluateMsaaClearFoldVertices(const RegisterFile& regs,
                                          ::rex::gpu::ShaderInterpreter& interpreter,
                                          const ::rex::gpu::Shader& vertex_shader,
                                          MsaaClearFoldVertices& out) {
  if (!::rex::gpu::ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return false;
  }
  auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return false;
  }
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  interpreter.SetShader(vertex_shader);
  MsaaClearFoldExportSink sink;
  interpreter.SetExportSink(&sink);
  bool ok = true;
  out.has_interpolator0 = true;
  for (uint32_t i = 0; i < 3 && ok; ++i) {
    uint32_t vertex_index =
        std::min(max_index, std::max(min_index, (i + index_offset) & 0xFFFFFF));
    sink.Reset();
    interpreter.GetTempRegisters()[0] = float(vertex_index);
    interpreter.Execute();
    if (!sink.has_position_) {
      ok = false;
      break;
    }
    std::memcpy(out.position[i], sink.position_, sizeof(sink.position_));
    std::memcpy(out.interpolator0[i], sink.interpolator0_,
                sizeof(sink.interpolator0_));
    out.has_interpolator0 &= sink.has_interpolator0_;
  }
  interpreter.SetExportSink(nullptr);
  return ok;
}

enum class MsaaClearFoldValueRefusal : uint32_t {
  kNone,
  kVertexEvaluation,
  kHomogeneousW,
  kDepthVaries,
  kColourVaries,
  kEdgeAlignment,
};

// Value invariance and pixel alignment of the evaluated rectangle (design D3):
// every vertex has w == 1 (or the vertex format already divides), the same z
// and the same interpolated colour (when colour is written), and every edge
// lands on a guest pixel boundary after the window and half-pixel offsets, so
// the 2x2-scaled host footprint covers exactly the guest samples the MSAA draw
// covers regardless of the host sample pattern.
inline MsaaClearFoldValueRefusal CheckMsaaClearFoldValues(
    const RegisterFile& regs, const MsaaClearFoldVertices& v,
    bool colour_written, bool half_pixel_offset_enabled) {
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  for (uint32_t i = 0; i < 3; ++i) {
    if (!vte.vtx_xy_fmt && v.position[i][3] != 1.0f) {
      return MsaaClearFoldValueRefusal::kHomogeneousW;
    }
  }
  for (uint32_t i = 1; i < 3; ++i) {
    if (std::memcmp(&v.position[i][2], &v.position[0][2], sizeof(float))) {
      return MsaaClearFoldValueRefusal::kDepthVaries;
    }
    if (colour_written &&
        (!v.has_interpolator0 ||
         std::memcmp(v.interpolator0[i], v.interpolator0[0],
                     sizeof(v.interpolator0[0])))) {
      return MsaaClearFoldValueRefusal::kColourVaries;
    }
  }
  float offset_x = 0.0f, offset_y = 0.0f;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset_x += float(window_offset.window_x_offset);
    offset_y += float(window_offset.window_y_offset);
  }
  if (half_pixel_offset_enabled &&
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
          xenos::PixelCenter::kD3DZero) {
    offset_x += 0.5f;
    offset_y += 0.5f;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    float x = v.position[i][0] + offset_x;
    float y = v.position[i][1] + offset_y;
    if (x != std::floor(x) || y != std::floor(y)) {
      return MsaaClearFoldValueRefusal::kEdgeAlignment;
    }
  }
  return MsaaClearFoldValueRefusal::kNone;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_MSAA_CLEAR_FOLD_H_
