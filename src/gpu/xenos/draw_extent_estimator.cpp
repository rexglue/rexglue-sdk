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

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cstdint>

#include <rex/bit.h>
#include <rex/assert.h>
#include <rex/platform/perf.h>
#include <rex/xenos/ucode.h>
#include <rex/xenos/register.h>
#include "gpu/xenos/draw_extent_estimator.h"
#include "gpu/xenos/physical_memory.h"
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>
#include <rex/memory.h>
#include "gpu/xenos/host/fixed_point.h"

namespace rex::gpu {

void DrawExtentEstimator::PositionYExportSink::Export(xenos::ucode::ExportRegister export_register,
                                                      const f32* value, u32 value_mask) {
  if (export_register == xenos::ucode::ExportRegister::kVSPosition) {
    if (value_mask & 0b0010) {
      position_y_ = value[1];
    }
    if (value_mask & 0b1000) {
      position_w_ = value[3];
    }
  } else if (export_register == xenos::ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if (value_mask & 0b0001) {
      point_size_ = value[0];
    }
    if (value_mask & 0b0100) {
      vertex_kill_ = std::bit_cast<u32>(value[2]);
    }
  }
}

u32 DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader) {
  REX_PROFILE_SCOPE("gpu");

  const xenos::RegisterFile& regs = register_file_;

  auto vgt_draw_initiator = regs.Get<xenos::reg::VGT_DRAW_INITIATOR>();
  if (!vgt_draw_initiator.num_indices) {
    return 0;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) &&
      regs.Get<xenos::reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  REX_ASSERT(vertex_shader.GetType() == xenos::ShaderType::kVertex);
  REX_ASSERT(vertex_shader.IsUcodeAnalyzed());
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  auto vgt_dma_size = regs.Get<xenos::reg::VGT_DMA_SIZE>();
  union {
    const void* index_buffer;
    const u16* index_buffer_16;
    const u32* index_buffer_32;
  };
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    xenos::IndexFormat index_format = vgt_draw_initiator.index_size;
    u32 index_buffer_base = regs[std::to_underlying(xenos::Register::VGT_DMA_BASE)];
    if (index_format == xenos::IndexFormat::kInt16) {
      // Index endianness is handled as in the PrimitiveProcessor.
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~static_cast<u32>(sizeof(u16) - 1);
    } else {
      REX_ASSERT(index_format == xenos::IndexFormat::kInt32);
      index_buffer_base &= ~static_cast<u32>(sizeof(u32) - 1);
    }
    index_buffer = GetPhysicalToEndForRead(physical_memory_, index_buffer_base);
    if (!index_buffer) {
      return xenos::kTexture2DCubeMaxWidthHeight;
    }
  }
  auto pa_su_sc_mode_cntl = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>();
  u32 reset_index = regs.Get<xenos::reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  u32 index_offset = regs.Get<xenos::reg::VGT_INDX_OFFSET>().indx_offset;
  u32 min_index = regs.Get<xenos::reg::VGT_MIN_VTX_INDX>().min_indx;
  u32 max_index = regs.Get<xenos::reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<xenos::reg::PA_CL_VTE_CNTL>();
  f32 viewport_y_scale =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_YSCALE) : 1.0f;
  f32 viewport_y_offset = pa_cl_vte_cntl.vport_y_offset_ena
                              ? regs.Get<f32>(xenos::Register::PA_CL_VPORT_YOFFSET)
                              : 0.0f;

  i32 point_vertex_min_diameter_float = 0;
  i32 point_vertex_max_diameter_float = 0;
  f32 point_constant_radius_y = 0.0f;
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<xenos::reg::PA_SU_POINT_MINMAX>();
    point_vertex_min_diameter_float =
        std::bit_cast<i32>(static_cast<f32>(pa_su_point_minmax.min_size) * (2.0f / 16.0f));
    point_vertex_max_diameter_float =
        std::bit_cast<i32>(static_cast<f32>(pa_su_point_minmax.max_size) * (2.0f / 16.0f));
    point_constant_radius_y =
        static_cast<f32>(regs.Get<xenos::reg::PA_SU_POINT_SIZE>().height) * (1.0f / 16.0f);
  }

  f32 max_y = -FLT_MAX;

  shader_interpreter_.SetShader(vertex_shader);

  PositionYExportSink position_y_export_sink;
  shader_interpreter_.SetExportSink(&position_y_export_sink);
  for (u32 i = 0; i < vgt_draw_initiator.num_indices; ++i) {
    u32 vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (i < vgt_dma_size.num_words) {
        if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
          vertex_index = index_buffer_16[i];
        } else {
          vertex_index = index_buffer_32[i];
        }
        // The Xenos uses 24 bits of the index, as reset_indx is 24 bit.
        vertex_index = xenos::GPUSwap(vertex_index, index_endian) & 0xFFFFFF;
      } else {
        vertex_index = 0;
      }
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        continue;
      }
    } else {
      REX_ASSERT(vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex);
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));

    position_y_export_sink.Reset();

    shader_interpreter_.GetTempRegisters()[0] = static_cast<f32>(vertex_index);
    shader_interpreter_.Execute();

    if (position_y_export_sink.GetVertexKill().has_value() &&
        (position_y_export_sink.GetVertexKill().value() & ~(UINT32_C(1) << 31))) {
      continue;
    }
    if (!position_y_export_sink.GetPositionY().has_value()) {
      continue;
    }
    f32 vertex_y = position_y_export_sink.GetPositionY().value();
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!position_y_export_sink.GetPositionW().has_value()) {
        continue;
      }
      vertex_y /= position_y_export_sink.GetPositionW().value();
    }

    vertex_y = vertex_y * viewport_y_scale + viewport_y_offset;

    if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
      f32 point_radius_y;
      if (position_y_export_sink.GetPointSize().has_value()) {
        point_radius_y =
            0.5f *
            std::bit_cast<f32>(std::min(
                point_vertex_max_diameter_float,
                std::max(point_vertex_min_diameter_float,
                         std::bit_cast<i32>(position_y_export_sink.GetPointSize().value()))));
      } else {
        point_radius_y = point_constant_radius_y;
      }
      vertex_y += point_radius_y;
    }

    max_y = std::max(max_y, vertex_y);
  }
  shader_interpreter_.SetExportSink(nullptr);

  i32 max_y_24p8 = gpu::FloatToD3D11Fixed16p8(max_y);
  if (regs.Get<xenos::reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    max_y_24p8 += 128;
  }
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    max_y_24p8 += regs.Get<xenos::reg::PA_SC_WINDOW_OFFSET>().window_y_offset * 256;
  }
  // Top left rule, so the 0.5 sample is exclusive without MSAA and the 1.0 sample with MSAA.
  auto rb_surface_info = regs.Get<xenos::reg::RB_SURFACE_INFO>();
  return (static_cast<u32>(std::max(static_cast<i32>(0), max_y_24p8)) +
          ((rb_surface_info.msaa_samples == xenos::MSAASamples::k1X) ? 127 : 255)) >>
         8;
}

u32 DrawExtentEstimator::EstimateMaxY(bool try_to_estimate_vertex_max_y,
                                      const Shader& vertex_shader) {
  REX_PROFILE_SCOPE("gpu");

  const xenos::RegisterFile& regs = register_file_;

  auto pa_sc_window_offset = regs.Get<xenos::reg::PA_SC_WINDOW_OFFSET>();
  i32 window_y_offset = pa_sc_window_offset.window_y_offset;

  auto pa_sc_window_scissor_br = regs.Get<xenos::reg::PA_SC_WINDOW_SCISSOR_BR>();
  i32 scissor_bottom = static_cast<i32>(pa_sc_window_scissor_br.br_y);
  bool scissor_window_offset =
      !regs.Get<xenos::reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable;
  if (scissor_window_offset) {
    scissor_bottom += window_y_offset;
  }
  auto pa_sc_screen_scissor_br = regs.Get<xenos::reg::PA_SC_SCREEN_SCISSOR_BR>();
  scissor_bottom = std::min(scissor_bottom, static_cast<i32>(pa_sc_screen_scissor_br.br_y));
  u32 max_y = static_cast<u32>(std::max(scissor_bottom, static_cast<i32>(0)));

  if (regs.Get<xenos::reg::PA_CL_CLIP_CNTL>().clip_disable) {
    if (try_to_estimate_vertex_max_y && settings_.execute_unclipped_draw_vs_on_cpu.Get()) {
      bool estimate_vertex_max_y;
      if (settings_.execute_unclipped_draw_vs_on_cpu_with_scissor.Get()) {
        estimate_vertex_max_y = true;
      } else {
        estimate_vertex_max_y = false;
        if (scissor_bottom >= static_cast<i32>(xenos::kTexture2DCubeMaxWidthHeight)) {
          i32 scissor_right = static_cast<i32>(pa_sc_window_scissor_br.br_x);
          if (scissor_window_offset) {
            scissor_right += pa_sc_window_offset.window_x_offset;
          }
          scissor_right = std::min(scissor_right, static_cast<i32>(pa_sc_screen_scissor_br.br_x));
          if (scissor_right >= static_cast<i32>(xenos::kTexture2DCubeMaxWidthHeight)) {
            estimate_vertex_max_y = true;
          }
        }
      }
      if (estimate_vertex_max_y) {
        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader));
      }
    }
  } else {
    auto pa_cl_vte_cntl = regs.Get<xenos::reg::PA_CL_VTE_CNTL>();
    f32 viewport_bottom = 0.0f;
    if (regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      viewport_bottom += static_cast<f32>(window_y_offset);
    }
    if (regs.Get<xenos::reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
      viewport_bottom += 0.5f;
    }
    if (pa_cl_vte_cntl.vport_y_offset_ena) {
      viewport_bottom += regs.Get<f32>(xenos::Register::PA_CL_VPORT_YOFFSET);
    }
    viewport_bottom += pa_cl_vte_cntl.vport_y_scale_ena
                           ? std::abs(regs.Get<f32>(xenos::Register::PA_CL_VPORT_YSCALE))
                           : 1.0f;
    max_y = static_cast<u32>(std::min(static_cast<f32>(max_y), std::max(0.0f, viewport_bottom)));
  }

  return max_y;
}

}  // namespace rex::gpu
