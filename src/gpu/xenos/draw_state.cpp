/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cmath>

#include <rex/assert.h>
#include <rex/cvar.h>
#include "gpu/xenos/pipeline/texture/cache.h"
#include <rex/xenos/texture_info.h>
#include <rex/xenos/texture_layout.h>
#include <rex/xenos/registers.h>
#include "gpu/xenos/draw_state.h"
#include <rex/xenos/xenos.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/memory.h>
#include "gpu/xenos/host/fixed_point.h"

#include "gpu/xenos/cvar_table_xenos.h"

namespace rex::gpu::draw_state {

bool IsRasterizationPotentiallyDone(const xenos::RegisterFile& regs, bool primitive_polygonal) {
  xenos::EDRAMMode edram_mode = regs.Get<xenos::reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EDRAMMode::kColorDepth && edram_mode != xenos::EDRAMMode::kDepthOnly) {
    return false;
  }
  if (regs.Get<xenos::reg::SQ_PROGRAM_CNTL>().vs_export_mode ==
          xenos::VertexShaderExportMode::kMultipass ||
      !regs.Get<xenos::reg::RB_SURFACE_INFO>().surface_pitch) {
    return false;
  }
  if (primitive_polygonal) {
    auto pa_su_sc_mode_cntl = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>();
    if (pa_su_sc_mode_cntl.cull_front && pa_su_sc_mode_cntl.cull_back) {
      return false;
    }
  }
  return true;
}

bool IsDepthTestNeverPassing(const xenos::RegisterFile& regs) {
  xenos::EDRAMMode edram_mode = regs.Get<xenos::reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EDRAMMode::kColorDepth && edram_mode != xenos::EDRAMMode::kDepthOnly) {
    return false;
  }
  auto depthcontrol = regs.Get<xenos::reg::RB_DEPTHCONTROL>();
  return depthcontrol.z_enable && depthcontrol.zfunc == xenos::CompareFunction::kNever &&
         !depthcontrol.stencil_enable;
}

bool IsRasterizationDone(const xenos::RegisterFile& regs, bool primitive_polygonal,
                         bool pixel_shader_memexports) {
  return IsRasterizationPotentiallyDone(regs, primitive_polygonal) &&
         (pixel_shader_memexports || !IsDepthTestNeverPassing(regs));
}

xenos::reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const xenos::RegisterFile& regs) {
  xenos::EDRAMMode edram_mode = regs.Get<xenos::reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EDRAMMode::kColorDepth && edram_mode != xenos::EDRAMMode::kDepthOnly) {
    xenos::reg::RB_DEPTHCONTROL disabled;
    disabled.value = 0;
    return disabled;
  }
  xenos::reg::RB_DEPTHCONTROL depthcontrol = regs.Get<xenos::reg::RB_DEPTHCONTROL>();
  if (depthcontrol.z_enable && !depthcontrol.z_write_enable &&
      depthcontrol.zfunc == xenos::CompareFunction::kAlways) {
    depthcontrol.z_enable = 0;
  }
  return depthcontrol;
}

const i8 kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const i8 kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

void GetPreferredFacePolygonOffset(const xenos::RegisterFile& regs, bool primitive_polygonal,
                                   f32& scale_out, f32& offset_out) {
  f32 scale = 0.0f, offset = 0.0f;
  auto pa_su_sc_mode_cntl = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    if (pa_su_sc_mode_cntl.poly_offset_front_enable && !pa_su_sc_mode_cntl.cull_front) {
      scale = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (pa_su_sc_mode_cntl.poly_offset_back_enable && !pa_su_sc_mode_cntl.cull_back &&
        scale == 0.0f && offset == 0.0f) {
      scale = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_BACK_SCALE);
      offset = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else {
    if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
      scale = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
  }
  scale_out = scale;
  offset_out = offset;
}

bool IsPixelShaderNeededWithRasterization(const Shader& shader, const xenos::RegisterFile& regs,
                                          bool include_memory_export) {
  REX_ASSERT(shader.GetType() == xenos::ShaderType::kPixel);
  REX_ASSERT(shader.IsUcodeAnalyzed());

  if (regs.Get<xenos::reg::RB_MODECONTROL>().edram_mode != xenos::EDRAMMode::kColorDepth) {
    return false;
  }

  if (shader.GetKillsPixels() || shader.GetWritesDepth() ||
      (include_memory_export && shader.GetMemExportEMWritten()) ||
      (shader.GetWritesColorTarget(0) &&
       DoesCoverageDependOnAlpha(regs.Get<xenos::reg::RB_COLORCONTROL>()))) {
    return true;
  }

  u32 rb_color_mask = regs[std::to_underlying(xenos::Register::RB_COLOR_MASK)];
  u32 rts_remaining = shader.GetWritesColorTargets();
  u32 rt_index;
  while (rex::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(static_cast<u32>(1) << rt_index);
    u32 format_component_count = GetColorRenderTargetFormatComponentCount(
        regs.Get<xenos::reg::RB_COLOR_INFO>(xenos::reg::RB_COLOR_INFO::kRtRegisterIndices[rt_index])
            .color_format);
    if ((rb_color_mask >> (rt_index * 4)) & ((static_cast<u32>(1) << format_component_count) - 1)) {
      return true;
    }
  }

  return false;
}

void GetHostViewportInfo(const xenos::RegisterFile& regs, u32 draw_resolution_scale_x,
                         u32 draw_resolution_scale_y, bool origin_bottom_left, u32 x_max, u32 y_max,
                         bool allow_reverse_z, xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                         bool convert_z_to_float24, bool full_float24_in_0_to_1,
                         bool pixel_shader_writes_depth, ViewportInfo& viewport_info_out) {
  REX_ASSERT(draw_resolution_scale_x != 0);
  REX_ASSERT(draw_resolution_scale_y != 0);

  auto pa_cl_clip_cntl = regs.Get<xenos::reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<xenos::reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>();
  auto pa_su_vtx_cntl = regs.Get<xenos::reg::PA_SU_VTX_CNTL>();

  f32 scale_xy[] = {
      pa_cl_vte_cntl.vport_x_scale_ena ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_XSCALE) : 1.0f,
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_YSCALE) : 1.0f,
  };
  f32 scale_z =
      pa_cl_vte_cntl.vport_z_scale_ena ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_ZSCALE) : 1.0f;
  f32 offset_base_xy[] = {
      pa_cl_vte_cntl.vport_x_offset_ena ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_XOFFSET)
                                        : 0.0f,
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_YOFFSET)
                                        : 0.0f,
  };
  f32 offset_z = pa_cl_vte_cntl.vport_z_offset_ena
                     ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_ZOFFSET)
                     : 0.0f;
  f32 offset_add_xy[2] = {};
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    auto pa_sc_window_offset = regs.Get<xenos::reg::PA_SC_WINDOW_OFFSET>();
    offset_add_xy[0] += static_cast<f32>(pa_sc_window_offset.window_x_offset);
    offset_add_xy[1] += static_cast<f32>(pa_sc_window_offset.window_y_offset);
  }
  if (REXCVAR_GET(::rex::gpu::cvars::half_pixel_offset) &&
      pa_su_vtx_cntl.pix_center == xenos::PixelCenter::kD3DZero) {
    offset_add_xy[0] += 0.5f;
    offset_add_xy[1] += 0.5f;
  }

  u32 xy_max_unscaled[] = {x_max / draw_resolution_scale_x, y_max / draw_resolution_scale_y};
  REX_ASSERT(xy_max_unscaled[0] != 0);
  REX_ASSERT(xy_max_unscaled[1] != 0);

  f32 z_min;
  f32 z_max;
  f32 ndc_scale[3];
  f32 ndc_offset[3];

  if (pa_cl_clip_cntl.clip_disable) {
    for (u32 i = 0; i < 2; ++i) {
      viewport_info_out.xy_offset[i] = 0;
      u32 extent_axis_unscaled = std::min(xenos::kTexture2DCubeMaxWidthHeight, xy_max_unscaled[i]);
      viewport_info_out.xy_extent[i] =
          extent_axis_unscaled * (i ? draw_resolution_scale_y : draw_resolution_scale_x);
      f32 extent_axis_unscaled_float = static_cast<f32>(extent_axis_unscaled);
      f32 pixels_to_ndc_axis = 2.0f / extent_axis_unscaled_float;
      ndc_scale[i] = scale_xy[i] * pixels_to_ndc_axis;
      ndc_offset[i] = (offset_base_xy[i] - extent_axis_unscaled_float * 0.5f + offset_add_xy[i]) *
                      pixels_to_ndc_axis;
    }

    z_min = 0.0f;
    z_max = 1.0f;
    ndc_scale[2] = scale_z;
    ndc_offset[2] = offset_z;
  } else {
    for (u32 i = 0; i < 2; ++i) {
      u32 axis_resolution_scale = i ? draw_resolution_scale_y : draw_resolution_scale_x;
      f32 offset_axis = offset_base_xy[i] + offset_add_xy[i];
      f32 scale_axis = scale_xy[i];
      f32 scale_axis_abs = std::abs(scale_xy[i]);
      f32 axis_max_unscaled_float = static_cast<f32>(xy_max_unscaled[i]);
      u32 axis_0_int = static_cast<u32>(
          rex::clamp_float(offset_axis - scale_axis_abs, 0.0f, axis_max_unscaled_float));
      u32 axis_1_int = static_cast<u32>(
          rex::clamp_float(offset_axis + scale_axis_abs, 0.0f, axis_max_unscaled_float));
      u32 axis_extent_int = axis_1_int - axis_0_int;
      viewport_info_out.xy_offset[i] = axis_0_int * axis_resolution_scale;
      viewport_info_out.xy_extent[i] = axis_extent_int * axis_resolution_scale;
      f32 ndc_scale_axis;
      f32 ndc_offset_axis;
      if (axis_extent_int) {
        f32 axis_extent_rounded = static_cast<f32>(axis_extent_int);
        ndc_scale_axis = scale_axis * 2.0f / axis_extent_rounded;
        ndc_offset_axis =
            (offset_axis - (static_cast<f32>(axis_0_int) + axis_extent_rounded * 0.5f)) * 2.0f /
            axis_extent_rounded;
      } else {
        ndc_scale_axis = 1.0f;
        ndc_offset_axis = 0.0f;
      }
      ndc_scale[i] = ndc_scale_axis;
      ndc_offset[i] = ndc_offset_axis;
    }

    f32 host_clip_offset_z;
    f32 host_clip_scale_z;
    if (pa_cl_clip_cntl.dx_clip_space_def) {
      host_clip_offset_z = offset_z;
      host_clip_scale_z = scale_z;
      ndc_scale[2] = 1.0f;
      ndc_offset[2] = 0.0f;
    } else {
      host_clip_offset_z = offset_z - scale_z;
      host_clip_scale_z = scale_z * 2.0f;
      ndc_scale[2] = 0.5f;
      ndc_offset[2] = 0.5f;
    }
    if (pixel_shader_writes_depth) {
      z_min = 0.0f;
      z_max = 1.0f;
    } else {
      z_min = rex::saturate(host_clip_offset_z);
      z_max = rex::saturate(host_clip_offset_z + host_clip_scale_z);
      if (!allow_reverse_z && z_min > z_max) {
        std::swap(z_min, z_max);
        ndc_scale[2] = -ndc_scale[2];
        ndc_offset[2] = 1.0f - ndc_offset[2];
      }
    }
  }

  if (normalized_depth_control.z_enable && regs.Get<xenos::reg::RB_DEPTH_INFO>().depth_format ==
                                               xenos::DepthRenderTargetFormat::kD24FS8) {
    if (convert_z_to_float24) {
      z_min = xenos::Float20e4To32(xenos::Float32To20e4(z_min, true));
      z_max = xenos::Float20e4To32(xenos::Float32To20e4(z_max, true));
    }
    if (full_float24_in_0_to_1) {
      z_min *= 0.5f;
      z_max *= 0.5f;
    }
  }
  viewport_info_out.z_min = z_min;
  viewport_info_out.z_max = z_max;

  if (origin_bottom_left) {
    ndc_scale[1] = -ndc_scale[1];
    ndc_offset[1] = -ndc_offset[1];
  }
  for (u32 i = 0; i < 3; ++i) {
    viewport_info_out.ndc_scale[i] = ndc_scale[i];
    viewport_info_out.ndc_offset[i] = ndc_offset[i];
  }
}

void GetScissor(const xenos::RegisterFile& regs, Scissor& scissor_out,
                bool clamp_to_surface_pitch) {
  auto pa_sc_window_scissor_tl = regs.Get<xenos::reg::PA_SC_WINDOW_SCISSOR_TL>();
  i32 tl_x = static_cast<i32>(pa_sc_window_scissor_tl.tl_x);
  i32 tl_y = static_cast<i32>(pa_sc_window_scissor_tl.tl_y);
  auto pa_sc_window_scissor_br = regs.Get<xenos::reg::PA_SC_WINDOW_SCISSOR_BR>();
  i32 br_x = static_cast<i32>(pa_sc_window_scissor_br.br_x);
  i32 br_y = static_cast<i32>(pa_sc_window_scissor_br.br_y);
  if (!pa_sc_window_scissor_tl.window_offset_disable) {
    auto pa_sc_window_offset = regs.Get<xenos::reg::PA_SC_WINDOW_OFFSET>();
    tl_x += pa_sc_window_offset.window_x_offset;
    tl_y += pa_sc_window_offset.window_y_offset;
    br_x += pa_sc_window_offset.window_x_offset;
    br_y += pa_sc_window_offset.window_y_offset;
  }
  auto pa_sc_screen_scissor_tl = regs.Get<xenos::reg::PA_SC_SCREEN_SCISSOR_TL>();
  tl_x = std::max(tl_x, static_cast<i32>(pa_sc_screen_scissor_tl.tl_x));
  tl_y = std::max(tl_y, static_cast<i32>(pa_sc_screen_scissor_tl.tl_y));
  auto pa_sc_screen_scissor_br = regs.Get<xenos::reg::PA_SC_SCREEN_SCISSOR_BR>();
  br_x = std::min(br_x, static_cast<i32>(pa_sc_screen_scissor_br.br_x));
  br_y = std::min(br_y, static_cast<i32>(pa_sc_screen_scissor_br.br_y));
  if (clamp_to_surface_pitch) {
    u32 surface_pitch = regs.Get<xenos::reg::RB_SURFACE_INFO>().surface_pitch;
    tl_x = std::min(tl_x, static_cast<i32>(surface_pitch));
    br_x = std::min(br_x, static_cast<i32>(surface_pitch));
  }
  tl_x = std::max(tl_x, static_cast<i32>(0));
  tl_y = std::max(tl_y, static_cast<i32>(0));
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  scissor_out.offset[0] = static_cast<u32>(tl_x);
  scissor_out.offset[1] = static_cast<u32>(tl_y);
  scissor_out.extent[0] = static_cast<u32>(br_x - tl_x);
  scissor_out.extent[1] = static_cast<u32>(br_y - tl_y);
}

u32 GetNormalizedColorMask(const xenos::RegisterFile& regs, u32 pixel_shader_writes_color_targets) {
  if (regs.Get<xenos::reg::RB_MODECONTROL>().edram_mode != xenos::EDRAMMode::kColorDepth) {
    return 0;
  }
  u32 normalized_color_mask = 0;
  u32 rb_color_mask = regs[std::to_underlying(xenos::Register::RB_COLOR_MASK)];
  for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(pixel_shader_writes_color_targets & (static_cast<u32>(1) << i))) {
      continue;
    }
    u32 format_component_mask =
        (static_cast<u32>(1) << xenos::GetColorRenderTargetFormatComponentCount(
             regs.Get<xenos::reg::RB_COLOR_INFO>(xenos::reg::RB_COLOR_INFO::kRtRegisterIndices[i])
                 .color_format)) -
        1;
    u32 rt_write_mask = (rb_color_mask >> (4 * i)) & format_component_mask;
    if (!rt_write_mask) {
      continue;
    }
    rt_write_mask |= 0b1111 & ~format_component_mask;
    normalized_color_mask |= rt_write_mask << (4 * i);
  }
  return normalized_color_mask;
}

void AddMemExportRanges(const xenos::RegisterFile& regs, const Shader& shader,
                        std::vector<MemExportRange>& ranges_out) {
  if (!shader.GetMemExportEMWritten()) {
    return;
  }
  u32 float_constants_base = shader.GetType() == xenos::ShaderType::kVertex
                                 ? regs.Get<xenos::reg::SQ_VS_CONST>().base
                                 : regs.Get<xenos::reg::SQ_PS_CONST>().base;
  for (u32 constant_index : shader.GetMemexportStreamConstants()) {
    xenos::MemExportStream stream = regs.GetMemExportStream(float_constants_base + constant_index);
    if (stream.const_0x1 != 0x1 || stream.const_0x4b0 != 0x4B0 || stream.const_0x96 != 0x96 ||
        !stream.index_count) {
      continue;
    }
    const xenos::FormatInfo& format_info =
        *xenos::FormatInfo::Get(static_cast<xenos::TextureFormat>(stream.format));
    if (format_info.type != xenos::FormatType::kResolvable) {
      REXLOG_ERROR(gpu, "Unsupported memexport format {}", format_info.name);
      continue;
    }
    switch (stream.format) {
      case xenos::ColorFormat::k_8_A:
      case xenos::ColorFormat::k_8_B:
      case xenos::ColorFormat::k_8_8_8_8_A:
        REXLOG_WARN(gpu,
                    "Memexport done to an unresearched format {}, report the title to "
                    "ReXGlue developers!",
                    format_info.name);
        break;
      default:
        break;
    }
    u32 stream_size_bytes = stream.index_count * (format_info.bits_per_pixel >> 3);
    bool range_reused = false;
    for (MemExportRange& range : ranges_out) {
      if (range.base_address_dwords == stream.base_address) {
        range.size_bytes = std::max(range.size_bytes, stream_size_bytes);
        range_reused = true;
        break;
      }
    }
    if (!range_reused) {
      ranges_out.emplace_back(static_cast<u32>(stream.base_address), stream_size_bytes);
    }
  }
}

xenos::CopySampleSelect SanitizeCopySampleSelect(xenos::CopySampleSelect copy_sample_select,
                                                 xenos::MSAASamples msaa_samples, bool is_depth) {
  if (msaa_samples >= xenos::MSAASamples::k4X) {
    if (copy_sample_select > xenos::CopySampleSelect::k0123) {
      copy_sample_select = xenos::CopySampleSelect::k0123;
    }
    if (is_depth) {
      switch (copy_sample_select) {
        case xenos::CopySampleSelect::k01:
        case xenos::CopySampleSelect::k0123:
          copy_sample_select = xenos::CopySampleSelect::k0;
          break;
        case xenos::CopySampleSelect::k23:
          copy_sample_select = xenos::CopySampleSelect::k2;
          break;
        default:
          break;
      }
    }
  } else if (msaa_samples >= xenos::MSAASamples::k2X) {
    switch (copy_sample_select) {
      case xenos::CopySampleSelect::k2:
        copy_sample_select = xenos::CopySampleSelect::k0;
        break;
      case xenos::CopySampleSelect::k3:
        copy_sample_select = xenos::CopySampleSelect::k1;
        break;
      default:
        if (copy_sample_select > xenos::CopySampleSelect::k01) {
          copy_sample_select = xenos::CopySampleSelect::k01;
        }
    }
    if (is_depth && copy_sample_select == xenos::CopySampleSelect::k01) {
      copy_sample_select = xenos::CopySampleSelect::k0;
    }
  } else {
    copy_sample_select = xenos::CopySampleSelect::k0;
  }
  return copy_sample_select;
}

void GetResolveEDRAMTileSpan(ResolveEDRAMInfo edram_info, ResolveCoordinateInfo coordinate_info,
                             u32 height_div_8, u32& base_out, u32& row_length_used_out,
                             u32& rows_out) {
  u32 x_scale_log2 = 3 + static_cast<u32>(edram_info.msaa_samples >= xenos::MSAASamples::k4X) +
                     edram_info.format_is_64bpp;
  u32 x0 = (coordinate_info.edram_offset_x_div_8 << x_scale_log2) / xenos::kEDRAMTileWidthSamples;
  u32 x1 = (((coordinate_info.edram_offset_x_div_8 + coordinate_info.width_div_8) << x_scale_log2) +
            (xenos::kEDRAMTileWidthSamples - 1)) /
           xenos::kEDRAMTileWidthSamples;
  u32 y_scale_log2 = 3 + static_cast<u32>(edram_info.msaa_samples >= xenos::MSAASamples::k2X);
  u32 y0 = (coordinate_info.edram_offset_y_div_8 << y_scale_log2) / xenos::kEDRAMTileHeightSamples;
  u32 y1 = (((coordinate_info.edram_offset_y_div_8 + height_div_8) << y_scale_log2) +
            (xenos::kEDRAMTileHeightSamples - 1)) /
           xenos::kEDRAMTileHeightSamples;
  base_out = edram_info.base_tiles + y0 * edram_info.pitch_tiles + x0;
  row_length_used_out = x1 - x0;
  rows_out = y1 - y0;
}

const ResolveCopyShaderInfo
    kResolveCopyShaderInfo[static_cast<size_t>(ResolveCopyShaderIndex::kCount)] = {
        {"Resolve Copy Fast 32bpp 1x/2xMSAA", false, 4, 4, 6, 3},
        {"Resolve Copy Fast 32bpp 4xMSAA", false, 4, 4, 6, 3},
        {"Resolve Copy Fast 64bpp 1x/2xMSAA", false, 4, 4, 5, 3},
        {"Resolve Copy Fast 64bpp 4xMSAA", false, 3, 4, 5, 3},
        {"Resolve Copy Full 8bpp", true, 2, 3, 6, 3},
        {"Resolve Copy Full 16bpp", true, 2, 3, 5, 3},
        {"Resolve Copy Full 32bpp", true, 2, 4, 5, 3},
        {"Resolve Copy Full 64bpp", true, 2, 4, 5, 3},
        {"Resolve Copy Full 128bpp", true, 2, 4, 4, 3},
};

bool GetResolveInfo(const xenos::RegisterFile& regs, const runtime::Memory& memory,
                    u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
                    bool fixed_rg16_truncated_to_minus_1_to_1,
                    bool fixed_rgba16_truncated_to_minus_1_to_1, ResolveInfo& info_out) {
  info_out.coordinate_info.packed = 0;
  info_out.height_div_8 = 0;

  auto rb_copy_control = regs.Get<xenos::reg::RB_COPY_CONTROL>();
  info_out.rb_copy_control = rb_copy_control;

  if (rb_copy_control.copy_command != xenos::CopyCommand::kRaw &&
      rb_copy_control.copy_command != xenos::CopyCommand::kConvert) {
    REXLOG_ERROR(gpu,
                 "Unsupported resolve copy command {}. Report the title to ReXGlue "
                 "developers",
                 static_cast<u32>(rb_copy_control.copy_command));
    REX_ASSERT_ALWAYS();
    return false;
  }

  xenos::VertexFetch fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    REXLOG_ERROR(gpu, "Unsupported resolve vertex buffer format");
    REX_ASSERT_ALWAYS();
    return false;
  }
  const f32* vertices_guest =
      reinterpret_cast<const f32*>(memory.TranslatePhysical(fetch.address * sizeof(u32)));
  f32 half_pixel_offset =
      regs.Get<xenos::reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f
                                                                                        : 0.0f;
  i32 vertices_fixed[6];
  for (size_t i = 0; i < rex::CountOf(vertices_fixed); ++i) {
    vertices_fixed[i] = gpu::FloatToD3D11Fixed16p8(xenos::GPUSwap(vertices_guest[i], fetch.endian) +
                                                   half_pixel_offset);
  }
  // Inclusive.
  i32 x0 = std::min(std::min(vertices_fixed[0], vertices_fixed[2]), vertices_fixed[4]);
  i32 y0 = std::min(std::min(vertices_fixed[1], vertices_fixed[3]), vertices_fixed[5]);
  // Exclusive.
  i32 x1 = std::max(std::max(vertices_fixed[0], vertices_fixed[2]), vertices_fixed[4]);
  i32 y1 = std::max(std::max(vertices_fixed[1], vertices_fixed[3]), vertices_fixed[5]);
  // Top-left includes .5, so 0.128 covers pixel 0 and 0.129 does not.
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  // Bottom-right excludes .5.
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;

  auto pa_sc_window_offset = regs.Get<xenos::reg::PA_SC_WINDOW_OFFSET>();

  if (regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    x0 += pa_sc_window_offset.window_x_offset;
    y0 += pa_sc_window_offset.window_y_offset;
    x1 += pa_sc_window_offset.window_x_offset;
    y1 += pa_sc_window_offset.window_y_offset;
  }

  Scissor scissor;
  // Pitch is clamped later after alignment, as it may not be a multiple of 8.
  GetScissor(regs, scissor, false);
  i32 scissor_right = static_cast<i32>(scissor.offset[0] + scissor.extent[0]);
  i32 scissor_bottom = static_cast<i32>(scissor.offset[1] + scissor.extent[1]);
  x0 = std::clamp(x0, static_cast<i32>(scissor.offset[0]), scissor_right);
  y0 = std::clamp(y0, static_cast<i32>(scissor.offset[1]), scissor_bottom);
  x1 = std::clamp(x1, static_cast<i32>(scissor.offset[0]), scissor_right);
  y1 = std::clamp(y1, static_cast<i32>(scissor.offset[1]), scissor_bottom);

  REX_ASSERT(x0 <= x1 && y0 <= y1);

  x0 &= ~static_cast<i32>(xenos::kResolveAlignmentPixels - 1);
  y0 &= ~static_cast<i32>(xenos::kResolveAlignmentPixels - 1);
  x1 = rex::RoundUpPow2(x1, static_cast<i32>(xenos::kResolveAlignmentPixels));
  y1 = rex::RoundUpPow2(y1, static_cast<i32>(xenos::kResolveAlignmentPixels));

  auto rb_surface_info = regs.Get<xenos::reg::RB_SURFACE_INFO>();
  if (rb_surface_info.msaa_samples > xenos::MSAASamples::k4X) {
    REX_ASSERT_ALWAYS();
    REX_FAIL(gpu, false,
             "{}x MSAA requested by the guest in a resolve, Xenos only supports up "
             "to 4x",
             static_cast<u32>(1) << static_cast<u32>(rb_surface_info.msaa_samples));
  }

  // The maximum surface pitch is assumed to be the largest resolvable size.
  i32 surface_pitch_aligned = static_cast<i32>(
      rb_surface_info.surface_pitch & ~static_cast<u32>(xenos::kResolveAlignmentPixels - 1));
  if (x1 > surface_pitch_aligned) {
    REXLOG_ERROR(gpu, "Resolve region {} <= x < {} is outside the surface pitch {}", x0, x1,
                 surface_pitch_aligned);
    x0 = std::min(x0, surface_pitch_aligned);
    x1 = std::min(x1, surface_pitch_aligned);
  }
  REX_ASSERT(x1 - x0 <= static_cast<i32>(xenos::kMaxResolveSize));

  // The height must fit in the packed shader constant.
  if (y1 - y0 > static_cast<i32>(xenos::kMaxResolveSize)) {
    REXLOG_ERROR(gpu, "Resolve region {} <= y < {} is taller than {}", y0, y1,
                 xenos::kMaxResolveSize);
    y1 = y0 + static_cast<i32>(xenos::kMaxResolveSize);
  }

  REX_ASSERT(x0 < x1 && y0 < y1);
  if (x0 >= x1 || y0 >= y1) {
    REX_FAIL(gpu, false, "Resolve region is empty");
  }

  info_out.coordinate_info.width_div_8 =
      static_cast<u32>(x1 - x0) >> xenos::kResolveAlignmentPixelsLog2;
  info_out.height_div_8 = static_cast<u32>(y1 - y0) >> xenos::kResolveAlignmentPixelsLog2;
  // The scales are packed in 3 bits each.
  REX_ASSERT(draw_resolution_scale_x <= 7);
  REX_ASSERT(draw_resolution_scale_y <= 7);
  info_out.coordinate_info.draw_resolution_scale_x = draw_resolution_scale_x;
  info_out.coordinate_info.draw_resolution_scale_y = draw_resolution_scale_y;

  bool is_depth = rb_copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
  xenos::CopySampleSelect sample_select = SanitizeCopySampleSelect(
      rb_copy_control.copy_sample_select, rb_surface_info.msaa_samples, is_depth);
  if (rb_copy_control.copy_sample_select != sample_select) {
    REXLOG_WARN(gpu,
                "Incorrect resolve sample selected for {}-sample {} ({}), treating like "
                "{}",
                1 << static_cast<u32>(rb_surface_info.msaa_samples), is_depth ? "depth" : "color",
                static_cast<u32>(rb_copy_control.copy_sample_select),
                static_cast<u32>(sample_select));
  }
  info_out.copy_dest_coordinate_info.copy_sample_select = sample_select;
  auto rb_copy_dest_info = regs.Get<xenos::reg::RB_COPY_DEST_INFO>();
  xenos::TextureFormat dest_format;
  auto rb_depth_info = regs.Get<xenos::reg::RB_DEPTH_INFO>();
  if (is_depth) {
    dest_format = DepthRenderTargetToTextureFormat(rb_depth_info.depth_format);
  } else {
    dest_format = static_cast<xenos::TextureFormat>(rb_copy_dest_info.copy_dest_format);
    xenos::TextureFormat dest_closest_format;
    switch (dest_format) {
      case xenos::TextureFormat::k_8_A:
      case xenos::TextureFormat::k_8_B:
        dest_closest_format = xenos::TextureFormat::k_8;
        break;
      case xenos::TextureFormat::k_8_8_8_8_A:
        dest_closest_format = xenos::TextureFormat::k_8_8_8_8;
        break;
      default:
        dest_closest_format = dest_format;
    }
    if (dest_format != dest_closest_format) {
      REXLOG_WARN(gpu,
                  "Resolving to format {}, which is untested, treating like {}. "
                  "Report the title to ReXGlue developers!",
                  xenos::FormatInfo::Get(dest_format)->name,
                  xenos::FormatInfo::Get(dest_closest_format)->name);
    }
  }

  u32 rb_copy_dest_base = regs[std::to_underlying(xenos::Register::RB_COPY_DEST_BASE)];
  u32 copy_dest_base_adjusted = rb_copy_dest_base;
  u32 copy_dest_extent_start, copy_dest_extent_end;
  auto rb_copy_dest_pitch = regs.Get<xenos::reg::RB_COPY_DEST_PITCH>();
  u32 copy_dest_pitch_aligned_div_32 =
      (rb_copy_dest_pitch.copy_dest_pitch + (xenos::kTextureTileWidthHeight - 1)) >>
      xenos::kTextureTileWidthHeightLog2;
  info_out.copy_dest_coordinate_info.pitch_aligned_div_32 = copy_dest_pitch_aligned_div_32;
  info_out.copy_dest_coordinate_info.height_aligned_div_32 =
      (rb_copy_dest_pitch.copy_dest_height + (xenos::kTextureTileWidthHeight - 1)) >>
      xenos::kTextureTileWidthHeightLog2;
  const xenos::FormatInfo& dest_format_info = *xenos::FormatInfo::Get(dest_format);
  if (is_depth || dest_format_info.type == xenos::FormatType::kResolvable) {
    u32 bpp_log2 = rex::log2_floor(dest_format_info.bits_per_pixel >> 3);
    u32 dest_base_relative_x_mask =
        (UINT32_C(1) << xenos::GetTextureTiledXBaseGranularityLog2(
             static_cast<bool>(rb_copy_dest_info.copy_dest_array), bpp_log2)) -
        1;
    u32 dest_base_relative_y_mask =
        (UINT32_C(1) << xenos::GetTextureTiledYBaseGranularityLog2(
             static_cast<bool>(rb_copy_dest_info.copy_dest_array), bpp_log2)) -
        1;
    info_out.copy_dest_coordinate_info.offset_x_div_8 =
        (static_cast<u32>(x0) & dest_base_relative_x_mask) >> xenos::kResolveAlignmentPixelsLog2;
    info_out.copy_dest_coordinate_info.offset_y_div_8 =
        (static_cast<u32>(y0) & dest_base_relative_y_mask) >> xenos::kResolveAlignmentPixelsLog2;
    u32 dest_base_x = static_cast<u32>(x0) & ~dest_base_relative_x_mask;
    u32 dest_base_y = static_cast<u32>(y0) & ~dest_base_relative_y_mask;
    if (rb_copy_dest_info.copy_dest_array) {
      copy_dest_base_adjusted += xenos::GetTiledOffset3D(
          static_cast<i32>(dest_base_x), static_cast<i32>(dest_base_y), 0,
          rb_copy_dest_pitch.copy_dest_pitch, rb_copy_dest_pitch.copy_dest_height, bpp_log2);
      copy_dest_extent_start =
          rb_copy_dest_base +
          xenos::GetTiledAddressLowerBound3D(
              static_cast<u32>(x0), static_cast<u32>(y0), rb_copy_dest_info.copy_dest_layer,
              rb_copy_dest_pitch.copy_dest_pitch, rb_copy_dest_pitch.copy_dest_height, bpp_log2);
      copy_dest_extent_end = rb_copy_dest_base + xenos::GetTiledAddressUpperBound3D(
                                                     static_cast<u32>(x1), static_cast<u32>(y1),
                                                     rb_copy_dest_info.copy_dest_layer + 1,
                                                     rb_copy_dest_pitch.copy_dest_pitch,
                                                     rb_copy_dest_pitch.copy_dest_height, bpp_log2);
    } else {
      copy_dest_base_adjusted +=
          xenos::GetTiledOffset2D(static_cast<i32>(dest_base_x), static_cast<i32>(dest_base_y),
                                  rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
      copy_dest_extent_start =
          rb_copy_dest_base +
          xenos::GetTiledAddressLowerBound2D(static_cast<u32>(x0), static_cast<u32>(y0),
                                             rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
      copy_dest_extent_end = rb_copy_dest_base + xenos::GetTiledAddressUpperBound2D(
                                                     static_cast<u32>(x1), static_cast<u32>(y1),
                                                     rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
    }
  } else {
    REXLOG_ERROR(gpu, "Tried to resolve to format {}, which is not a ColorFormat",
                 dest_format_info.name);
    copy_dest_extent_start = copy_dest_base_adjusted;
    copy_dest_extent_end = copy_dest_base_adjusted;
  }
  REX_ASSERT(copy_dest_extent_start >= copy_dest_base_adjusted);
  REX_ASSERT(copy_dest_extent_end >= copy_dest_base_adjusted);
  REX_ASSERT(copy_dest_extent_end >= copy_dest_extent_start);
  info_out.copy_dest_base = copy_dest_base_adjusted;
  info_out.copy_dest_extent_start = copy_dest_extent_start;
  info_out.copy_dest_extent_length = copy_dest_extent_end - copy_dest_extent_start;

  u32 sample_count_log2_x =
      static_cast<u32>(rb_surface_info.msaa_samples >= xenos::MSAASamples::k4X);
  u32 sample_count_log2_y =
      static_cast<u32>(rb_surface_info.msaa_samples >= xenos::MSAASamples::k2X);
  u32 x0_samples = static_cast<u32>(x0) << sample_count_log2_x;
  u32 y0_samples = static_cast<u32>(y0) << sample_count_log2_y;
  u32 base_offset_x_tiles = x0_samples / xenos::kEDRAMTileWidthSamples;
  u32 base_offset_y_tiles = y0_samples / xenos::kEDRAMTileHeightSamples;
  info_out.coordinate_info.edram_offset_x_div_8 =
      (x0_samples % xenos::kEDRAMTileWidthSamples) >> (sample_count_log2_x + 3);
  info_out.coordinate_info.edram_offset_y_div_8 =
      (y0_samples % xenos::kEDRAMTileHeightSamples) >> (sample_count_log2_y + 3);
  u32 surface_pitch_tiles = xenos::GetSurfacePitchTiles(rb_surface_info.surface_pitch,
                                                        rb_surface_info.msaa_samples, false);
  u32 edram_base_offset_tiles = base_offset_y_tiles * surface_pitch_tiles + base_offset_x_tiles;

  bool fill_half_pixel_offset =
      (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) &&
      REXCVAR_GET(::rex::gpu::cvars::resolve_resolution_scale_fill_half_pixel_offset) &&
      REXCVAR_GET(::rex::gpu::cvars::half_pixel_offset) &&
      regs.Get<xenos::reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero;
  i32 exp_bias = is_depth ? 0 : rb_copy_dest_info.copy_dest_exp_bias;
  ResolveEDRAMInfo depth_edram_info;
  depth_edram_info.packed = 0;
  if (is_depth || rb_copy_control.depth_clear_enable) {
    depth_edram_info.pitch_tiles = surface_pitch_tiles;
    depth_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    depth_edram_info.is_depth = 1;
    depth_edram_info.base_tiles = rb_depth_info.depth_base + edram_base_offset_tiles;
    depth_edram_info.format = static_cast<u32>(rb_depth_info.depth_format);
    depth_edram_info.format_is_64bpp = 0;
    depth_edram_info.fill_half_pixel_offset = static_cast<u32>(fill_half_pixel_offset);
    info_out.depth_original_base = rb_depth_info.depth_base;
  } else {
    info_out.depth_original_base = 0;
  }
  info_out.depth_edram_info = depth_edram_info;
  ResolveEDRAMInfo color_edram_info;
  color_edram_info.packed = 0;
  if (!is_depth) {
    auto color_info = regs.Get<xenos::reg::RB_COLOR_INFO>(
        xenos::reg::RB_COLOR_INFO::kRtRegisterIndices[rb_copy_control.copy_src_select]);
    u32 is_64bpp = static_cast<u32>(xenos::IsColorRenderTargetFormat64bpp(color_info.color_format));
    color_edram_info.pitch_tiles = surface_pitch_tiles << is_64bpp;
    color_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    color_edram_info.is_depth = 0;
    color_edram_info.base_tiles = color_info.color_base + (edram_base_offset_tiles << is_64bpp);
    color_edram_info.format = static_cast<u32>(color_info.color_format);
    color_edram_info.format_is_64bpp = is_64bpp;
    color_edram_info.fill_half_pixel_offset = static_cast<u32>(fill_half_pixel_offset);
    if ((fixed_rg16_truncated_to_minus_1_to_1 &&
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16) ||
        (fixed_rgba16_truncated_to_minus_1_to_1 &&
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)) {
      exp_bias = std::min(exp_bias + static_cast<i32>(5), static_cast<i32>(31));
    }
    info_out.color_original_base = color_info.color_base;
  } else {
    info_out.color_original_base = 0;
  }
  info_out.color_edram_info = color_edram_info;

  info_out.copy_dest_info = rb_copy_dest_info;
  info_out.copy_dest_info.copy_dest_format = static_cast<xenos::ColorFormat>(dest_format);
  info_out.copy_dest_info.copy_dest_exp_bias = exp_bias;
  if (is_depth) {
    info_out.copy_dest_info.copy_dest_swap = false;
  }

  info_out.rb_depth_clear = regs[std::to_underlying(xenos::Register::RB_DEPTH_CLEAR)];
  info_out.rb_color_clear = regs[std::to_underlying(xenos::Register::RB_COLOR_CLEAR)];
  info_out.rb_color_clear_lo = regs[std::to_underlying(xenos::Register::RB_COLOR_CLEAR_LO)];

  REXLOG_TRACE(gpu,
               "Resolve: {},{} <= x,y < {},{}, {} -> {} at 0x{:08X} (potentially "
               "modified memory range 0x{:08X} to 0x{:08X})",
               x0, y0, x1, y1,
               is_depth ? xenos::GetDepthRenderTargetFormatName(
                              static_cast<xenos::DepthRenderTargetFormat>(depth_edram_info.format))
                        : xenos::GetColorRenderTargetFormatName(
                              static_cast<xenos::ColorRenderTargetFormat>(color_edram_info.format)),
               dest_format_info.name, rb_copy_dest_base, copy_dest_extent_start,
               copy_dest_extent_end);

  return true;
}

ResolveCopyShaderIndex ResolveInfo::GetCopyShader(u32 draw_resolution_scale_x,
                                                  u32 draw_resolution_scale_y,
                                                  ResolveCopyShaderConstants& constants_out,
                                                  u32& group_count_x_out,
                                                  u32& group_count_y_out) const {
  ResolveCopyShaderIndex shader = ResolveCopyShaderIndex::kUnknown;
  bool is_depth = IsCopyingDepth();
  ResolveEDRAMInfo edram_info = is_depth ? depth_edram_info : color_edram_info;
  bool source_is_64bpp = !is_depth && color_edram_info.format_is_64bpp != 0;
  if (is_depth ||
      (!copy_dest_info.copy_dest_exp_bias &&
       xenos::IsSingleCopySampleSelected(copy_dest_coordinate_info.copy_sample_select) &&
       xenos::IsColorResolveFormatBitwiseEquivalent(
           static_cast<xenos::ColorRenderTargetFormat>(color_edram_info.format),
           static_cast<xenos::ColorFormat>(copy_dest_info.copy_dest_format)))) {
    if (edram_info.msaa_samples >= xenos::MSAASamples::k4X) {
      shader = source_is_64bpp ? ResolveCopyShaderIndex::kFast64bpp4xMSAA
                               : ResolveCopyShaderIndex::kFast32bpp4xMSAA;
    } else {
      shader = source_is_64bpp ? ResolveCopyShaderIndex::kFast64bpp1x2xMSAA
                               : ResolveCopyShaderIndex::kFast32bpp1x2xMSAA;
    }
  } else {
    const xenos::FormatInfo& dest_format_info =
        *xenos::FormatInfo::Get(static_cast<xenos::TextureFormat>(copy_dest_info.copy_dest_format));
    switch (dest_format_info.bits_per_pixel) {
      case 8:
        shader = ResolveCopyShaderIndex::kFull8bpp;
        break;
      case 16:
        shader = ResolveCopyShaderIndex::kFull16bpp;
        break;
      case 32:
        shader = ResolveCopyShaderIndex::kFull32bpp;
        break;
      case 64:
        shader = ResolveCopyShaderIndex::kFull64bpp;
        break;
      case 128:
        shader = ResolveCopyShaderIndex::kFull128bpp;
        break;
      default:
        REX_UNHANDLED_CASE(dest_format_info.bits_per_pixel);
    }
  }

  constants_out.dest_relative.edram_info = edram_info;
  constants_out.dest_relative.coordinate_info = coordinate_info;
  constants_out.dest_relative.dest_info = copy_dest_info;
  constants_out.dest_relative.dest_coordinate_info = copy_dest_coordinate_info;
  constants_out.dest_base = copy_dest_base;

  if (shader != ResolveCopyShaderIndex::kUnknown) {
    u32 width = (coordinate_info.width_div_8 << xenos::kResolveAlignmentPixelsLog2) *
                draw_resolution_scale_x;
    u32 height = (height_div_8 << xenos::kResolveAlignmentPixelsLog2) * draw_resolution_scale_y;
    const ResolveCopyShaderInfo& shader_info = kResolveCopyShaderInfo[static_cast<size_t>(shader)];
    group_count_x_out =
        (width + ((1 << shader_info.group_size_x_log2) - 1)) >> shader_info.group_size_x_log2;
    group_count_y_out =
        (height + ((1 << shader_info.group_size_y_log2) - 1)) >> shader_info.group_size_y_log2;
  } else {
    REXLOG_ERROR(gpu, "No resolve copy compute shader for the provided configuration");
    REX_ASSERT_ALWAYS();
    group_count_x_out = 0;
    group_count_y_out = 0;
  }

  return shader;
}

}  // namespace rex::gpu::draw_state
