/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * Metal-only draw helpers. Anything the Xenos GPU core itself provides comes
 * from gpu/xenos/draw_state.h; what remains here is what it cannot express: the
 * gathered viewport arguments (a fold or a clear is rendered into a view of a
 * different size), the user clip planes and point parameters the Metal system
 * constants carry, and the Metal resolve copy table.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/divisors.h>
#include <rex/math.h>  // ::rex::divisors::MagicDiv
#include <rex/memory.h>
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>

#include "gpu/xenos/draw_state.h"
#include "gpu/xenos/pipeline/shader/shader.h"
#include "gpu/xenos/xenos_settings.h"

namespace rex {
namespace graphics {
namespace metal {
namespace draw_util {

// The Xenos GPU core already defines these; the Metal backend uses its types rather than
// carrying a second copy that could drift.
using ViewportInfo = ::rex::gpu::draw_state::HostViewport;
using ResolveEdramInfo = ::rex::gpu::draw_state::ResolveEDRAMLayout;
using ResolveCoordinateInfo = ::rex::gpu::draw_state::ResolveCoordinates;
using ResolveCopyDestCoordinateInfo = ::rex::gpu::draw_state::ResolveCopyDestCoordinates;

// The Xenos core's scissor and sample-selection helpers, imported so the carried resolve
// code reads exactly as it did upstream.
using ::rex::gpu::draw_state::GetScissor;
using ::rex::gpu::draw_state::SanitizeCopySampleSelect;
using ::rex::gpu::draw_state::Scissor;

// Everything GetHostViewportInfo reads, gathered so a backend can cache the viewport per draw state instead of
// rebuilding it from the register file on every draw. Two consecutive draws with the same args produce the same
// viewport, which is what makes the comparison in the caches meaningful.
struct GetViewportInfoArgs {
  union alignas(64) {
    struct {
      // group 1
      uint32_t x_max;
      uint32_t y_max;
      union {
        struct {
          uint32_t origin_bottom_left : 1;
          uint32_t allow_reverse_z : 1;
          uint32_t convert_z_to_float24 : 1;
          uint32_t full_float24_in_0_to_1 : 1;
          uint32_t pixel_shader_writes_depth : 1;
          xenos::DepthRenderTargetFormat depth_format : 1;
        };
        uint32_t packed_portions;
      };
      reg::RB_DEPTHCONTROL normalized_depth_control;
      // group 2
      reg::PA_CL_CLIP_CNTL pa_cl_clip_cntl;
      reg::PA_CL_VTE_CNTL pa_cl_vte_cntl;
      reg::PA_SU_SC_MODE_CNTL pa_su_sc_mode_cntl;
      reg::PA_SU_VTX_CNTL pa_su_vtx_cntl;
      // group 3
      reg::PA_SC_WINDOW_OFFSET pa_sc_window_offset;
      float PA_CL_VPORT_XSCALE;
      float PA_CL_VPORT_YSCALE;
      float PA_CL_VPORT_ZSCALE;

      float PA_CL_VPORT_XOFFSET;
      float PA_CL_VPORT_YOFFSET;
      float PA_CL_VPORT_ZOFFSET;
      uint32_t padding_set_to_0;
    };
  };

  // everything that follows here does not need to be compared
  uint32_t draw_resolution_scale_x;
  uint32_t draw_resolution_scale_y;
  ::rex::divisors::MagicDiv draw_resolution_scale_x_divisor;
  ::rex::divisors::MagicDiv draw_resolution_scale_y_divisor;
  void Setup(uint32_t _draw_resolution_scale_x, uint32_t _draw_resolution_scale_y,
             ::rex::divisors::MagicDiv _draw_resolution_scale_x_divisor,
             ::rex::divisors::MagicDiv _draw_resolution_scale_y_divisor, bool _origin_bottom_left, uint32_t _x_max,
             uint32_t _y_max, bool _allow_reverse_z, reg::RB_DEPTHCONTROL _normalized_depth_control,
             bool _convert_z_to_float24, bool _full_float24_in_0_to_1, bool _pixel_shader_writes_depth) {
    packed_portions = 0;
    padding_set_to_0 = 0;  // important to zero this
    draw_resolution_scale_x = _draw_resolution_scale_x;
    draw_resolution_scale_y = _draw_resolution_scale_y;
    draw_resolution_scale_x_divisor = _draw_resolution_scale_x_divisor;
    draw_resolution_scale_y_divisor = _draw_resolution_scale_y_divisor;
    origin_bottom_left = _origin_bottom_left;
    x_max = _x_max;
    y_max = _y_max;
    allow_reverse_z = _allow_reverse_z;
    normalized_depth_control = _normalized_depth_control;
    convert_z_to_float24 = _convert_z_to_float24;
    full_float24_in_0_to_1 = _full_float24_in_0_to_1;
    pixel_shader_writes_depth = _pixel_shader_writes_depth;
  }

  void SetupRegisterValues(const RegisterFile& regs, bool window_offset_in_edram) {
    pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
    pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    pa_su_vtx_cntl = regs.Get<reg::PA_SU_VTX_CNTL>();
    PA_CL_VPORT_XSCALE = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
    PA_CL_VPORT_YSCALE = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
    PA_CL_VPORT_ZSCALE = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
    PA_CL_VPORT_XOFFSET = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
    PA_CL_VPORT_YOFFSET = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    PA_CL_VPORT_ZOFFSET = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
    pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    if (window_offset_in_edram) {
      pa_sc_window_offset.value = 0;
    }
    depth_format = regs.Get<reg::RB_DEPTH_INFO>().depth_format;
  }
  // Field-by-field comparison. Upstream compares four packed 128-bit groups at once on x86; this SDK builds for arm64
  // too, and the scalar form is what runs there, so there is one implementation rather than two.
  bool operator==(const GetViewportInfoArgs& prev) const {
    bool result = true;
    auto accum_eq = [&result](auto x, auto y) { result &= (x == y); };

#define EQC(field) accum_eq(field, prev.field)
    EQC(x_max);
    EQC(y_max);
    EQC(packed_portions);
    EQC(normalized_depth_control.value);
    EQC(pa_cl_clip_cntl.value);
    EQC(pa_cl_vte_cntl.value);

    EQC(pa_su_sc_mode_cntl.value);
    EQC(pa_su_vtx_cntl.value);
    EQC(PA_CL_VPORT_XSCALE);
    EQC(PA_CL_VPORT_YSCALE);
    EQC(PA_CL_VPORT_ZSCALE);
    EQC(PA_CL_VPORT_XOFFSET);
    EQC(PA_CL_VPORT_YOFFSET);
    EQC(PA_CL_VPORT_ZOFFSET);
    EQC(pa_sc_window_offset.value);
#undef EQC
    return result;
  }
};

// Same as the register-file form below, but from a gathered GetViewportInfoArgs so a cached viewport can be rebuilt
// without touching the register file.

// Same as the register-file form, but from a gathered GetViewportInfoArgs so a cached viewport can be rebuilt
// without touching the register file. The settings are threaded through rather than read as globals, the way the
// Xenos core's own draw_state functions take them.
void GetHostViewportInfo(GetViewportInfoArgs* args, const ::rex::gpu::XenosSettings& settings,
                         ViewportInfo& viewport_info_out);

struct NormalizedUserClipPlanes {
  uint32_t count;
  float equations[6][4];
};

// The translated vertex shader only knows how many user clip planes are
// enabled, not their guest indices. Pack the enabled equations in increasing
// index order so this array matches the shader modification's compact layout.
inline NormalizedUserClipPlanes GetNormalizedUserClipPlanes(
    const RegisterFile& regs) {
  NormalizedUserClipPlanes result = {};
  reg::PA_CL_CLIP_CNTL clip_control = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (clip_control.clip_disable) {
    return result;
  }
  for (uint32_t plane_index = 0; plane_index < 6; ++plane_index) {
    if (!(clip_control.ucp_ena & (uint32_t(1) << plane_index))) {
      continue;
    }
    for (uint32_t component = 0; component < 4; ++component) {
      result.equations[result.count][component] = regs.Get<float>(
          XE_GPU_REG_PA_CL_UCP_0_X + plane_index * 4 + component);
    }
    ++result.count;
  }
  return result;
}

// Direct3D 9 and Xenos constant polygon offset is an absolute floating-point
// value.
// It's possibly treated just as an absolute offset by the Xenos too - the

struct NormalizedPointParameters {
  float vertex_diameter_min;
  float vertex_diameter_max;
  float constant_diameter[2];
  float screen_diameter_to_ndc_radius[2];
};

// Xenos point size registers contain a 12.4 fixed-point radius. Translated
// shaders and the shared point-list geometry shader consume diameters.
inline NormalizedPointParameters GetNormalizedPointParameters(
    const RegisterFile& regs, const ViewportInfo& viewport_info,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y) {
  NormalizedPointParameters result = {};
  if (regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type !=
      xenos::PrimitiveType::kPointList) {
    return result;
  }

  reg::PA_SU_POINT_MINMAX point_minmax =
      regs.Get<reg::PA_SU_POINT_MINMAX>();
  reg::PA_SU_POINT_SIZE point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
  constexpr float kPointRadiusFixedToDiameter = 2.0f / 16.0f;
  result.vertex_diameter_min =
      float(point_minmax.min_size) * kPointRadiusFixedToDiameter;
  result.vertex_diameter_max =
      float(point_minmax.max_size) * kPointRadiusFixedToDiameter;
  result.constant_diameter[0] =
      float(point_size.width) * kPointRadiusFixedToDiameter;
  result.constant_diameter[1] =
      float(point_size.height) * kPointRadiusFixedToDiameter;

  // One NDC unit is half the viewport axis, while the point value is a
  // diameter, so the factor of two and the diameter-to-radius factor cancel.
  uint32_t extent_x = viewport_info.xy_extent[0]
                          ? viewport_info.xy_extent[0]
                          : uint32_t(1);
  uint32_t extent_y = viewport_info.xy_extent[1]
                          ? viewport_info.xy_extent[1]
                          : uint32_t(1);
  result.screen_diameter_to_ndc_radius[0] =
      float(draw_resolution_scale_x) / float(extent_x);
  result.screen_diameter_to_ndc_radius[1] =
      float(draw_resolution_scale_y) / float(extent_y);
  return result;
}


// Converts the guest viewport (or fakes one if drawing without a viewport) to
// a viewport, plus values to multiply-add the returned position by, usable on
// host graphics APIs such as Direct3D 11+ and Vulkan, also forcing it to the
// Direct3D clip space with 0...W Z rather than -W...W.
void GetHostViewportInfo(const RegisterFile& regs, uint32_t draw_resolution_scale_x,
                         uint32_t draw_resolution_scale_y, bool origin_bottom_left, uint32_t x_max,
                         uint32_t y_max, bool allow_reverse_z,
                         reg::RB_DEPTHCONTROL normalized_depth_control, bool convert_z_to_float24,
                         bool full_float24_in_0_to_1, bool pixel_shader_writes_depth,
                         ViewportInfo& viewport_info_out);


// For backends with Shader Model 5-like compute, host shaders to use to perform
// copying in resolve operations. The variants shared with the Xenos core keep its
// order so the two enums agree value for value; kDXT3AAs1111 is Metal-only and is
// therefore the last value, which is why this enum lives in this backend.
enum class ResolveCopyShaderIndex {
  kFast32bpp1x2xMSAA,
  kFast32bpp4xMSAA,
  kFast64bpp1x2xMSAA,
  kFast64bpp4xMSAA,

  kFull8bpp,
  kFull16bpp,
  kFull32bpp,
  kFull64bpp,
  kFull128bpp,

  // xenos::CopyCommand::kConstantOne (compare to k_DXT3A_AS_1_1_1_1) to 1_1_1_1.
  kDXT3AAs1111,

  kCount,
  kUnknown = kCount,
};

// Only the Metal thread group sizing; the variant choice and the shared constants
// come from draw_state::ResolveRequest::GetCopyShader.
struct ResolveCopyShaderInfo {
  // Debug name of the pipeline state object with this shader.
  const char* debug_name;
  // Log2 of number of pixels in a single thread group along X and Y.
  uint32_t group_size_x_log2, group_size_y_log2;
};

extern const ResolveCopyShaderInfo resolve_copy_shader_info[size_t(ResolveCopyShaderIndex::kCount)];

// The Metal resolve shaders read one 32-byte push-constant block whatever the variant, so the
// DXT3A conversion operands sit after the shared part rather than in a variant of their own.
struct ResolveCopyShaderConstants : ::rex::gpu::draw_state::ResolveCopyShaderConstants {
  // Read only by the kDXT3AAs1111 resolve (RB_COPY_FUNC, RB_COPY_REF, RB_COPY_MASK), which converts
  // to 1.0 in every channel; the other resolve shaders ignore them.
  uint32_t copy_func;
  uint32_t copy_ref;
  uint32_t copy_mask;
};
static_assert(sizeof(ResolveCopyShaderConstants) == 32,
              "MSL push-const ABI: every resolve variant shares this block");

using ResolveClearShaderConstants = ::rex::gpu::draw_state::ResolveClearShaderConstants;

// The resolve request is the Xenos core's own; only the copy shader selection is Metal's,
// because the table it indexes is Metal's pipeline set.
struct ResolveInfo : ::rex::gpu::draw_state::ResolveRequest {
  // The DXT3A as 1_1_1_1 copy compares each component against these; the shared request has no
  // fields for them because no other backend runs that shader.
  uint32_t rb_copy_func = 0;
  uint32_t rb_copy_ref = 0;
  uint32_t rb_copy_mask = 0;

  bool IsColorRawCopySafe() const {
    return !copy_dest_info.copy_dest_exp_bias &&
           xenos::IsSingleCopySampleSelected(copy_dest_coordinate_info.copy_sample_select) &&
           xenos::IsColorResolveFormatBitwiseEquivalent(
               xenos::ColorRenderTargetFormat(color_edram_info.format),
               xenos::ColorFormat(copy_dest_info.copy_dest_format));
  }

  ResolveCopyShaderIndex GetCopyShader(uint32_t draw_resolution_scale_x,
                                       uint32_t draw_resolution_scale_y,
                                       ResolveCopyShaderConstants& constants_out,
                                       uint32_t& group_count_x_out,
                                       uint32_t& group_count_y_out) const;
};

// Reads the resolve from the registers. Carried rather than calling the Xenos core's
// draw_state::GetResolveInfo because that one rejects the DXT3A-as-1_1_1_1 copy command
// outright, which would drop the clears that accompany it.
bool GetResolveInfo(const ::rex::gpu::XenosSettings& settings, const RegisterFile& regs,
                    ::rex::IPhysicalMemory& memory, uint32_t draw_resolution_scale_x,
                    uint32_t draw_resolution_scale_y, bool fixed_rg16_truncated_to_minus_1_to_1,
                    bool fixed_rgba16_truncated_to_minus_1_to_1, ResolveInfo& info_out);

}  // namespace draw_util
}  // namespace metal
}  // namespace graphics
}  // namespace rex
