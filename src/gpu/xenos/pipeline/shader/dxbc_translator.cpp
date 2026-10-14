/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "thirdparty/dxbc/DXBCChecksum.h"

#include <algorithm>
#include <functional>
#include <atomic>
#include <cstring>
#include <memory>

#include <rex/assert.h>
#include <rex/cvar.h>
#include "gpu/xenos/pipeline/shader/dxbc.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include <rex/xenos/xenos.h>
#include <rex/math.h>
#include "gpu/xenos/host/graphics_provider.h"

#include "gpu/xenos/cvar_table.h"

namespace rex::gpu {
using namespace xenos::ucode;

// x# is accessed only by mov, CBs are indexed CBx[y][z] with x binding, y register, z vector.

DXBCShaderTranslator::DXBCShaderTranslator(gpu::GraphicsProvider::GPUVendorID vendor_id,
                                           bool bindless_resources_used, bool edram_rov_used,
                                           bool gamma_render_target_as_unorm8,
                                           bool msaa_2x_supported, u32 draw_resolution_scale_x,
                                           u32 draw_resolution_scale_y, bool force_emit_source_map)
    : a_(shader_code_, statistics_),
      ao_(shader_object_, statistics_),
      emit_source_map_(force_emit_source_map || REXCVAR_GET(::rex::gpu::cvars::dxbc_source_map)),
      vendor_id_(vendor_id),
      bindless_resources_used_(bindless_resources_used),
      edram_rov_used_(edram_rov_used),
      gamma_render_target_as_unorm8_(gamma_render_target_as_unorm8),
      msaa_2x_supported_(msaa_2x_supported),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y) {
  REX_ASSERT(draw_resolution_scale_x != 0);
  REX_ASSERT(draw_resolution_scale_y != 0);
  shader_code_.reserve(8192);
  shader_object_.reserve(16384);
}
DXBCShaderTranslator::~DXBCShaderTranslator() = default;

std::vector<u8> DXBCShaderTranslator::CreateDepthOnlyPixelShader() {
  is_depth_only_pixel_shader_ = true;
  Shader shader(xenos::ShaderType::kPixel, 0, nullptr, 0);
  shader.AnalyzeUcode(instruction_disassembly_buffer_);
  Shader::Translation& translation = *shader.GetOrCreateTranslation(0);
  TranslateAnalyzedShader(translation);
  is_depth_only_pixel_shader_ = false;
  return translation.GetTranslatedBinary();
}

u64 DXBCShaderTranslator::GetDefaultVertexShaderModification(
    u32 dynamic_addressable_register_count,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  Modification shader_modification;
  shader_modification.vertex.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  shader_modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  shader_modification.vertex.interpolator_mask = (UINT32_C(1) << xenos::kMaxInterpolators) - 1;
  return shader_modification.value;
}

u64 DXBCShaderTranslator::GetDefaultPixelShaderModification(
    u32 dynamic_addressable_register_count) const {
  Modification shader_modification;
  shader_modification.pixel.dynamic_addressable_register_count = dynamic_addressable_register_count;
  shader_modification.pixel.interpolator_mask = (UINT32_C(1) << xenos::kMaxInterpolators) - 1;
  shader_modification.pixel.depth_stencil_mode = Modification::DepthStencilMode::kNoModifiers;
  return shader_modification.value;
}

void DXBCShaderTranslator::Reset() {
  ShaderTranslator::Reset();

  shader_code_.clear();

  cbuffer_count_ = 0;
  cbuffer_index_system_constants_ = cbuffer_count_++;
  cbuffer_index_float_constants_ = kBindingIndexUnallocated;
  cbuffer_index_bool_loop_constants_ = kBindingIndexUnallocated;
  cbuffer_index_fetch_constants_ = kBindingIndexUnallocated;
  cbuffer_index_descriptor_indices_ = kBindingIndexUnallocated;

  system_constants_used_ = 0;

  out_reg_vs_interpolators_ = UINT32_MAX;
  out_reg_vs_position_ = UINT32_MAX;
  out_reg_vs_clip_cull_distances_ = UINT32_MAX;
  out_reg_vs_point_size_ = UINT32_MAX;
  in_reg_ps_interpolators_ = UINT32_MAX;
  in_reg_ps_point_coordinates_ = UINT32_MAX;
  in_reg_ps_position_ = UINT32_MAX;
  in_reg_ps_front_face_sample_index_ = UINT32_MAX;

  in_domain_location_used_ = 0;
  in_control_point_index_used_ = false;
  in_position_used_ = 0;
  in_front_face_used_ = false;

  system_temp_count_current_ = 0;
  system_temp_count_max_ = 0;

  cf_exec_bool_constant_ = kCfExecBoolConstantNone;
  cf_exec_predicated_ = false;
  cf_instruction_predicate_if_open_ = false;
  cf_exec_predicate_written_ = false;

  srv_count_ = 0;
  srv_index_shared_memory_ = kBindingIndexUnallocated;
  srv_index_bindless_textures_2d_ = kBindingIndexUnallocated;
  srv_index_bindless_textures_3d_ = kBindingIndexUnallocated;
  srv_index_bindless_textures_cube_ = kBindingIndexUnallocated;

  texture_bindings_.clear();
  texture_bindings_for_bindful_srv_indices_.clear();

  uav_count_ = 0;
  uav_index_shared_memory_ = kBindingIndexUnallocated;
  uav_index_edram_ = kBindingIndexUnallocated;

  sampler_bindings_.clear();

  shader_feature_info_ = {};
  statistics_ = {};
}

u32 DXBCShaderTranslator::GetModificationRegisterCount() const {
  Modification modification = GetDXBCShaderModification();
  return IsVertexShader() ? modification.vertex.dynamic_addressable_register_count
                          : modification.pixel.dynamic_addressable_register_count;
}

bool DXBCShaderTranslator::UseSwitchForControlFlow() const {
  // Intel HD Graphics 4000 crashes with switch.
  return REXCVAR_GET(::rex::gpu::cvars::dxbc_switch) &&
         vendor_id_ != gpu::GraphicsProvider::GPUVendorID::kIntel;
}

u32 DXBCShaderTranslator::PushSystemTemp(u32 zero_mask, u32 count) {
  u32 register_index = system_temp_count_current_;
  if (!is_depth_only_pixel_shader_ && !GetCurrentShader().UsesRegisterDynamicAddressing()) {
    register_index += GetRegisterCount();
  }
  system_temp_count_current_ += count;
  system_temp_count_max_ = std::max(system_temp_count_max_, system_temp_count_current_);
  zero_mask &= 0b1111;
  if (zero_mask) {
    for (u32 i = 0; i < count; ++i) {
      a_.OpMov(dxbc::Dest::R(register_index + i, zero_mask), dxbc::Src::LU(0));
    }
  }
  return register_index;
}

void DXBCShaderTranslator::PopSystemTemp(u32 count) {
  REX_ASSERT(count <= system_temp_count_current_);
  system_temp_count_current_ -= std::min(count, system_temp_count_current_);
}

void DXBCShaderTranslator::PWLGammaToLinear(dxbc::Assembler& a, u32 target_temp,
                                            u32 target_temp_component, u32 source_temp,
                                            u32 source_temp_component, bool source_pre_saturated,
                                            u32 temp1, u32 temp1_component, u32 temp2,
                                            u32 temp2_component) {
  // The source may be the same as the destination.
  REX_ASSERT(temp1 != target_temp || temp1_component != target_temp_component);
  REX_ASSERT(temp1 != source_temp || temp1_component != source_temp_component);
  REX_ASSERT(temp2 != target_temp || temp2_component != target_temp_component);
  REX_ASSERT(temp2 != source_temp || temp2_component != source_temp_component);
  REX_ASSERT(temp1 != temp2 || temp1_component != temp2_component);
  dxbc::Dest target_dest(dxbc::Dest::R(target_temp, UINT32_C(1) << target_temp_component));
  dxbc::Src target_src(dxbc::Src::R(target_temp).Select(target_temp_component));
  dxbc::Src source_src(dxbc::Src::R(source_temp).Select(source_temp_component));
  dxbc::Dest temp1_dest(dxbc::Dest::R(temp1, UINT32_C(1) << temp1_component));
  dxbc::Src temp1_src(dxbc::Src::R(temp1).Select(temp1_component));
  dxbc::Dest temp2_dest(dxbc::Dest::R(temp2, UINT32_C(1) << temp2_component));
  dxbc::Src temp2_src(dxbc::Src::R(temp2).Select(temp2_component));

  // Compare with source >= threshold so a NaN input selects the 0...64/255 piece.
  a.OpGE(temp2_dest, source_src, dxbc::Src::LF(96.0f / 255.0f));
  a.OpIf(true, temp2_src);
  a.OpGE(temp2_dest, source_src, dxbc::Src::LF(192.0f / 255.0f));
  a.OpMovC(temp1_dest, temp2_src, dxbc::Src::LF(8.0f / 1024.0f), dxbc::Src::LF(4.0f / 1024.0f));
  a.OpMovC(temp2_dest, temp2_src, dxbc::Src::LF(-1024.0f), dxbc::Src::LF(-256.0f));
  a.OpElse();
  a.OpGE(temp2_dest, source_src, dxbc::Src::LF(64.0f / 255.0f));
  a.OpMovC(temp1_dest, temp2_src, dxbc::Src::LF(2.0f / 1024.0f), dxbc::Src::LF(1.0f / 1024.0f));
  a.OpMovC(temp2_dest, temp2_src, dxbc::Src::LF(-64.0f), dxbc::Src::LF(0.0f));
  a.OpEndIf();

  if (!source_pre_saturated) {
    a.OpMov(target_dest, source_src, true);
  }
  // Apply 1024 before the scale, which is at least 1/1024, so small values do not go denormal.
  a.OpMul(target_dest, source_pre_saturated ? source_src : target_src,
          dxbc::Src::LF(255.0f * 1024.0f));
  a.OpMAd(target_dest, target_src, temp1_src, temp2_src);
  a.OpMul(temp1_dest, target_src, temp1_src);
  a.OpRoundZ(temp1_dest, temp1_src);
  a.OpAdd(target_dest, target_src, temp1_src);
  a.OpMul(target_dest, target_src, dxbc::Src::LF(1.0f / 1023.0f));
}

void DXBCShaderTranslator::PreSaturatedLinearToPWLGamma(
    dxbc::Assembler& a, u32 target_temp, u32 target_temp_component, u32 source_temp,
    u32 source_temp_component, u32 temp_or_target, u32 temp_or_target_component,
    u32 temp_non_target, u32 temp_non_target_component) {
  // The source may be the same as the target but then cannot also be the temporary.
  REX_ASSERT(target_temp != source_temp || target_temp_component != source_temp_component ||
             target_temp != temp_or_target || target_temp_component != temp_or_target_component);
  REX_ASSERT(temp_or_target != source_temp || temp_or_target_component != source_temp_component);
  REX_ASSERT(temp_non_target != target_temp || temp_non_target_component != target_temp_component);
  REX_ASSERT(temp_non_target != source_temp || temp_non_target_component != source_temp_component);
  REX_ASSERT(temp_or_target != temp_non_target ||
             temp_or_target_component != temp_non_target_component);
  dxbc::Dest target_dest(dxbc::Dest::R(target_temp, UINT32_C(1) << target_temp_component));
  dxbc::Src target_src(dxbc::Src::R(target_temp).Select(target_temp_component));
  dxbc::Src source_src(dxbc::Src::R(source_temp).Select(source_temp_component));
  dxbc::Dest temp_or_target_dest(
      dxbc::Dest::R(temp_or_target, UINT32_C(1) << temp_or_target_component));
  dxbc::Src temp_or_target_src(dxbc::Src::R(temp_or_target).Select(temp_or_target_component));
  dxbc::Dest temp_non_target_dest(
      dxbc::Dest::R(temp_non_target, UINT32_C(1) << temp_non_target_component));
  dxbc::Src temp_non_target_src(dxbc::Src::R(temp_non_target).Select(temp_non_target_component));

  a.OpGE(temp_non_target_dest, source_src, dxbc::Src::LF(128.0f / 1023.0f));
  a.OpIf(true, temp_non_target_src);
  a.OpGE(temp_non_target_dest, source_src, dxbc::Src::LF(512.0f / 1023.0f));
  a.OpMovC(temp_or_target_dest, temp_non_target_src, dxbc::Src::LF(1023.0f / 8.0f),
           dxbc::Src::LF(1023.0f / 4.0f));
  a.OpMovC(temp_non_target_dest, temp_non_target_src, dxbc::Src::LF(128.0f / 255.0f),
           dxbc::Src::LF(64.0f / 255.0f));
  a.OpElse();
  a.OpGE(temp_non_target_dest, source_src, dxbc::Src::LF(64.0f / 1023.0f));
  a.OpMovC(temp_or_target_dest, temp_non_target_src, dxbc::Src::LF(1023.0f / 2.0f),
           dxbc::Src::LF(1023.0f));
  a.OpMovC(temp_non_target_dest, temp_non_target_src, dxbc::Src::LF(32.0f / 255.0f),
           dxbc::Src::LF(0.0f));
  a.OpEndIf();

  a.OpMul(target_dest, source_src, temp_or_target_src);
  a.OpRoundZ(target_dest, target_src);
  a.OpMAd(target_dest, target_src, dxbc::Src::LF(1.0f / 255.0f), temp_non_target_src);
}

void DXBCShaderTranslator::RemapAndConvertVertexIndices(u32 dest_temp, u32 dest_temp_components,
                                                        const dxbc::Src& src) {
  dxbc::Dest dest(dxbc::Dest::R(dest_temp, dest_temp_components));
  dxbc::Src dest_src(dxbc::Src::R(dest_temp));

  a_.OpIAdd(dest, src,
            LoadSystemConstant(SystemConstants::Index::kVertexIndexOffset,
                               offsetof(SystemConstants, vertex_index_offset), dxbc::Src::kXXXX));

  // The GPU uses only the lower 24 bits of the vertex index.
  a_.OpAnd(dest, dest_src, dxbc::Src::LU(xenos::kVertexIndexMask));

  a_.OpUMax(dest, dest_src,
            LoadSystemConstant(SystemConstants::Index::kVertexIndexMinMax,
                               offsetof(SystemConstants, vertex_index_min), dxbc::Src::kXXXX));
  a_.OpUMin(dest, dest_src,
            LoadSystemConstant(SystemConstants::Index::kVertexIndexMinMax,
                               offsetof(SystemConstants, vertex_index_max), dxbc::Src::kXXXX));

  a_.OpUToF(dest, dest_src);
}

void DXBCShaderTranslator::StartVertexShaderLoadVertexIndex() {
  if (GetRegisterCount() < 1) {
    return;
  }

  bool uses_register_dynamic_addressing = GetCurrentShader().UsesRegisterDynamicAddressing();

  u32 reg;
  if (uses_register_dynamic_addressing) {
    reg = PushSystemTemp();
  } else {
    reg = 0;
  }

  dxbc::Dest index_dest(dxbc::Dest::R(reg, 0b0001));
  dxbc::Src index_src(dxbc::Src::R(reg, dxbc::Src::kXXXX));

  a_.OpINE(
      index_dest, dxbc::Src::V1D(kInRegisterVSVertexIndex, dxbc::Src::kXXXX),
      LoadSystemConstant(SystemConstants::Index::kLineLoopClosingIndex,
                         offsetof(SystemConstants, line_loop_closing_index), dxbc::Src::kXXXX));
  a_.OpAnd(index_dest, dxbc::Src::V1D(kInRegisterVSVertexIndex, dxbc::Src::kXXXX), index_src);

  {
    dxbc::Src endian_src(LoadSystemConstant(SystemConstants::Index::kVertexIndexEndian,
                                            offsetof(SystemConstants, vertex_index_endian),
                                            dxbc::Src::kXXXX));
    dxbc::Dest swap_temp_dest(dxbc::Dest::R(reg, 0b0010));
    dxbc::Src swap_temp_src(dxbc::Src::R(reg, dxbc::Src::kYYYY));

    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian::k8in16)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian::k8in32)));
    a_.OpAnd(swap_temp_dest, index_src, dxbc::Src::LU(0x00FF00FF));
    a_.OpUShR(index_dest, index_src, dxbc::Src::LU(8));
    a_.OpAnd(index_dest, index_src, dxbc::Src::LU(0x00FF00FF));
    a_.OpUMAd(index_dest, swap_temp_src, dxbc::Src::LU(256), index_src);
    a_.OpBreak();
    a_.OpEndSwitch();

    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian::k8in32)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian::k16in32)));
    a_.OpUShR(swap_temp_dest, index_src, dxbc::Src::LU(16));
    a_.OpBFI(index_dest, dxbc::Src::LU(16), dxbc::Src::LU(16), index_src, swap_temp_src);
    a_.OpBreak();
    a_.OpEndSwitch();

    if (!uses_register_dynamic_addressing) {
      a_.OpMov(swap_temp_dest, dxbc::Src::LF(0.0f));
    }
  }

  RemapAndConvertVertexIndices(index_dest.index_1d.index, index_dest.write_mask, index_src);

  if (uses_register_dynamic_addressing) {
    a_.OpMov(dxbc::Dest::X(0, 0, 0b0001), index_src);
    PopSystemTemp();
  }
}

void DXBCShaderTranslator::StartVertexOrDomainShader() {
  bool uses_register_dynamic_addressing = GetCurrentShader().UsesRegisterDynamicAddressing();

  for (u32 i = 0; i < GetRegisterCount(); ++i) {
    a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, i) : dxbc::Dest::R(i),
             dxbc::Src::LF(0.0f));
  }

  u32 interpolator_count = rex::BitCount(GetModificationInterpolatorMask());
  for (u32 i = 0; i < interpolator_count; ++i) {
    a_.OpMov(dxbc::Dest::O(out_reg_vs_interpolators_ + i), dxbc::Src::LF(0.0f));
  }

  // x# registers are accessible only by mov load or store, so compute in a temporary.
  Shader::HostVertexShaderType host_vertex_shader_type =
      GetDXBCShaderModification().vertex.host_vertex_shader_type;
  switch (host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kVertex:
      StartVertexShaderLoadVertexIndex();
      break;

    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      REX_ASSERT(GetRegisterCount() >= 2);
      if (GetRegisterCount() >= 1) {
        in_domain_location_used_ |= 0b0111;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0111)
                                                  : dxbc::Dest::R(0, 0b0111),
                 dxbc::Src::VDomain(0b000110));
        if (GetRegisterCount() >= 2) {
          dxbc::Dest control_point_index_dest(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 1)
                                                                               : dxbc::Dest::R(1));
          in_control_point_index_used_ = true;
          for (u32 i = 0; i < 3; ++i) {
            a_.OpMov(control_point_index_dest.Mask(1 << i),
                     dxbc::Src::VICP(i, kInRegisterDSControlPointIndex, dxbc::Src::kXXXX));
          }
        }
      }
      break;

    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      REX_ASSERT(GetRegisterCount() >= 2);
      if (GetRegisterCount() >= 1) {
        in_domain_location_used_ |= 0b0111;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0111)
                                                  : dxbc::Dest::R(0, 0b0111),
                 dxbc::Src::VDomain(0b000110));
        if (GetRegisterCount() >= 2) {
          in_control_point_index_used_ = true;
          a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 1, 0b0001)
                                                    : dxbc::Dest::R(1, 0b0001),
                   dxbc::Src::VICP(0, kInRegisterDSControlPointIndex, dxbc::Src::kXXXX));
          // r1.y holds the barycentric swizzle, 1.0 ZXY, 2.0 YZX, 4.0 XZY, 5.0 YXZ, 6.0 ZYX.
          a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 1, 0b0010)
                                                    : dxbc::Dest::R(1, 0b0010),
                   dxbc::Src::LF(0.0f));
        }
      }
      break;

    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      REX_ASSERT(GetRegisterCount() >= 2);
      if (GetRegisterCount() >= 1) {
        in_domain_location_used_ |= 0b0011;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0011)
                                                  : dxbc::Dest::R(0, 0b0011),
                 dxbc::Src::VDomain());
        // Control point weights are r0.z (1-x)(1-y), r1.x x(1-y), r1.y xy, r1.z (1-x)y.
        in_control_point_index_used_ = true;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0100)
                                                  : dxbc::Dest::R(0, 0b0100),
                 dxbc::Src::VICP(0, kInRegisterDSControlPointIndex, dxbc::Src::kXXXX));
        if (GetRegisterCount() >= 2) {
          dxbc::Dest r1_dest(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 1)
                                                              : dxbc::Dest::R(1));
          for (u32 i = 0; i < 3; ++i) {
            a_.OpMov(r1_dest.Mask(1 << i),
                     dxbc::Src::VICP(1 + i, kInRegisterDSControlPointIndex, dxbc::Src::kXXXX));
          }
        }
      }
      break;

    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      REX_ASSERT(GetRegisterCount() >= 2);
      if (GetRegisterCount() >= 1) {
        in_domain_location_used_ |= 0b0011;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0110)
                                                  : dxbc::Dest::R(0, 0b0110),
                 dxbc::Src::VDomain(0b010000));
        in_control_point_index_used_ = true;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0001)
                                                  : dxbc::Dest::R(0, 0b0001),
                 dxbc::Src::VICP(0, kInRegisterDSControlPointIndex, dxbc::Src::kXXXX));
        if (GetRegisterCount() >= 2) {
          // r1.x holds the UV swizzle, 0.0 is identity, 1.0 is YXWZ, 2.0 is WZYX and 3.0 is ZWXY.
          a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 1, 0b0001)
                                                    : dxbc::Dest::R(1, 0b0001),
                   dxbc::Src::LF(0.0f));
        }
      }
      break;

    default:
      REX_UNHANDLED_CASE(host_vertex_shader_type);
      EmitTranslationError("Unsupported host vertex shader type in StartVertexOrDomainShader");
      break;
  }
}

void DXBCShaderTranslator::StartPixelShader() {
  if (edram_rov_used_) {
    StartPixelShaderLoadROVParameters();

    if (ROVIsDepthStencilEarly()) {
      ROVDepthStencilTest();
    } else {
      if (!GetCurrentShader().GetWritesDepth()) {
        REX_ASSERT(system_temp_depth_stencil_ != UINT32_MAX);
        dxbc::Src in_position_z(dxbc::Src::V1D(in_reg_ps_position_, dxbc::Src::kZZZZ));
        in_position_used_ |= 0b0100;
        a_.OpDerivRTXCoarse(dxbc::Dest::R(system_temp_depth_stencil_, 0b0001), in_position_z);
        a_.OpDerivRTYCoarse(dxbc::Dest::R(system_temp_depth_stencil_, 0b0010), in_position_z);
      }
    }
  }

  if (is_depth_only_pixel_shader_) {
    return;
  }

  bool uses_register_dynamic_addressing = GetCurrentShader().UsesRegisterDynamicAddressing();
  Modification shader_modification = GetDXBCShaderModification();

  u32 param_gen_interpolator =
      (shader_modification.pixel.param_gen_enable &&
       shader_modification.pixel.param_gen_interpolator < GetRegisterCount())
          ? shader_modification.pixel.param_gen_interpolator
          : UINT32_MAX;

  u32 interpolator_mask = GetModificationInterpolatorMask();
  for (u32 i = 0; i < GetRegisterCount(); ++i) {
    if (i == param_gen_interpolator) {
      continue;
    }
    a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, i) : dxbc::Dest::R(i),
             (i < xenos::kMaxInterpolators && (interpolator_mask & (UINT32_C(1) << i)))
                 ? dxbc::Src::V1D(in_reg_ps_interpolators_ +
                                  rex::BitCount(interpolator_mask & ((UINT32_C(1) << i) - 1)))
                 : dxbc::Src::LF(0.0f));
  }

  if (param_gen_interpolator != UINT32_MAX) {
    u32 param_gen_temp =
        uses_register_dynamic_addressing ? PushSystemTemp() : param_gen_interpolator;
    in_position_used_ |= 0b0011;
    a_.OpRoundNI(dxbc::Dest::R(param_gen_temp, 0b0011), dxbc::Src::V1D(in_reg_ps_position_));
    u32 resolution_scaled_axes = static_cast<u32>(draw_resolution_scale_x_ > 1) |
                                 (static_cast<u32>(draw_resolution_scale_y_ > 1) << 1);
    if (resolution_scaled_axes) {
      a_.OpMul(dxbc::Dest::R(param_gen_temp, resolution_scaled_axes), dxbc::Src::R(param_gen_temp),
               dxbc::Src::LF(1.0f / static_cast<f32>(draw_resolution_scale_x_),
                             1.0f / static_cast<f32>(draw_resolution_scale_y_), 1.0f, 1.0f));
    }
    if (shader_modification.pixel.param_gen_point) {
      a_.OpMov(dxbc::Dest::R(param_gen_temp, 0b0001),
               dxbc::Src::R(param_gen_temp, dxbc::Src::kXXXX).Abs());
      a_.OpMov(dxbc::Dest::R(param_gen_temp, 0b0010),
               -(dxbc::Src::R(param_gen_temp, dxbc::Src::kYYYY).Abs()));
      REX_ASSERT(in_reg_ps_point_coordinates_ != UINT32_MAX);
      a_.OpMov(dxbc::Dest::R(param_gen_temp, 0b1100),
               dxbc::Src::V1D(in_reg_ps_point_coordinates_, 0b0100 << 4), true);
    } else {
      a_.OpMov(dxbc::Dest::R(param_gen_temp, 0b0011), dxbc::Src::R(param_gen_temp).Abs());
      a_.OpAnd(dxbc::Dest::R(param_gen_temp, 0b0100), LoadFlagsSystemConstant(),
               dxbc::Src::LU(kSysFlagPrimitivePolygonal));
      a_.OpIf(true, dxbc::Src::R(param_gen_temp, dxbc::Src::kZZZZ));
      {
        // The negate modifier flips the sign bit even for 0, so set minus for back faces.
        in_front_face_used_ = true;
        a_.OpMovC(dxbc::Dest::R(param_gen_temp, 0b0001),
                  dxbc::Src::V1D(in_reg_ps_front_face_sample_index_, dxbc::Src::kXXXX),
                  dxbc::Src::R(param_gen_temp, dxbc::Src::kXXXX),
                  -dxbc::Src::R(param_gen_temp, dxbc::Src::kXXXX));
      }
      a_.OpEndIf();
      a_.OpUBFE(dxbc::Dest::R(param_gen_temp, 0b0100), dxbc::Src::LU(1),
                dxbc::Src::LU(kSysFlagPrimitiveLineShift), LoadFlagsSystemConstant());
      a_.OpIShL(dxbc::Dest::R(param_gen_temp, 0b0100),
                dxbc::Src::R(param_gen_temp, dxbc::Src::kZZZZ), dxbc::Src::LU(31));
      a_.OpMov(dxbc::Dest::R(param_gen_temp, 0b1000), dxbc::Src::LF(0.0f));
    }
    if (uses_register_dynamic_addressing) {
      a_.OpMov(dxbc::Dest::X(0, param_gen_interpolator), dxbc::Src::R(param_gen_temp));
      PopSystemTemp();
    }
  }

  if (GetCurrentShader().GetMemExportEMWritten()) {
    dxbc::Dest memexport_enabled_dest(
        dxbc::Dest::R(system_temp_memexport_enabled_and_em_written_, 0b0001));
    dxbc::Src memexport_enabled_src(
        dxbc::Src::R(system_temp_memexport_enabled_and_em_written_, dxbc::Src::kXXXX));
    u32 resolution_scaled_axes = static_cast<u32>(draw_resolution_scale_x_ > 1) |
                                 (static_cast<u32>(draw_resolution_scale_y_ > 1) << 1);
    if (resolution_scaled_axes) {
      u32 memexport_condition_temp = PushSystemTemp();
      in_position_used_ |= resolution_scaled_axes;
      a_.OpFToU(dxbc::Dest::R(memexport_condition_temp, resolution_scaled_axes),
                dxbc::Src::V1D(in_reg_ps_position_));
      a_.OpUDiv(dxbc::Dest::Null(), dxbc::Dest::R(memexport_condition_temp, resolution_scaled_axes),
                dxbc::Src::R(memexport_condition_temp),
                dxbc::Src::LU(draw_resolution_scale_x_, draw_resolution_scale_y_, 0, 0));
      a_.OpIEq(dxbc::Dest::R(memexport_condition_temp, resolution_scaled_axes),
               dxbc::Src::R(memexport_condition_temp),
               dxbc::Src::LU(draw_resolution_scale_x_ >> 1, draw_resolution_scale_y_ >> 1, 0, 0));
      for (u32 i = 0; i < 2; ++i) {
        if (!(resolution_scaled_axes & (1 << i))) {
          continue;
        }
        a_.OpAnd(memexport_enabled_dest, memexport_enabled_src,
                 dxbc::Src::R(memexport_condition_temp).Select(i));
      }
      PopSystemTemp();
    }
    // Only the sample with SV_SampleIndex == firstbit_lo(SV_Coverage) exports, ~0 if empty.
    if (IsSampleRate()) {
      u32 memexport_condition_temp = PushSystemTemp();
      a_.OpFirstBitLo(dxbc::Dest::R(memexport_condition_temp, 0b0001), dxbc::Src::VCoverage());
      a_.OpIEq(dxbc::Dest::R(memexport_condition_temp, 0b0001),
               dxbc::Src::V1D(in_reg_ps_front_face_sample_index_, dxbc::Src::kYYYY),
               dxbc::Src::R(memexport_condition_temp, dxbc::Src::kXXXX));
      a_.OpAnd(memexport_enabled_dest, memexport_enabled_src,
               dxbc::Src::R(memexport_condition_temp, dxbc::Src::kXXXX));
      PopSystemTemp();
    }
  }
}

void DXBCShaderTranslator::StartTranslation() {
  Modification shader_modification = GetDXBCShaderModification();
  u32 interpolator_register_mask = GetModificationInterpolatorMask();
  u32 interpolator_register_count = rex::BitCount(interpolator_register_mask);
  if (IsVertexShader()) {
    u32 out_reg_index = 0;
    if (interpolator_register_count) {
      out_reg_vs_interpolators_ = out_reg_index;
      out_reg_index += interpolator_register_count;
    }
    out_reg_vs_position_ = out_reg_index;
    ++out_reg_index;
    u32 clip_and_cull_distance_count = shader_modification.GetVertexClipDistanceCount() +
                                       shader_modification.GetVertexCullDistanceCount();
    if (clip_and_cull_distance_count) {
      out_reg_vs_clip_cull_distances_ = out_reg_index;
      out_reg_index += (clip_and_cull_distance_count + 3) >> 2;
    }
    if (shader_modification.vertex.output_point_size) {
      out_reg_vs_point_size_ = out_reg_index;
      ++out_reg_index;
    }
  } else if (IsPixelShader()) {
    u32 in_reg_index = 0;
    if (interpolator_register_count) {
      in_reg_ps_interpolators_ = in_reg_index;
      in_reg_index += interpolator_register_count;
    }
    if (shader_modification.pixel.param_gen_point) {
      in_reg_ps_point_coordinates_ = in_reg_index;
      ++in_reg_index;
    }
    in_reg_ps_position_ = in_reg_index;
    ++in_reg_index;
    in_reg_ps_front_face_sample_index_ = in_reg_index;
    ++in_reg_index;
  }

  if (IsVertexShader()) {
    system_temp_position_ = PushSystemTemp(0b1111);
    system_temp_point_size_edge_flag_kill_vertex_ = PushSystemTemp(0b0100);
    // A negative point size tells the geometry shader to use the default size.
    a_.OpMov(dxbc::Dest::R(system_temp_point_size_edge_flag_kill_vertex_, 0b0001),
             dxbc::Src::LF(-1.0f));
  } else if (IsPixelShader()) {
    if (edram_rov_used_) {
      system_temp_rov_params_ = PushSystemTemp();
    }
    if (IsDepthStencilSystemTempUsed()) {
      u32 depth_stencil_temp_zero_mask;
      if (GetCurrentShader().GetWritesDepth()) {
        depth_stencil_temp_zero_mask = 0b0001;
      } else {
        REX_ASSERT(edram_rov_used_);
        if (ROVIsDepthStencilEarly()) {
          depth_stencil_temp_zero_mask = 0b1111;
        } else {
          depth_stencil_temp_zero_mask = 0b0000;
        }
      }
      system_temp_depth_stencil_ = PushSystemTemp(depth_stencil_temp_zero_mask);
    }
    u32 shader_writes_color_targets = GetCurrentShader().GetWritesColorTargets();
    for (u32 i = 0; i < 4; ++i) {
      if (shader_writes_color_targets & (1 << i)) {
        system_temps_color_[i] = PushSystemTemp(0b1111);
      }
    }
  }

  u8 memexport_em_written = GetCurrentShader().GetMemExportEMWritten();
  if (memexport_em_written) {
    system_temp_memexport_enabled_and_em_written_ = PushSystemTemp(0b0010);
    // The memexport condition starts as 0 or UINT32_MAX by whether shared memory is bound as a UAV.
    a_.OpIBFE(dxbc::Dest::R(system_temp_memexport_enabled_and_em_written_, 0b0001),
              dxbc::Src::LU(1), dxbc::Src::LU(kSysFlagSharedMemoryIsUAVShift),
              LoadFlagsSystemConstant());
    system_temp_memexport_address_ = PushSystemTemp(0b1111);
    u8 memexport_em_remaining = memexport_em_written;
    u32 memexport_em_index;
    while (rex::bit_scan_forward(memexport_em_remaining, &memexport_em_index)) {
      memexport_em_remaining &= ~(static_cast<u8>(1) << memexport_em_index);
      system_temps_memexport_data_[memexport_em_index] = PushSystemTemp(0b1111);
    }
  }

  if (!is_depth_only_pixel_shader_) {
    system_temp_result_ = PushSystemTemp();
    system_temp_ps_pc_p0_a0_ = PushSystemTemp(0b1111);
    system_temp_al_ = PushSystemTemp(0b1111);
    system_temp_loop_count_ = PushSystemTemp(0b1111);
    system_temp_grad_h_lod_ = PushSystemTemp(0b1111);
    system_temp_grad_v_vfetch_address_ = PushSystemTemp(0b1111);
  }

  if (IsVertexShader()) {
    StartVertexOrDomainShader();
  } else if (IsPixelShader()) {
    StartPixelShader();
  }

  if (is_depth_only_pixel_shader_) {
    return;
  }

  a_.OpLoop();
  if (UseSwitchForControlFlow()) {
    a_.OpSwitch(dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kYYYY));
    a_.OpCase(dxbc::Src::LU(0));
  } else {
    a_.OpIf(false, dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kYYYY));
  }
}

void DXBCShaderTranslator::CompleteVertexOrDomainShader() {
  u32 temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));

  dxbc::Src flags_src(LoadFlagsSystemConstant());

  a_.OpAnd(temp_x_dest, flags_src, dxbc::Src::LU(kSysFlagWNotReciprocal));
  a_.OpIf(false, temp_x_src);
  a_.OpDiv(dxbc::Dest::R(system_temp_position_, 0b1000), dxbc::Src::LF(1.0f),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW));
  a_.OpEndIf();

  a_.OpAnd(temp_x_dest, flags_src, dxbc::Src::LU(kSysFlagXYDividedByW));
  a_.OpIf(true, temp_x_src);
  a_.OpMul(dxbc::Dest::R(system_temp_position_, 0b0011), dxbc::Src::R(system_temp_position_),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW));
  a_.OpEndIf();

  a_.OpAnd(temp_x_dest, flags_src, dxbc::Src::LU(kSysFlagZDividedByW));
  a_.OpIf(true, temp_x_src);
  a_.OpMul(dxbc::Dest::R(system_temp_position_, 0b0100),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kZZZZ),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW));
  a_.OpEndIf();

  Modification shader_modification = GetDXBCShaderModification();
  u32 clip_distance_next_component = 0;
  u32 cull_distance_next_component = shader_modification.GetVertexClipDistanceCount();

  u32& ucp_clip_cull_distance_next_component_ref = shader_modification.vertex.user_clip_plane_cull
                                                       ? cull_distance_next_component
                                                       : clip_distance_next_component;
  for (u32 i = 0; i < shader_modification.vertex.user_clip_plane_count; ++i) {
    a_.OpDP4(dxbc::Dest::O(
                 out_reg_vs_clip_cull_distances_ + (ucp_clip_cull_distance_next_component_ref >> 2),
                 UINT32_C(1) << (ucp_clip_cull_distance_next_component_ref & 3)),
             dxbc::Src::R(system_temp_position_),
             LoadSystemConstant(SystemConstants::Index::kUserClipPlanes,
                                offsetof(SystemConstants, user_clip_planes) + sizeof(f32) * 4 * i,
                                dxbc::Src::kXYZW));
    ++ucp_clip_cull_distance_next_component_ref;
  }

  a_.OpMul(dxbc::Dest::R(system_temp_position_, 0b0111), dxbc::Src::R(system_temp_position_),
           LoadSystemConstant(SystemConstants::Index::kNDCScale,
                              offsetof(SystemConstants, ndc_scale), 0b100100));

  a_.OpMAd(dxbc::Dest::R(system_temp_position_, 0b0111),
           LoadSystemConstant(SystemConstants::Index::kNDCOffset,
                              offsetof(SystemConstants, ndc_offset), 0b100100),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW),
           dxbc::Src::R(system_temp_position_));

  // Vertex kill register bits 0:30 non zero kill the vertex, tested with and to avoid denormals.
  bool shader_writes_vertex_kill =
      (GetCurrentShader().GetWritesPointSizeEdgeFlagKillVertex() & 0b100) != 0;
  if (shader_writes_vertex_kill) {
    a_.OpAnd(temp_x_dest,
             dxbc::Src::R(system_temp_point_size_edge_flag_kill_vertex_, dxbc::Src::kZZZZ),
             dxbc::Src::LU(UINT32_C(0x7FFFFFFF)));
  }
  if (shader_modification.vertex.vertex_kill_and) {
    dxbc::Dest vertex_kill_dest(
        dxbc::Dest::O(out_reg_vs_clip_cull_distances_ + (cull_distance_next_component >> 2),
                      UINT32_C(1) << (cull_distance_next_component & 3)));
    if (shader_writes_vertex_kill) {
      a_.OpMovC(vertex_kill_dest, temp_x_src, dxbc::Src::LF(-1.0f), dxbc::Src::LF(0.0f));
    } else {
      a_.OpMov(vertex_kill_dest, dxbc::Src::LF(0.0f));
    }
    ++cull_distance_next_component;
  } else {
    if (shader_writes_vertex_kill) {
      a_.OpMovC(dxbc::Dest::R(system_temp_position_, 0b1000), temp_x_src,
                dxbc::Src::LF(std::nanf("")),
                dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW));
    }
  }

  a_.OpMov(dxbc::Dest::O(out_reg_vs_position_), dxbc::Src::R(system_temp_position_));

  if (out_reg_vs_point_size_ != UINT32_MAX) {
    a_.OpMov(dxbc::Dest::O(out_reg_vs_point_size_, 0b0001),
             dxbc::Src::R(system_temp_point_size_edge_flag_kill_vertex_, dxbc::Src::kXXXX));
  }

  PopSystemTemp();
}

void DXBCShaderTranslator::CompleteShaderCode() {
  if (!is_depth_only_pixel_shader_) {
    CloseExecConditionals();
    if (UseSwitchForControlFlow()) {
      a_.OpBreak();
      a_.OpEndSwitch();
    } else {
      a_.OpEndIf();
    }
    a_.OpBreak();
    a_.OpEndLoop();

    PopSystemTemp(6);
  }

  u8 memexport_em_written = GetCurrentShader().GetMemExportEMWritten();
  if (memexport_em_written) {
    ExportToMemory(GetCurrentShader().GetMemExportEMPotentiallyWrittenBeforeEnd());

    PopSystemTemp(rex::BitCount(static_cast<u32>(memexport_em_written)) + 2);
  }

  if (IsVertexShader()) {
    CompleteVertexOrDomainShader();
  } else if (IsPixelShader()) {
    CompletePixelShader();
  }

  a_.OpRet();

  if (IsVertexShader()) {
    PopSystemTemp(2);
  } else if (IsPixelShader()) {
    u32 shader_writes_color_targets = GetCurrentShader().GetWritesColorTargets();
    for (i32 i = 3; i >= 0; --i) {
      if (shader_writes_color_targets & (1 << i)) {
        PopSystemTemp();
      }
    }
    if (IsDepthStencilSystemTempUsed()) {
      PopSystemTemp();
    }
    if (edram_rov_used_) {
      PopSystemTemp();
    }
  }
}

std::vector<u8> DXBCShaderTranslator::CompleteTranslation() {
  // shader_object_.resize() invalidates pointers and zeroes the new memory.

  CompleteShaderCode();

  shader_object_.clear();

  // The container holds 6 or 7 blobs, RDEF, ISGN, optional PCSG, OSGN, SHEX, SFI0 and STAT.
  u32 blob_count = 6 + static_cast<u32>(IsDXBCDomainShader());
  shader_object_.resize(sizeof(dxbc::ContainerHeader) / sizeof(u32) + blob_count);

  u32 blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(u32);
  u32 blob_position_dwords = static_cast<u32>(shader_object_.size());
  constexpr u32 kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(u32);

  shader_object_[blob_offset_position_dwords] =
      static_cast<u32>(blob_position_dwords * sizeof(u32));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteResourceDefinition();
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = static_cast<u32>(shader_object_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_object_[blob_offset_position_dwords++];
  }

  shader_object_[blob_offset_position_dwords] =
      static_cast<u32>(blob_position_dwords * sizeof(u32));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteInputSignature();
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = static_cast<u32>(shader_object_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_object_[blob_offset_position_dwords++];
  }

  if (IsDXBCDomainShader()) {
    shader_object_[blob_offset_position_dwords] =
        static_cast<u32>(blob_position_dwords * sizeof(u32));
    shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
    WritePatchConstantSignature();
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(shader_object_.data() + blob_position_dwords);
      blob_header.fourcc = dxbc::BlobHeader::FourCC::kPatchConstantSignature;
      blob_position_dwords = static_cast<u32>(shader_object_.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                               shader_object_[blob_offset_position_dwords++];
    }
  }

  shader_object_[blob_offset_position_dwords] =
      static_cast<u32>(blob_position_dwords * sizeof(u32));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteOutputSignature();
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignature;
    blob_position_dwords = static_cast<u32>(shader_object_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_object_[blob_offset_position_dwords++];
  }

  shader_object_[blob_offset_position_dwords] =
      static_cast<u32>(blob_position_dwords * sizeof(u32));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteShaderCode();
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = static_cast<u32>(shader_object_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_object_[blob_offset_position_dwords++];
  }

  shader_object_[blob_offset_position_dwords] =
      static_cast<u32>(blob_position_dwords * sizeof(u32));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords +
                        sizeof(dxbc::ShaderFeatureInfo) / sizeof(u32));
  std::memcpy(shader_object_.data() + blob_position_dwords + kBlobHeaderSizeDwords,
              &shader_feature_info_, sizeof(shader_feature_info_));
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderFeatureInfo;
    blob_position_dwords = static_cast<u32>(shader_object_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_object_[blob_offset_position_dwords++];
  }

  shader_object_[blob_offset_position_dwords] =
      static_cast<u32>(blob_position_dwords * sizeof(u32));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords +
                        sizeof(dxbc::Statistics) / sizeof(u32));
  std::memcpy(shader_object_.data() + blob_position_dwords + kBlobHeaderSizeDwords, &statistics_,
              sizeof(statistics_));
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = static_cast<u32>(shader_object_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_object_[blob_offset_position_dwords++];
  }

  u32 shader_object_size_bytes = static_cast<u32>(shader_object_.size() * sizeof(u32));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(shader_object_.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = shader_object_size_bytes;
    container_header.blob_count = blob_count;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(shader_object_.data()),
                          static_cast<unsigned int>(shader_object_size_bytes),
                          container_header.hash);
  }

  std::vector<u8> shader_object_bytes;
  shader_object_bytes.resize(shader_object_size_bytes);
  std::memcpy(shader_object_bytes.data(), shader_object_.data(), shader_object_size_bytes);
  return shader_object_bytes;
}

void DXBCShaderTranslator::PostTranslation() {
  Shader::Translation& translation = GetCurrentTranslation();
  if (!translation.IsValid()) {
    return;
  }
  DXBCShader* dxbc_shader = dynamic_cast<DXBCShader*>(&translation.GetShader());
  if (dxbc_shader &&
      !dxbc_shader->bindings_setup_entered_.test_and_set(std::memory_order_relaxed)) {
    dxbc_shader->translated_texture_bindings_.clear();
    dxbc_shader->translated_texture_bindings_.reserve(texture_bindings_.size());
    dxbc_shader->used_texture_mask_ = 0;
    for (const TextureBinding& translator_binding : texture_bindings_) {
      DXBCShader::TextureBinding& shader_binding =
          dxbc_shader->translated_texture_bindings_.emplace_back();
      std::memset(&shader_binding, 0, sizeof(shader_binding));
      shader_binding.bindless_descriptor_index = translator_binding.bindless_descriptor_index;
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.dimension = translator_binding.dimension;
      shader_binding.is_signed = translator_binding.is_signed;
      dxbc_shader->used_texture_mask_ |= 1u << translator_binding.fetch_constant;
    }
    dxbc_shader->sampler_bindings_.clear();
    dxbc_shader->sampler_bindings_.reserve(sampler_bindings_.size());
    for (const SamplerBinding& translator_binding : sampler_bindings_) {
      DXBCShader::SamplerBinding& shader_binding = dxbc_shader->sampler_bindings_.emplace_back();
      shader_binding.bindless_descriptor_index = translator_binding.bindless_descriptor_index;
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.mag_filter = translator_binding.mag_filter;
      shader_binding.min_filter = translator_binding.min_filter;
      shader_binding.mip_filter = translator_binding.mip_filter;
      shader_binding.aniso_filter = translator_binding.aniso_filter;
    }
  }
}

void DXBCShaderTranslator::EmitInstructionDisassembly() {
  if (!emit_source_map_) {
    return;
  }
  const char* source = instruction_disassembly_buffer_.GetBuffer();
  u32 length = static_cast<u32>(instruction_disassembly_buffer_.GetLength());
  while (length != 0 && source[0] == ' ') {
    ++source;
    --length;
  }
  while (length != 0 && source[length - 1] == '\n') {
    --length;
  }
  if (length == 0) {
    return;
  }
  char* dest =
      reinterpret_cast<char*>(a_.OpCustomData(dxbc::CustomDataClass::kComment, length + 1));
  std::memcpy(dest, source, length);
  dest[length] = '\0';
}

dxbc::Src DXBCShaderTranslator::LoadOperand(const InstructionOperand& operand,
                                            u32 needed_components, bool& temp_pushed_out) {
  temp_pushed_out = false;

  u32 first_needed_component;
  if (!rex::bit_scan_forward(needed_components, &first_needed_component)) {
    return dxbc::Src::LF(0.0f);
  }

  dxbc::Index index(operand.storage_index);
  switch (operand.storage_addressing_mode) {
    case InstructionStorageAddressingMode::kAbsolute:
      break;
    case InstructionStorageAddressingMode::kAddressRegisterRelative:
      index = dxbc::Index(system_temp_ps_pc_p0_a0_, 3, operand.storage_index);
      break;
    case InstructionStorageAddressingMode::kLoopRelative:
      index = dxbc::Index(system_temp_al_, 0, operand.storage_index);
      break;
  }

  dxbc::Src src(dxbc::Src::LF(0.0f));
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister: {
      if (GetCurrentShader().UsesRegisterDynamicAddressing()) {
        u32 temp = PushSystemTemp();
        temp_pushed_out = true;
        u32 used_swizzle_components = 0;
        for (u32 i = 0; i < u32(operand.component_count); ++i) {
          if (!(needed_components & (1 << i))) {
            continue;
          }
          SwizzleSource component = operand.GetComponent(i);
          REX_ASSERT(component >= SwizzleSource::kX && component <= SwizzleSource::kW);
          used_swizzle_components |=
              1 << (static_cast<u32>(component) - std::to_underlying(SwizzleSource::kX));
        }
        REX_ASSERT(used_swizzle_components != 0);
        a_.OpMov(dxbc::Dest::R(temp, used_swizzle_components), dxbc::Src::X(0, index));
        src = dxbc::Src::R(temp);
      } else {
        REX_ASSERT(operand.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute);
        src = dxbc::Src::R(index.index);
      }
    } break;
    case InstructionStorageSource::kConstantFloat: {
      if (cbuffer_index_float_constants_ == kBindingIndexUnallocated) {
        cbuffer_index_float_constants_ = cbuffer_count_++;
      }
      const Shader::ConstantRegisterMap& constant_register_map =
          GetCurrentShader().GetConstantRegisterMap();
      if (operand.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute) {
        u32 float_constant_index =
            constant_register_map.GetPackedFloatConstantIndex(operand.storage_index);
        REX_ASSERT(float_constant_index != UINT32_MAX);
        if (float_constant_index == UINT32_MAX) {
          return dxbc::Src::LF(0.0f);
        }
        index.index = float_constant_index;
      } else {
        REX_ASSERT(constant_register_map.float_dynamic_addressing);
      }
      src = dxbc::Src::CB(cbuffer_index_float_constants_,
                          std::to_underlying(CbufferRegister::kFloatConstants), index);
    } break;
    default:
      REX_UNHANDLED_CASE(operand.storage_source);
      return dxbc::Src::LF(0.0f);
  }

  u32 swizzle = 0;
  for (u32 i = 0; i < 4; ++i) {
    SwizzleSource component =
        operand.GetComponent((needed_components & (1 << i)) ? i : first_needed_component);
    REX_ASSERT(component >= SwizzleSource::kX && component <= SwizzleSource::kW);
    swizzle |= (static_cast<u32>(component) - std::to_underlying(SwizzleSource::kX)) << (i * 2);
  }
  src = src.Swizzle(swizzle);

  return src.WithModifiers(operand.is_absolute_value, operand.is_negated);
}

void DXBCShaderTranslator::StoreResult(const InstructionResult& result, const dxbc::Src& src,
                                       bool can_store_memexport_address) {
  u32 used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }

  dxbc::Dest dest(dxbc::Dest::Null());
  bool is_clamped = result.is_clamped;
  switch (result.storage_target) {
    case InstructionStorageTarget::kNone:
      return;
    case InstructionStorageTarget::kRegister:
      if (GetCurrentShader().UsesRegisterDynamicAddressing()) {
        dxbc::Index register_index(result.storage_index);
        switch (result.storage_addressing_mode) {
          case InstructionStorageAddressingMode::kAbsolute:
            break;
          case InstructionStorageAddressingMode::kAddressRegisterRelative:
            register_index = dxbc::Index(system_temp_ps_pc_p0_a0_, 3, result.storage_index);
            break;
          case InstructionStorageAddressingMode::kLoopRelative:
            register_index = dxbc::Index(system_temp_al_, 0, result.storage_index);
            break;
        }
        dest = dxbc::Dest::X(0, register_index);
      } else {
        REX_ASSERT(result.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute);
        dest = dxbc::Dest::R(result.storage_index);
      }
      break;
    case InstructionStorageTarget::kInterpolator: {
      u32 interpolator_mask = GetModificationInterpolatorMask();
      u32 interpolator_bit = UINT32_C(1) << result.storage_index;
      if (interpolator_mask & interpolator_bit) {
        dest = dxbc::Dest::O(out_reg_vs_interpolators_ +
                             rex::BitCount(interpolator_mask & (interpolator_bit - 1)));
      }
    } break;
    case InstructionStorageTarget::kPosition:
      dest = dxbc::Dest::R(system_temp_position_);
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      REX_ASSERT((used_write_mask & 0b1000) == 0);
      dest = dxbc::Dest::R(system_temp_point_size_edge_flag_kill_vertex_);
      break;
    case InstructionStorageTarget::kExportAddress:
      if (!can_store_memexport_address) {
        return;
      }
      if (!GetCurrentShader().GetMemExportEMWritten()) {
        return;
      }
      dest = dxbc::Dest::R(system_temp_memexport_address_);
      break;
    case InstructionStorageTarget::kExportData: {
      REX_ASSERT((GetCurrentShader().GetMemExportEMWritten() &
                  (static_cast<u8>(1) << result.storage_index)) != 0);
      dest = dxbc::Dest::R(system_temps_memexport_data_[result.storage_index]);
      REX_ASSERT(used_write_mask != 0);
      a_.OpOr(dxbc::Dest::R(system_temp_memexport_enabled_and_em_written_, 0b0010),
              dxbc::Src::R(system_temp_memexport_enabled_and_em_written_, dxbc::Src::kYYYY),
              dxbc::Src::LU(static_cast<u8>(1) << result.storage_index));
    } break;
    case InstructionStorageTarget::kColor:
      REX_ASSERT(used_write_mask != 0);
      REX_ASSERT(GetCurrentShader().GetWritesColorTarget(result.storage_index));
      dest = dxbc::Dest::R(system_temps_color_[result.storage_index]);
      if (edram_rov_used_) {
        // Unwritten targets stay intact through flow control, unwritten components are undefined.
        a_.OpOr(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
                dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                dxbc::Src::LU(static_cast<u32>(1) << (8 + result.storage_index)));
      }
      break;
    case InstructionStorageTarget::kDepth:
      REX_ASSERT(used_write_mask == 0b0001);
      REX_ASSERT(GetCurrentShader().GetWritesDepth());
      if (IsDepthStencilSystemTempUsed()) {
        dest = dxbc::Dest::R(system_temp_depth_stencil_);
      } else {
        dest = dxbc::Dest::ODepth();
      }
      // Depth outside [0, 1] is unsafe for the ROV code, 20e4 as 32 conversion and float24 remap.
      is_clamped = true;
      break;
  }
  if (dest.type == dxbc::OperandType::kNull) {
    return;
  }

  u32 src_additional_swizzle = 0;
  u32 constant_mask = 0, constant_1_mask = 0;
  for (u32 i = 0; i < 4; ++i) {
    if (!(used_write_mask & (1 << i))) {
      continue;
    }
    SwizzleSource component = result.components[i];
    if (component >= SwizzleSource::kX && component <= SwizzleSource::kW) {
      src_additional_swizzle |=
          (static_cast<u32>(component) - std::to_underlying(SwizzleSource::kX)) << (i * 2);
    } else {
      constant_mask |= 1 << i;
      if (component == SwizzleSource::k1) {
        constant_1_mask |= 1 << i;
      }
    }
  }
  if (used_write_mask != constant_mask) {
    a_.OpMov(dest.Mask(used_write_mask & ~constant_mask),
             src.SwizzleSwizzled(src_additional_swizzle), is_clamped);
  }
  if (constant_mask) {
    a_.OpMov(dest.Mask(constant_mask), dxbc::Src::LF(static_cast<f32>(constant_1_mask & 1),
                                                     static_cast<f32>((constant_1_mask >> 1) & 1),
                                                     static_cast<f32>((constant_1_mask >> 2) & 1),
                                                     static_cast<f32>((constant_1_mask >> 3) & 1)));
  }

  // Clamp like R400 as signed int32, -NaN and -Infinity...-0 to the minimum, +NaN to the maximum.
  if (result.storage_target == InstructionStorageTarget::kPointSizeEdgeFlagKillVertex &&
      (used_write_mask & 0b0001)) {
    a_.OpIMax(
        dxbc::Dest::R(system_temp_point_size_edge_flag_kill_vertex_, 0b0001),
        LoadSystemConstant(SystemConstants::Index::kPointVertexDiameterMin,
                           offsetof(SystemConstants, point_vertex_diameter_min), dxbc::Src::kXXXX),
        dxbc::Src::R(system_temp_point_size_edge_flag_kill_vertex_, dxbc::Src::kXXXX));
    a_.OpIMin(
        dxbc::Dest::R(system_temp_point_size_edge_flag_kill_vertex_, 0b0001),
        LoadSystemConstant(SystemConstants::Index::kPointVertexDiameterMax,
                           offsetof(SystemConstants, point_vertex_diameter_max), dxbc::Src::kXXXX),
        dxbc::Src::R(system_temp_point_size_edge_flag_kill_vertex_, dxbc::Src::kXXXX));
  }
}

void DXBCShaderTranslator::UpdateExecConditionalsAndEmitDisassembly(
    ParsedExecInstruction::Type type, u32 bool_constant_index, bool condition) {
  bool merge = false;
  if (type == ParsedExecInstruction::Type::kConditional) {
    if (cf_exec_bool_constant_ == bool_constant_index &&
        cf_exec_bool_constant_condition_ == condition) {
      merge = true;
    }
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    // Predicated execs merge only when conditions match and the predicate register is unmodified.
    if (!cf_exec_predicate_written_ && cf_exec_predicated_ &&
        cf_exec_predicate_condition_ == condition) {
      merge = true;
    }
  } else {
    if (cf_exec_bool_constant_ == kCfExecBoolConstantNone && !cf_exec_predicated_) {
      merge = true;
    }
  }

  if (merge) {
    EmitInstructionDisassembly();
    return;
  }

  CloseExecConditionals();

  EmitInstructionDisassembly();

  if (type == ParsedExecInstruction::Type::kConditional) {
    u32 bool_constant_test_temp = PushSystemTemp();
    if (cbuffer_index_bool_loop_constants_ == kBindingIndexUnallocated) {
      cbuffer_index_bool_loop_constants_ = cbuffer_count_++;
    }
    a_.OpAnd(dxbc::Dest::R(bool_constant_test_temp, 0b0001),
             dxbc::Src::CB(cbuffer_index_bool_loop_constants_,
                           std::to_underlying(CbufferRegister::kBoolLoopConstants),
                           bool_constant_index >> 7)
                 .Select((bool_constant_index >> 5) & 3),
             dxbc::Src::LU(static_cast<u32>(1) << (bool_constant_index & 31)));
    a_.OpIf(condition, dxbc::Src::R(bool_constant_test_temp, dxbc::Src::kXXXX));
    PopSystemTemp();
    cf_exec_bool_constant_ = bool_constant_index;
    cf_exec_bool_constant_condition_ = condition;
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    a_.OpIf(condition, dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ));
    cf_exec_predicated_ = true;
    cf_exec_predicate_condition_ = condition;
  }
}

void DXBCShaderTranslator::CloseExecConditionals() {
  CloseInstructionPredication();
  if (cf_exec_bool_constant_ != kCfExecBoolConstantNone || cf_exec_predicated_) {
    a_.OpEndIf();
    cf_exec_bool_constant_ = kCfExecBoolConstantNone;
    cf_exec_predicated_ = false;
  }
  cf_exec_predicate_written_ = false;
}

void DXBCShaderTranslator::UpdateInstructionPredicationAndEmitDisassembly(bool predicated,
                                                                          bool condition) {
  if (!predicated) {
    CloseInstructionPredication();
    EmitInstructionDisassembly();
    return;
  }

  if (cf_instruction_predicate_if_open_) {
    if (cf_instruction_predicate_condition_ == condition) {
      EmitInstructionDisassembly();
      return;
    }
    CloseInstructionPredication();
  }

  EmitInstructionDisassembly();

  if (!cf_exec_predicate_written_ && cf_exec_predicated_ &&
      cf_exec_predicate_condition_ == condition) {
    return;
  }

  a_.OpIf(condition, dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ));
  cf_instruction_predicate_if_open_ = true;
  cf_instruction_predicate_condition_ = condition;
}

void DXBCShaderTranslator::CloseInstructionPredication() {
  if (cf_instruction_predicate_if_open_) {
    a_.OpEndIf();
    cf_instruction_predicate_if_open_ = false;
  }
}

void DXBCShaderTranslator::JumpToLabel(u32 address) {
  a_.OpMov(dxbc::Dest::R(system_temp_ps_pc_p0_a0_, 0b0010), dxbc::Src::LU(address));
  a_.OpContinue();
}

void DXBCShaderTranslator::ProcessLabel(u32 cf_index) {
  if (cf_index == 0) {
    return;
  }
  CloseExecConditionals();
  if (UseSwitchForControlFlow()) {
    // A non empty switch case must end with a break in DXBC, so the fallthrough is explicit.
    JumpToLabel(cf_index);
    a_.OpBreak();
    a_.OpCase(dxbc::Src::LU(cf_index));
  } else {
    a_.OpEndIf();
    u32 test_temp = PushSystemTemp();
    a_.OpUGE(dxbc::Dest::R(test_temp, 0b0001), dxbc::Src::LU(cf_index),
             dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kYYYY));
    a_.OpIf(true, dxbc::Src::R(test_temp, dxbc::Src::kXXXX));
    PopSystemTemp();
  }
}

void DXBCShaderTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr) {
  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
  }
  UpdateExecConditionalsAndEmitDisassembly(instr.type, instr.bool_constant_index, instr.condition);
}

void DXBCShaderTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr) {
  if (instr.is_end) {
    CloseInstructionPredication();
    if (UseSwitchForControlFlow()) {
      a_.OpMov(dxbc::Dest::R(system_temp_ps_pc_p0_a0_, 0b0010), dxbc::Src::LU(UINT32_MAX));
      a_.OpContinue();
    } else {
      a_.OpBreak();
    }
  }
}

void DXBCShaderTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) {
  CloseExecConditionals();

  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
    EmitInstructionDisassembly();
  }

  // Loop constant bits 0:7 are the count, 8:15 the initial aL, at vector 2 after bool constants.
  if (cbuffer_index_bool_loop_constants_ == kBindingIndexUnallocated) {
    cbuffer_index_bool_loop_constants_ = cbuffer_count_++;
  }
  dxbc::Src loop_constant_src(dxbc::Src::CB(cbuffer_index_bool_loop_constants_,
                                            std::to_underlying(CbufferRegister::kBoolLoopConstants),
                                            2 + (instr.loop_constant_index >> 2))
                                  .Select(instr.loop_constant_index & 3));

  {
    u32 loop_count_temp = PushSystemTemp();
    a_.OpAnd(dxbc::Dest::R(loop_count_temp, 0b0001), loop_constant_src, dxbc::Src::LU(UINT8_MAX));

    a_.OpIf(false, dxbc::Src::R(loop_count_temp, dxbc::Src::kXXXX));
    JumpToLabel(instr.loop_skip_address);
    a_.OpEndIf();

    a_.OpMov(dxbc::Dest::R(system_temp_loop_count_, 0b1110),
             dxbc::Src::R(system_temp_loop_count_, 0b10010000));
    a_.OpMov(dxbc::Dest::R(system_temp_loop_count_, 0b0001),
             dxbc::Src::R(loop_count_temp, dxbc::Src::kXXXX));

    PopSystemTemp();
  }

  a_.OpMov(dxbc::Dest::R(system_temp_al_, instr.is_repeat ? 0b1111 : 0b1110),
           dxbc::Src::R(system_temp_al_, 0b10010000));
  if (!instr.is_repeat) {
    a_.OpUBFE(dxbc::Dest::R(system_temp_al_, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(8),
              loop_constant_src);
  }
}

void DXBCShaderTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) {
  CloseExecConditionals();

  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
    EmitInstructionDisassembly();
  }

  a_.OpIAdd(dxbc::Dest::R(system_temp_loop_count_, 0b0001),
            dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX), dxbc::Src::LI(-1));

  if (instr.is_predicated_break) {
    u32 break_case_temp = PushSystemTemp();
    if (instr.predicate_condition) {
      a_.OpMovC(dxbc::Dest::R(break_case_temp, 0b0001),
                dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ), dxbc::Src::LU(0),
                dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX));
    } else {
      a_.OpMovC(dxbc::Dest::R(break_case_temp, 0b0001),
                dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ),
                dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX), dxbc::Src::LU(0));
    }
    a_.OpIf(false, dxbc::Src::R(break_case_temp, dxbc::Src::kXXXX));
    PopSystemTemp();
  } else {
    a_.OpIf(false, dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX));
  }
  {
    a_.OpMov(dxbc::Dest::R(system_temp_loop_count_, 0b0111),
             dxbc::Src::R(system_temp_loop_count_, 0b111001));
    a_.OpMov(dxbc::Dest::R(system_temp_loop_count_, 0b1000), dxbc::Src::LU(0));
    a_.OpMov(dxbc::Dest::R(system_temp_al_, 0b0111), dxbc::Src::R(system_temp_al_, 0b111001));
    a_.OpMov(dxbc::Dest::R(system_temp_al_, 0b1000), dxbc::Src::LI(0));
  }
  a_.OpElse();
  {
    u32 al_add_temp = PushSystemTemp();
    // The signed aL increment is in bits 16:23 of the loop constant.
    if (cbuffer_index_bool_loop_constants_ == kBindingIndexUnallocated) {
      cbuffer_index_bool_loop_constants_ = cbuffer_count_++;
    }
    a_.OpIBFE(dxbc::Dest::R(al_add_temp, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(16),
              dxbc::Src::CB(cbuffer_index_bool_loop_constants_,
                            std::to_underlying(CbufferRegister::kBoolLoopConstants),
                            2 + (instr.loop_constant_index >> 2))
                  .Select(instr.loop_constant_index & 3));
    a_.OpIAdd(dxbc::Dest::R(system_temp_al_, 0b0001),
              dxbc::Src::R(system_temp_al_, dxbc::Src::kXXXX),
              dxbc::Src::R(al_add_temp, dxbc::Src::kXXXX));
    PopSystemTemp();
    JumpToLabel(instr.loop_body_address);
  }
  a_.OpEndIf();
}

void DXBCShaderTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr) {
  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
  }

  ParsedExecInstruction::Type type;
  if (instr.type == ParsedJumpInstruction::Type::kConditional) {
    type = ParsedExecInstruction::Type::kConditional;
  } else if (instr.type == ParsedJumpInstruction::Type::kPredicated) {
    type = ParsedExecInstruction::Type::kPredicated;
  } else {
    type = ParsedExecInstruction::Type::kUnconditional;
  }
  UpdateExecConditionalsAndEmitDisassembly(type, instr.bool_constant_index, instr.condition);

  CloseInstructionPredication();

  JumpToLabel(instr.target_address);
}

void DXBCShaderTranslator::ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                                                   u8 export_em) {
  bool start_memexport =
      instr.type == AllocType::kMemory && GetCurrentShader().GetMemExportEMWritten();
  if (export_em || start_memexport) {
    CloseExecConditionals();
  }

  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
    EmitInstructionDisassembly();
  }

  if (export_em) {
    ExportToMemory(export_em);
    a_.OpMov(dxbc::Dest::R(system_temp_memexport_enabled_and_em_written_, 0b0010),
             dxbc::Src::LU(0));
    u8 export_em_remaining = export_em;
    u32 em_index;
    while (rex::bit_scan_forward(export_em_remaining, &em_index)) {
      export_em_remaining &= ~(static_cast<u8>(1) << em_index);
      a_.OpMov(dxbc::Dest::R(system_temps_memexport_data_[em_index]), dxbc::Src::LF(0.0f));
    }
  }

  if (start_memexport) {
    a_.OpMov(dxbc::Dest::R(system_temp_memexport_address_), dxbc::Src::LU(0));
  }
}

const DXBCShaderTranslator::ShaderRdefType DXBCShaderTranslator::kRDEFTypes[static_cast<size_t>(
    DXBCShaderTranslator::ShaderRdefTypeIndex::kCount)] = {
    {"float", dxbc::RdefVariableClass::kScalar, dxbc::RdefVariableType::kFloat, 1, 1, 0,
     ShaderRdefTypeIndex::kUnknown},
    {"float2", dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kFloat, 1, 2, 0,
     ShaderRdefTypeIndex::kUnknown},
    {"float3", dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kFloat, 1, 3, 0,
     ShaderRdefTypeIndex::kUnknown},
    {"float4", dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kFloat, 1, 4, 0,
     ShaderRdefTypeIndex::kUnknown},
    {"dword", dxbc::RdefVariableClass::kScalar, dxbc::RdefVariableType::kUInt, 1, 1, 0,
     ShaderRdefTypeIndex::kUnknown},
    {"uint2", dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kUInt, 1, 2, 0,
     ShaderRdefTypeIndex::kUnknown},
    {"uint4", dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kUInt, 1, 4, 0,
     ShaderRdefTypeIndex::kUnknown},
    {nullptr, dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kFloat, 1, 4, 4,
     ShaderRdefTypeIndex::kFloat4},
    {nullptr, dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kFloat, 1, 4, 6,
     ShaderRdefTypeIndex::kFloat4},
    {nullptr, dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kFloat, 1, 4, 0,
     ShaderRdefTypeIndex::kFloat4},
    {nullptr, dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kUInt, 1, 4, 2,
     ShaderRdefTypeIndex::kUint4},
    {nullptr, dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kUInt, 1, 4, 8,
     ShaderRdefTypeIndex::kUint4},
    {nullptr, dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kUInt, 1, 4, 48,
     ShaderRdefTypeIndex::kUint4},
    {nullptr, dxbc::RdefVariableClass::kVector, dxbc::RdefVariableType::kUInt, 1, 4, 0,
     ShaderRdefTypeIndex::kUint4},
};

const DXBCShaderTranslator::SystemConstantRdef
    DXBCShaderTranslator::kSystemConstantRDEF[static_cast<size_t>(
        DXBCShaderTranslator::SystemConstants::Index::kCount)] = {
        {"xe_flags", ShaderRdefTypeIndex::kUint, sizeof(u32)},
        {"xe_tessellation_factor_range", ShaderRdefTypeIndex::kFloat2, sizeof(f32) * 2},
        {"xe_line_loop_closing_index", ShaderRdefTypeIndex::kUint, sizeof(u32)},

        {"xe_vertex_index_endian", ShaderRdefTypeIndex::kUint, sizeof(u32)},
        {"xe_vertex_index_offset", ShaderRdefTypeIndex::kUint, sizeof(i32)},
        {"xe_vertex_index_min_max", ShaderRdefTypeIndex::kUint2, sizeof(u32) * 2},

        {"xe_user_clip_planes", ShaderRdefTypeIndex::kFloat4Array6, sizeof(f32) * 4 * 6},

        {"xe_ndc_scale", ShaderRdefTypeIndex::kFloat3, sizeof(f32) * 3},
        {"xe_point_vertex_diameter_min", ShaderRdefTypeIndex::kFloat, sizeof(f32)},

        {"xe_ndc_offset", ShaderRdefTypeIndex::kFloat3, sizeof(f32) * 3},
        {"xe_point_vertex_diameter_max", ShaderRdefTypeIndex::kFloat, sizeof(f32)},

        {"xe_point_constant_diameter", ShaderRdefTypeIndex::kFloat2, sizeof(f32) * 2},
        {"xe_point_screen_diameter_to_ndc_radius", ShaderRdefTypeIndex::kFloat2, sizeof(f32) * 2},

        {"xe_texture_swizzled_signs", ShaderRdefTypeIndex::kUint4Array2, sizeof(u32) * 4 * 2},

        {"xe_textures_resolution_scaled", ShaderRdefTypeIndex::kUint, sizeof(u32)},
        {"xe_sample_count_log2", ShaderRdefTypeIndex::kUint2, sizeof(u32) * 2},
        {"xe_alpha_test_reference", ShaderRdefTypeIndex::kFloat, sizeof(f32)},

        {"xe_alpha_to_mask", ShaderRdefTypeIndex::kUint, sizeof(u32)},
        {"xe_edram_32bpp_tile_pitch_dwords_scaled", ShaderRdefTypeIndex::kUint, sizeof(u32)},
        {"xe_edram_depth_base_dwords_scaled", ShaderRdefTypeIndex::kUint, sizeof(u32), sizeof(u32)},

        {"xe_color_exp_bias", ShaderRdefTypeIndex::kFloat4, sizeof(f32) * 4},

        {"xe_edram_poly_offset_front", ShaderRdefTypeIndex::kFloat2, sizeof(f32) * 2},
        {"xe_edram_poly_offset_back", ShaderRdefTypeIndex::kFloat2, sizeof(f32) * 2},

        {"xe_edram_stencil", ShaderRdefTypeIndex::kUint4Array2, sizeof(u32) * 4 * 2},

        {"xe_edram_rt_base_dwords_scaled", ShaderRdefTypeIndex::kUint4, sizeof(u32) * 4},

        {"xe_edram_rt_format_flags", ShaderRdefTypeIndex::kUint4, sizeof(u32) * 4},

        {"xe_edram_rt_clamp", ShaderRdefTypeIndex::kFloat4Array4, sizeof(f32) * 4 * 4},

        {"xe_edram_rt_keep_mask", ShaderRdefTypeIndex::kUint4Array2, sizeof(u32) * 4 * 2},

        {"xe_edram_rt_blend_factors_ops", ShaderRdefTypeIndex::kUint4, sizeof(u32) * 4},

        {"xe_edram_blend_constant", ShaderRdefTypeIndex::kFloat4, sizeof(f32) * 4},
};

void DXBCShaderTranslator::WriteResourceDefinition() {
  // shader_object_.resize() invalidates pointers and zeroes the new memory.

  u32 blob_position_dwords = static_cast<u32>(shader_object_.size());
  u32 name_ptr;

  const Shader::ConstantRegisterMap& constant_register_map =
      GetCurrentShader().GetConstantRegisterMap();

  shader_object_.resize(shader_object_.size() + sizeof(dxbc::RdefHeader) / sizeof(u32));
  dxbc::AppendAlignedString(shader_object_, "ReXGlue");

  name_ptr = (static_cast<u32>(shader_object_.size()) - blob_position_dwords) * sizeof(u32);
  u32 type_name_ptrs[std::to_underlying(ShaderRdefTypeIndex::kCount)];
  for (u32 i = 0; i < std::to_underlying(ShaderRdefTypeIndex::kCount); ++i) {
    const ShaderRdefType& type = kRDEFTypes[i];
    if (type.name == nullptr) {
      REX_ASSERT(static_cast<u32>(type.array_element_type) < i);
      type_name_ptrs[i] = type_name_ptrs[static_cast<u32>(type.array_element_type)];
      continue;
    }
    type_name_ptrs[i] = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_object_, type.name);
  }
  u32 types_position_dwords = static_cast<u32>(shader_object_.size());
  u32 types_ptr = (types_position_dwords - blob_position_dwords) * sizeof(u32);
  shader_object_.resize(types_position_dwords +
                        sizeof(dxbc::RdefType) / sizeof(u32) *
                            std::to_underlying(ShaderRdefTypeIndex::kCount));
  {
    auto types = reinterpret_cast<dxbc::RdefType*>(shader_object_.data() + types_position_dwords);
    for (u32 i = 0; i < std::to_underlying(ShaderRdefTypeIndex::kCount); ++i) {
      dxbc::RdefType& type = types[i];
      const ShaderRdefType& translator_type = kRDEFTypes[i];
      type.variable_class = translator_type.variable_class;
      type.variable_type = translator_type.variable_type;
      type.row_count = translator_type.row_count;
      type.column_count = translator_type.column_count;
      switch (ShaderRdefTypeIndex(i)) {
        case ShaderRdefTypeIndex::kFloat4ConstantArray:
          type.element_count =
              std::max(static_cast<u16>(constant_register_map.float_count), static_cast<u16>(1));
          break;
        case ShaderRdefTypeIndex::kUint4DescriptorIndexArray:
          type.element_count = std::max(static_cast<u16>((GetBindlessResourceCount() + 3) >> 2),
                                        static_cast<u16>(1));
          break;
        default:
          type.element_count = translator_type.element_count;
      }
      type.name_ptr = type_name_ptrs[i];
    }
  }

  name_ptr = (static_cast<u32>(shader_object_.size()) - blob_position_dwords) * sizeof(u32);
  u32 constant_name_ptrs_system[std::to_underlying(SystemConstants::Index::kCount)];
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    for (size_t i = 0; i < size_t(SystemConstants::Index::kCount); ++i) {
      constant_name_ptrs_system[i] = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, kSystemConstantRDEF[i].name);
    }
  }
  u32 constant_name_ptr_float = name_ptr;
  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_float_constants");
  }
  u32 constant_name_ptr_bool = name_ptr;
  u32 constant_name_ptr_loop = name_ptr;
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_bool_constants");
    constant_name_ptr_loop = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_loop_constants");
  }
  u32 constant_name_ptr_fetch = name_ptr;
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_fetch_constants");
  }
  u32 constant_name_ptr_descriptor_indices = name_ptr;
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_descriptor_indices");
  }

  u32 constant_position_dwords_system = static_cast<u32>(shader_object_.size());
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    shader_object_.resize(constant_position_dwords_system +
                          sizeof(dxbc::RdefVariable) / sizeof(u32) *
                              static_cast<size_t>(SystemConstants::Index::kCount));
    auto constants_system = reinterpret_cast<dxbc::RdefVariable*>(shader_object_.data() +
                                                                  constant_position_dwords_system);
    u32 constant_offset_system = 0;
    for (size_t i = 0; i < size_t(SystemConstants::Index::kCount); ++i) {
      dxbc::RdefVariable& constant_system = constants_system[i];
      const SystemConstantRdef& translator_constant_system = kSystemConstantRDEF[i];
      constant_system.name_ptr = constant_name_ptrs_system[i];
      constant_system.start_offset_bytes = constant_offset_system;
      constant_system.size_bytes = translator_constant_system.size;
      constant_system.flags =
          (system_constants_used_ & (static_cast<u64>(1) << i)) ? dxbc::kRdefVariableFlagUsed : 0;
      constant_system.type_ptr =
          types_ptr + sizeof(dxbc::RdefType) * static_cast<u32>(translator_constant_system.type);
      constant_system.start_texture = UINT32_MAX;
      constant_system.start_sampler = UINT32_MAX;
      constant_offset_system +=
          translator_constant_system.size + translator_constant_system.padding_after;
    }
  }

  u32 constant_position_dwords_float = static_cast<u32>(shader_object_.size());
  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    REX_ASSERT(constant_register_map.float_count != 0);
    shader_object_.resize(constant_position_dwords_float +
                          sizeof(dxbc::RdefVariable) / sizeof(u32));
    auto& constant_float = *reinterpret_cast<dxbc::RdefVariable*>(shader_object_.data() +
                                                                  constant_position_dwords_float);
    constant_float.name_ptr = constant_name_ptr_float;
    constant_float.size_bytes = sizeof(f32) * 4 * constant_register_map.float_count;
    constant_float.flags = dxbc::kRdefVariableFlagUsed;
    constant_float.type_ptr =
        types_ptr +
        sizeof(dxbc::RdefType) * std::to_underlying(ShaderRdefTypeIndex::kFloat4ConstantArray);
    constant_float.start_texture = UINT32_MAX;
    constant_float.start_sampler = UINT32_MAX;
  }

  u32 constant_position_dwords_bool_loop = static_cast<u32>(shader_object_.size());
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    shader_object_.resize(constant_position_dwords_bool_loop +
                          sizeof(dxbc::RdefVariable) / sizeof(u32) * 2);
    auto constants_bool_loop = reinterpret_cast<dxbc::RdefVariable*>(
        shader_object_.data() + constant_position_dwords_bool_loop);

    constants_bool_loop[0].name_ptr = constant_name_ptr_bool;
    constants_bool_loop[0].size_bytes = sizeof(u32) * 4 * 2;
    if (std::ranges::any_of(constant_register_map.bool_bitmap, std::identity{})) {
      constants_bool_loop[0].flags |= dxbc::kRdefVariableFlagUsed;
    }
    constants_bool_loop[0].type_ptr =
        types_ptr + sizeof(dxbc::RdefType) * std::to_underlying(ShaderRdefTypeIndex::kUint4Array2);
    constants_bool_loop[0].start_texture = UINT32_MAX;
    constants_bool_loop[0].start_sampler = UINT32_MAX;

    constants_bool_loop[1].name_ptr = constant_name_ptr_loop;
    constants_bool_loop[1].start_offset_bytes = sizeof(u32) * 4 * 2;
    constants_bool_loop[1].size_bytes = sizeof(u32) * 4 * 8;
    constants_bool_loop[1].flags =
        constant_register_map.loop_bitmap ? dxbc::kRdefVariableFlagUsed : 0;
    constants_bool_loop[1].type_ptr =
        types_ptr + sizeof(dxbc::RdefType) * std::to_underlying(ShaderRdefTypeIndex::kUint4Array8);
    constants_bool_loop[1].start_texture = UINT32_MAX;
    constants_bool_loop[1].start_sampler = UINT32_MAX;
  }

  u32 constant_position_dwords_fetch = static_cast<u32>(shader_object_.size());
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    shader_object_.resize(constant_position_dwords_fetch +
                          sizeof(dxbc::RdefVariable) / sizeof(u32));
    auto& constant_fetch = *reinterpret_cast<dxbc::RdefVariable*>(shader_object_.data() +
                                                                  constant_position_dwords_fetch);
    constant_fetch.name_ptr = constant_name_ptr_fetch;
    constant_fetch.size_bytes = sizeof(u32) * 6 * 32;
    constant_fetch.flags = dxbc::kRdefVariableFlagUsed;
    constant_fetch.type_ptr =
        types_ptr + sizeof(dxbc::RdefType) * std::to_underlying(ShaderRdefTypeIndex::kUint4Array48);
    constant_fetch.start_texture = UINT32_MAX;
    constant_fetch.start_sampler = UINT32_MAX;
  }

  u32 constant_position_dwords_descriptor_indices = static_cast<u32>(shader_object_.size());
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    REX_ASSERT(GetBindlessResourceCount() != 0);
    shader_object_.resize(constant_position_dwords_descriptor_indices +
                          sizeof(dxbc::RdefVariable) / sizeof(u32));
    auto& constant_descriptor_indices = *reinterpret_cast<dxbc::RdefVariable*>(
        shader_object_.data() + constant_position_dwords_descriptor_indices);
    constant_descriptor_indices.name_ptr = constant_name_ptr_descriptor_indices;
    constant_descriptor_indices.size_bytes =
        sizeof(u32) * rex::Align(GetBindlessResourceCount(), static_cast<u32>(4));
    constant_descriptor_indices.flags = dxbc::kRdefVariableFlagUsed;
    constant_descriptor_indices.type_ptr =
        types_ptr + sizeof(dxbc::RdefType) *
                        std::to_underlying(ShaderRdefTypeIndex::kUint4DescriptorIndexArray);
    constant_descriptor_indices.start_texture = UINT32_MAX;
    constant_descriptor_indices.start_sampler = UINT32_MAX;
  }

  name_ptr = (static_cast<u32>(shader_object_.size()) - blob_position_dwords) * sizeof(u32);
  u32 cbuffer_name_ptr_system = name_ptr;
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_system_cbuffer");
  }
  u32 cbuffer_name_ptr_float = name_ptr;
  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_float_cbuffer");
  }
  u32 cbuffer_name_ptr_bool_loop = name_ptr;
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_bool_loop_cbuffer");
  }
  u32 cbuffer_name_ptr_fetch = name_ptr;
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_fetch_cbuffer");
  }
  u32 cbuffer_name_ptr_descriptor_indices = name_ptr;
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_descriptor_indices_cbuffer");
  }

  u32 cbuffers_position_dwords = static_cast<u32>(shader_object_.size());
  shader_object_.resize(cbuffers_position_dwords +
                        sizeof(dxbc::RdefCbuffer) / sizeof(u32) * cbuffer_count_);
  {
    auto cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(shader_object_.data() + cbuffers_position_dwords);
    for (u32 i = 0; i < cbuffer_count_; ++i) {
      dxbc::RdefCbuffer& cbuffer = cbuffers[i];
      cbuffer.type = dxbc::RdefCbufferType::kCbuffer;
      if (i == cbuffer_index_system_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_system;
        cbuffer.variable_count = std::to_underlying(SystemConstants::Index::kCount);
        cbuffer.variables_ptr =
            (constant_position_dwords_system - blob_position_dwords) * sizeof(u32);
        cbuffer.size_vector_aligned_bytes =
            static_cast<u32>(rex::Align(sizeof(SystemConstants), sizeof(u32) * 4));
      } else if (i == cbuffer_index_float_constants_) {
        REX_ASSERT(constant_register_map.float_count != 0);
        cbuffer.name_ptr = cbuffer_name_ptr_float;
        cbuffer.variable_count = 1;
        cbuffer.variables_ptr =
            (constant_position_dwords_float - blob_position_dwords) * sizeof(u32);
        cbuffer.size_vector_aligned_bytes = sizeof(f32) * 4 * constant_register_map.float_count;
      } else if (i == cbuffer_index_bool_loop_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_bool_loop;
        cbuffer.variable_count = 2;
        cbuffer.variables_ptr =
            (constant_position_dwords_bool_loop - blob_position_dwords) * sizeof(u32);
        cbuffer.size_vector_aligned_bytes = sizeof(u32) * 4 * (2 + 8);
      } else if (i == cbuffer_index_fetch_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_fetch;
        cbuffer.variable_count = 1;
        cbuffer.variables_ptr =
            (constant_position_dwords_fetch - blob_position_dwords) * sizeof(u32);
        cbuffer.size_vector_aligned_bytes = sizeof(u32) * 6 * 32;
      } else if (i == cbuffer_index_descriptor_indices_) {
        REX_ASSERT(GetBindlessResourceCount() != 0);
        cbuffer.name_ptr = cbuffer_name_ptr_descriptor_indices;
        cbuffer.variable_count = 1;
        cbuffer.variables_ptr =
            (constant_position_dwords_descriptor_indices - blob_position_dwords) * sizeof(u32);
        cbuffer.size_vector_aligned_bytes =
            sizeof(u32) * rex::Align(GetBindlessResourceCount(), static_cast<u32>(4));
      } else {
        REX_UNHANDLED_CASE(i);
      }
    }
  }

  name_ptr = (static_cast<u32>(shader_object_.size()) - blob_position_dwords) * sizeof(u32);
  u32 sampler_name_ptr = name_ptr;
  if (!sampler_bindings_.empty()) {
    if (bindless_resources_used_) {
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_samplers");
    } else {
      for (u32 i = 0; i < u32(sampler_bindings_.size()); ++i) {
        name_ptr +=
            dxbc::AppendAlignedString(shader_object_, sampler_bindings_[i].bindful_name.c_str());
      }
    }
  }
  u32 shared_memory_srv_name_ptr = name_ptr;
  if (srv_index_shared_memory_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_shared_memory_srv");
  }
  u32 bindless_textures_2d_name_ptr = name_ptr;
  u32 bindless_textures_3d_name_ptr = name_ptr;
  u32 bindless_textures_cube_name_ptr = name_ptr;
  if (bindless_resources_used_) {
    if (srv_index_bindless_textures_2d_ != kBindingIndexUnallocated) {
      bindless_textures_2d_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_textures_2d");
    }
    if (srv_index_bindless_textures_3d_ != kBindingIndexUnallocated) {
      bindless_textures_3d_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_textures_3d");
    }
    if (srv_index_bindless_textures_cube_ != kBindingIndexUnallocated) {
      bindless_textures_cube_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_textures_cube");
    }
  } else {
    for (TextureBinding& texture_binding : texture_bindings_) {
      texture_binding.bindful_srv_rdef_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, texture_binding.bindful_name.c_str());
    }
  }
  u32 shared_memory_uav_name_ptr = name_ptr;
  if (uav_index_shared_memory_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_shared_memory_uav");
  }
  u32 edram_name_ptr = name_ptr;
  if (uav_index_edram_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_edram");
  }

  u32 bindings_position_dwords = static_cast<u32>(shader_object_.size());

  if (!sampler_bindings_.empty()) {
    u32 samplers_position_dwords = static_cast<u32>(shader_object_.size());
    shader_object_.resize(samplers_position_dwords +
                          sizeof(dxbc::RdefInputBind) / sizeof(u32) *
                              (bindless_resources_used_ ? 1 : sampler_bindings_.size()));
    auto samplers =
        reinterpret_cast<dxbc::RdefInputBind*>(shader_object_.data() + samplers_position_dwords);
    if (bindless_resources_used_) {
      samplers[0].name_ptr = sampler_name_ptr;
      samplers[0].type = dxbc::RdefInputType::kSampler;
    } else {
      u32 sampler_current_name_ptr = sampler_name_ptr;
      for (size_t i = 0; i < sampler_bindings_.size(); ++i) {
        dxbc::RdefInputBind& sampler = samplers[i];
        sampler.name_ptr = sampler_current_name_ptr;
        sampler.type = dxbc::RdefInputType::kSampler;
        sampler.bind_point = static_cast<u32>(i);
        sampler.bind_count = 1;
        sampler.id = static_cast<u32>(i);
        sampler_current_name_ptr +=
            dxbc::GetAlignedStringLength(sampler_bindings_[i].bindful_name.c_str());
      }
    }
  }

  u32 srvs_position_dwords = static_cast<u32>(shader_object_.size());
  shader_object_.resize(srvs_position_dwords +
                        sizeof(dxbc::RdefInputBind) / sizeof(u32) * srv_count_);
  {
    auto srvs =
        reinterpret_cast<dxbc::RdefInputBind*>(shader_object_.data() + srvs_position_dwords);
    for (u32 i = 0; i < srv_count_; ++i) {
      dxbc::RdefInputBind& srv = srvs[i];
      srv.id = i;
      if (i == srv_index_shared_memory_) {
        srv.name_ptr = shared_memory_srv_name_ptr;
        srv.type = dxbc::RdefInputType::kByteAddress;
        srv.return_type = dxbc::ResourceReturnType::kMixed;
        srv.dimension = dxbc::RdefDimension::kSRVBuffer;
        srv.bind_point = std::to_underlying(SRVMainRegister::kSharedMemory);
        srv.bind_count = 1;
        srv.bind_point_space = std::to_underlying(SRVSpace::kMain);
      } else {
        srv.type = dxbc::RdefInputType::kTexture;
        srv.return_type = dxbc::ResourceReturnType::kFloat;
        srv.sample_count = UINT32_MAX;
        srv.flags = dxbc::kRdefInputFlags4Component;
        if (bindless_resources_used_) {
          if (i == srv_index_bindless_textures_3d_) {
            srv.name_ptr = bindless_textures_3d_name_ptr;
            srv.dimension = dxbc::RdefDimension::kSRVTexture3D;
            srv.bind_point_space = std::to_underlying(SRVSpace::kBindlessTextures3D);
          } else if (i == srv_index_bindless_textures_cube_) {
            srv.name_ptr = bindless_textures_cube_name_ptr;
            srv.dimension = dxbc::RdefDimension::kSRVTextureCube;
            srv.bind_point_space = std::to_underlying(SRVSpace::kBindlessTexturesCube);
          } else {
            REX_ASSERT(i == srv_index_bindless_textures_2d_);
            srv.name_ptr = bindless_textures_2d_name_ptr;
            srv.dimension = dxbc::RdefDimension::kSRVTexture2DArray;
            srv.bind_point_space = std::to_underlying(SRVSpace::kBindlessTextures2DArray);
          }
        } else {
          auto it = texture_bindings_for_bindful_srv_indices_.find(i);
          REX_ASSERT(it != texture_bindings_for_bindful_srv_indices_.end());
          u32 texture_binding_index = it->second;
          const TextureBinding& texture_binding = texture_bindings_[texture_binding_index];
          srv.name_ptr = texture_binding.bindful_srv_rdef_name_ptr;
          switch (texture_binding.dimension) {
            case xenos::FetchOpDimension::k3DOrStacked:
              srv.dimension = dxbc::RdefDimension::kSRVTexture3D;
              break;
            case xenos::FetchOpDimension::kCube:
              srv.dimension = dxbc::RdefDimension::kSRVTextureCube;
              break;
            default:
              REX_ASSERT(texture_binding.dimension == xenos::FetchOpDimension::k2D);
              srv.dimension = dxbc::RdefDimension::kSRVTexture2DArray;
          }
          srv.bind_point =
              std::to_underlying(SRVMainRegister::kBindfulTexturesStart) + texture_binding_index;
          srv.bind_count = 1;
          srv.bind_point_space = std::to_underlying(SRVSpace::kMain);
        }
      }
    }
  }

  u32 uavs_position_dwords = static_cast<u32>(shader_object_.size());
  shader_object_.resize(uavs_position_dwords +
                        sizeof(dxbc::RdefInputBind) / sizeof(u32) * uav_count_);
  {
    auto uavs =
        reinterpret_cast<dxbc::RdefInputBind*>(shader_object_.data() + uavs_position_dwords);
    for (u32 i = 0; i < uav_count_; ++i) {
      dxbc::RdefInputBind& uav = uavs[i];
      uav.bind_count = 1;
      uav.id = i;
      if (i == uav_index_shared_memory_) {
        uav.name_ptr = shared_memory_uav_name_ptr;
        uav.type = dxbc::RdefInputType::kUAVRWByteAddress;
        uav.return_type = dxbc::ResourceReturnType::kMixed;
        uav.dimension = dxbc::RdefDimension::kUAVBuffer;
        uav.bind_point = std::to_underlying(UAVRegister::kSharedMemory);
      } else if (i == uav_index_edram_) {
        uav.name_ptr = edram_name_ptr;
        uav.type = dxbc::RdefInputType::kUAVRWTyped;
        uav.return_type = dxbc::ResourceReturnType::kUInt;
        uav.dimension = dxbc::RdefDimension::kUAVBuffer;
        uav.sample_count = UINT32_MAX;
        uav.bind_point = std::to_underlying(UAVRegister::kEDRAM);
      } else {
        REX_UNHANDLED_CASE(i);
      }
    }
  }

  u32 cbuffer_binding_position_dwords = static_cast<u32>(shader_object_.size());
  shader_object_.resize(cbuffer_binding_position_dwords +
                        sizeof(dxbc::RdefInputBind) / sizeof(u32) * cbuffer_count_);
  {
    auto cbuffers = reinterpret_cast<dxbc::RdefInputBind*>(shader_object_.data() +
                                                           cbuffer_binding_position_dwords);
    for (u32 i = 0; i < cbuffer_count_; ++i) {
      dxbc::RdefInputBind& cbuffer = cbuffers[i];
      cbuffer.type = dxbc::RdefInputType::kCbuffer;
      cbuffer.bind_count = 1;
      cbuffer.flags = dxbc::kRdefInputFlagUserPacked;
      cbuffer.id = i;
      if (i == cbuffer_index_system_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_system;
        cbuffer.bind_point = std::to_underlying(CbufferRegister::kSystemConstants);
      } else if (i == cbuffer_index_float_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_float;
        cbuffer.bind_point = std::to_underlying(CbufferRegister::kFloatConstants);
      } else if (i == cbuffer_index_bool_loop_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_bool_loop;
        cbuffer.bind_point = std::to_underlying(CbufferRegister::kBoolLoopConstants);
      } else if (i == cbuffer_index_fetch_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_fetch;
        cbuffer.bind_point = std::to_underlying(CbufferRegister::kFetchConstants);
      } else if (i == cbuffer_index_descriptor_indices_) {
        cbuffer.name_ptr = cbuffer_name_ptr_descriptor_indices;
        cbuffer.bind_point = std::to_underlying(CbufferRegister::kDescriptorIndices);
      } else {
        REX_UNHANDLED_CASE(i);
      }
    }
  }

  u32 bindings_end_position_dwords = static_cast<u32>(shader_object_.size());

  {
    auto& header =
        *reinterpret_cast<dxbc::RdefHeader*>(shader_object_.data() + blob_position_dwords);
    header.cbuffer_count = cbuffer_count_;
    header.cbuffers_ptr = (cbuffers_position_dwords - blob_position_dwords) * sizeof(u32);
    header.input_bind_count = (bindings_end_position_dwords - bindings_position_dwords) *
                              sizeof(u32) / sizeof(dxbc::RdefInputBind);
    header.input_binds_ptr = (bindings_position_dwords - blob_position_dwords) * sizeof(u32);
    if (IsDXBCVertexShader()) {
      header.shader_model = dxbc::RdefShaderModel::kVertexShader51;
    } else if (IsDXBCDomainShader()) {
      header.shader_model = dxbc::RdefShaderModel::kDomainShader51;
    } else {
      REX_ASSERT(IsPixelShader());
      header.shader_model = dxbc::RdefShaderModel::kPixelShader51;
    }
    header.compile_flags = dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
                           dxbc::kCompileFlagIeeeStrictness;
    if (bindless_resources_used_) {
      header.compile_flags |= dxbc::kCompileFlagEnableUnboundedDescriptorTables;
    }
    header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    header.fourcc = dxbc::RdefHeader::FourCC::k51;
    header.InitializeSizes();
  }
}

void DXBCShaderTranslator::WriteInputSignature() {
  // shader_object_.resize() invalidates pointers and zeroes the new memory.
  u32 blob_position = static_cast<u32>(shader_object_.size());
  shader_object_.resize(shader_object_.size() + sizeof(dxbc::Signature) / sizeof(u32));
  u32 parameter_count = 0;
  constexpr size_t kParameterDwords = sizeof(dxbc::SignatureParameter) / sizeof(u32);

  if (IsDXBCVertexShader()) {
    size_t vertex_id_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& vertex_id =
          *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + vertex_id_position);
      vertex_id.system_value = dxbc::Name::kVertexID;
      vertex_id.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      vertex_id.register_index = kInRegisterVSVertexIndex;
      vertex_id.mask = 0b0001;
      vertex_id.always_reads_mask = (GetRegisterCount() >= 1) ? 0b0001 : 0b0000;
    }

    u32 semantic_offset = static_cast<u32>((shader_object_.size() - blob_position) * sizeof(u32));
    {
      auto& vertex_id =
          *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + vertex_id_position);
      vertex_id.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_VertexID");
  } else if (IsDXBCDomainShader()) {
    size_t control_point_index_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& control_point_index = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + control_point_index_position);
      control_point_index.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      control_point_index.register_index = kInRegisterDSControlPointIndex;
      control_point_index.mask = 0b0001;
      control_point_index.always_reads_mask = in_control_point_index_used_ ? 0b0001 : 0b0000;
    }

    u32 semantic_offset = static_cast<u32>((shader_object_.size() - blob_position) * sizeof(u32));
    {
      auto& control_point_index = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + control_point_index_position);
      control_point_index.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "XEVERTEXID");
  } else if (IsPixelShader()) {
    size_t interpolator_position = shader_object_.size();
    u32 interpolator_mask = GetModificationInterpolatorMask();
    u32 interpolator_count = rex::BitCount(interpolator_mask);
    shader_object_.resize(shader_object_.size() + interpolator_count * kParameterDwords);
    parameter_count += interpolator_count;
    {
      auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                       interpolator_position);
      u32 used_interpolator_index = 0;
      u32 interpolators_remaining = interpolator_mask;
      u32 interpolator_index;
      while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
        interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
        dxbc::SignatureParameter& interpolator = interpolators[used_interpolator_index];
        interpolator.semantic_index = used_interpolator_index;
        interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        interpolator.register_index = in_reg_ps_interpolators_ + used_interpolator_index;
        interpolator.mask = 0b1111;
        interpolator.always_reads_mask = interpolator_index < GetRegisterCount() ? 0b1111 : 0b0000;
        ++used_interpolator_index;
      }
    }

    size_t point_coordinates_position = shader_object_.size();
    if (in_reg_ps_point_coordinates_ != UINT32_MAX) {
      shader_object_.resize(shader_object_.size() + kParameterDwords);
      ++parameter_count;
      {
        auto& point_coordinates = *reinterpret_cast<dxbc::SignatureParameter*>(
            shader_object_.data() + point_coordinates_position);
        point_coordinates.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        point_coordinates.register_index = in_reg_ps_point_coordinates_;
        point_coordinates.mask = 0b0011;
        point_coordinates.always_reads_mask = 0b0011;
      }
    }

    size_t position_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& position =
          *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + position_position);
      position.system_value = dxbc::Name::kPosition;
      position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      position.register_index = in_reg_ps_position_;
      position.mask = 0b1111;
      position.always_reads_mask = in_position_used_;
    }

    size_t is_front_face_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& is_front_face = *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                         is_front_face_position);
      is_front_face.system_value = dxbc::Name::kIsFrontFace;
      is_front_face.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      is_front_face.register_index = in_reg_ps_front_face_sample_index_;
      is_front_face.mask = 0b0001;
      is_front_face.always_reads_mask = in_front_face_used_ ? 0b0001 : 0b0000;
    }

    size_t sample_index_position = SIZE_MAX;
    if (GetCurrentShader().GetMemExportEMWritten() && IsSampleRate()) {
      size_t sample_index_position = shader_object_.size();
      shader_object_.resize(shader_object_.size() + kParameterDwords);
      ++parameter_count;
      {
        auto& sample_index = *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                          sample_index_position);
        sample_index.system_value = dxbc::Name::kSampleIndex;
        sample_index.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
        sample_index.register_index = in_reg_ps_front_face_sample_index_;
        sample_index.mask = 0b0010;
        sample_index.always_reads_mask = 0b0010;
      }
    }

    u32 semantic_offset = static_cast<u32>((shader_object_.size() - blob_position) * sizeof(u32));
    if (interpolator_count) {
      auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                       interpolator_position);
      for (u32 i = 0; i < interpolator_count; ++i) {
        interpolators[i].semantic_name_ptr = semantic_offset;
      }
      semantic_offset += dxbc::AppendAlignedString(shader_object_, "TEXCOORD");
    }
    if (in_reg_ps_point_coordinates_ != UINT32_MAX) {
      auto& point_coordinates = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + point_coordinates_position);
      point_coordinates.semantic_name_ptr = semantic_offset;
      semantic_offset += dxbc::AppendAlignedString(shader_object_, "XESPRITETEXCOORD");
    }
    {
      auto& position =
          *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + position_position);
      position.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_Position");
    {
      auto& is_front_face = *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                         is_front_face_position);
      is_front_face.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_IsFrontFace");
    if (sample_index_position != SIZE_MAX) {
      {
        auto& sample_index = *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                          sample_index_position);
        sample_index.semantic_name_ptr = semantic_offset;
      }
      semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_SampleIndex");
    }
  }

  {
    auto& header = *reinterpret_cast<dxbc::Signature*>(shader_object_.data() + blob_position);
    header.parameter_count = parameter_count;
    header.parameter_info_ptr = sizeof(dxbc::Signature);
  }
}

void DXBCShaderTranslator::WritePatchConstantSignature() {
  REX_ASSERT(IsDXBCDomainShader());
  // shader_object_.resize() invalidates pointers and zeroes the new memory.
  u32 blob_position = static_cast<u32>(shader_object_.size());
  shader_object_.resize(shader_object_.size() + sizeof(dxbc::Signature) / sizeof(u32));
  u32 parameter_count = 0;
  constexpr size_t kParameterDwords = sizeof(dxbc::SignatureParameter) / sizeof(u32);

  // FXC compiles with SV_TessFactor and SV_InsideTessFactor inputs, required if unreferenced.
  u32 tess_factor_edge_count = 0;
  dxbc::Name tess_factor_edge_system_value = dxbc::Name::kUndefined;
  u32 tess_factor_inside_count = 0;
  dxbc::Name tess_factor_inside_system_value = dxbc::Name::kUndefined;
  Shader::HostVertexShaderType host_vertex_shader_type =
      GetDXBCShaderModification().vertex.host_vertex_shader_type;
  switch (host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      tess_factor_edge_count = 3;
      tess_factor_edge_system_value = dxbc::Name::kFinalTriEdgeTessFactor;
      tess_factor_inside_count = 1;
      tess_factor_inside_system_value = dxbc::Name::kFinalTriInsideTessFactor;
      break;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      tess_factor_edge_count = 4;
      tess_factor_edge_system_value = dxbc::Name::kFinalQuadEdgeTessFactor;
      tess_factor_inside_count = 2;
      tess_factor_inside_system_value = dxbc::Name::kFinalQuadInsideTessFactor;
      break;
    default:
      REX_UNHANDLED_CASE(host_vertex_shader_type);
      EmitTranslationError("Unsupported host vertex shader type in WritePatchConstantSignature");
  }

  size_t tess_factor_edge_position = shader_object_.size();
  shader_object_.resize(shader_object_.size() + tess_factor_edge_count * kParameterDwords);
  parameter_count += tess_factor_edge_count;
  {
    auto tess_factors_edge = reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                         tess_factor_edge_position);
    for (u32 i = 0; i < tess_factor_edge_count; ++i) {
      dxbc::SignatureParameter& tess_factor_edge = tess_factors_edge[i];
      tess_factor_edge.semantic_index = i;
      tess_factor_edge.system_value = tess_factor_edge_system_value;
      tess_factor_edge.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      tess_factor_edge.register_index = i;
      tess_factor_edge.mask = 0b0001;
    }
  }

  size_t tess_factor_inside_position = shader_object_.size();
  shader_object_.resize(shader_object_.size() + tess_factor_inside_count * kParameterDwords);
  parameter_count += tess_factor_inside_count;
  {
    auto tess_factors_inside = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_object_.data() + tess_factor_inside_position);
    for (u32 i = 0; i < tess_factor_inside_count; ++i) {
      dxbc::SignatureParameter& tess_factor_inside = tess_factors_inside[i];
      tess_factor_inside.semantic_index = i;
      tess_factor_inside.system_value = tess_factor_inside_system_value;
      tess_factor_inside.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      tess_factor_inside.register_index = tess_factor_edge_count + i;
      tess_factor_inside.mask = 0b0001;
    }
  }

  u32 semantic_offset = static_cast<u32>((shader_object_.size() - blob_position) * sizeof(u32));
  {
    auto tess_factors_edge = reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                         tess_factor_edge_position);
    for (u32 i = 0; i < tess_factor_edge_count; ++i) {
      tess_factors_edge[i].semantic_name_ptr = semantic_offset;
    }
  }
  semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_TessFactor");
  {
    auto tess_factors_inside = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_object_.data() + tess_factor_inside_position);
    for (u32 i = 0; i < tess_factor_inside_count; ++i) {
      tess_factors_inside[i].semantic_name_ptr = semantic_offset;
    }
  }
  semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_InsideTessFactor");

  {
    auto& header = *reinterpret_cast<dxbc::Signature*>(shader_object_.data() + blob_position);
    header.parameter_count = parameter_count;
    header.parameter_info_ptr = sizeof(dxbc::Signature);
  }
}

void DXBCShaderTranslator::WriteOutputSignature() {
  // shader_object_.resize() invalidates pointers and zeroes the new memory.
  u32 blob_position = static_cast<u32>(shader_object_.size());
  shader_object_.resize(shader_object_.size() + sizeof(dxbc::Signature) / sizeof(u32));
  u32 parameter_count = 0;
  constexpr size_t kParameterDwords = sizeof(dxbc::SignatureParameter) / sizeof(u32);

  Modification shader_modification = GetDXBCShaderModification();

  if (IsVertexShader()) {
    size_t interpolator_position = shader_object_.size();
    u32 interpolator_count = rex::BitCount(GetModificationInterpolatorMask());
    shader_object_.resize(shader_object_.size() + interpolator_count * kParameterDwords);
    parameter_count += interpolator_count;
    {
      auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                       interpolator_position);
      for (u32 i = 0; i < interpolator_count; ++i) {
        dxbc::SignatureParameter& interpolator = interpolators[i];
        interpolator.semantic_index = i;
        interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        interpolator.register_index = out_reg_vs_interpolators_ + i;
        interpolator.mask = 0b1111;
      }
    }

    size_t position_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& position =
          *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + position_position);
      position.system_value = dxbc::Name::kPosition;
      position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      position.register_index = out_reg_vs_position_;
      position.mask = 0b1111;
    }

    size_t clip_and_cull_distance_position = shader_object_.size();
    u32 clip_distance_count = shader_modification.GetVertexClipDistanceCount();
    u32 cull_distance_count = shader_modification.GetVertexCullDistanceCount();
    u32 clip_and_cull_distance_count = clip_distance_count + cull_distance_count;
    u32 clip_distance_parameter_count = 0;
    u32 cull_distance_parameter_count = 0;
    for (u32 i = 0; i < clip_and_cull_distance_count; i += 4) {
      u32 clip_cull_distance_register = out_reg_vs_clip_cull_distances_ + (i >> 2);
      if (i < clip_distance_count) {
        shader_object_.resize(shader_object_.size() + kParameterDwords);
        ++parameter_count;
        {
          auto& clip_distance = *reinterpret_cast<dxbc::SignatureParameter*>(
              shader_object_.data() + (shader_object_.size() - kParameterDwords));
          clip_distance.semantic_index = clip_distance_parameter_count;
          clip_distance.system_value = dxbc::Name::kClipDistance;
          clip_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          clip_distance.register_index = clip_cull_distance_register;
          u8 clip_distance_mask =
              (UINT8_C(1) << std::min(clip_distance_count - i, UINT32_C(4))) - 1;
          clip_distance.mask = clip_distance_mask;
          clip_distance.never_writes_mask = clip_distance_mask ^ 0b1111;
        }
        ++clip_distance_parameter_count;
      }
      if (cull_distance_count && i + 4 > clip_distance_count) {
        shader_object_.resize(shader_object_.size() + kParameterDwords);
        ++parameter_count;
        {
          auto& cull_distance = *reinterpret_cast<dxbc::SignatureParameter*>(
              shader_object_.data() + (shader_object_.size() - kParameterDwords));
          cull_distance.semantic_index = cull_distance_parameter_count;
          cull_distance.system_value = dxbc::Name::kCullDistance;
          cull_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          cull_distance.register_index = clip_cull_distance_register;
          u8 cull_distance_mask =
              (UINT8_C(1) << std::min(cull_distance_count - i, UINT32_C(4))) - 1;
          if (i < clip_distance_count) {
            cull_distance_mask &= ~((UINT8_C(1) << (clip_distance_count - i)) - 1);
          }
          cull_distance.mask = cull_distance_mask;
          cull_distance.never_writes_mask = cull_distance_mask ^ 0b1111;
        }
        ++cull_distance_parameter_count;
      }
    }

    size_t point_size_position = shader_object_.size();
    if (out_reg_vs_point_size_ != UINT32_MAX) {
      shader_object_.resize(shader_object_.size() + kParameterDwords);
      ++parameter_count;
      {
        auto& point_size = *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                        point_size_position);
        point_size.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        point_size.register_index = out_reg_vs_point_size_;
        point_size.mask = 0b0001;
        point_size.never_writes_mask = 0b1110;
      }
    }

    u32 semantic_offset = static_cast<u32>((shader_object_.size() - blob_position) * sizeof(u32));
    if (interpolator_count) {
      {
        auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                         interpolator_position);
        for (u32 i = 0; i < interpolator_count; ++i) {
          interpolators[i].semantic_name_ptr = semantic_offset;
        }
      }
      semantic_offset += dxbc::AppendAlignedString(shader_object_, "TEXCOORD");
    }
    {
      auto& position =
          *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + position_position);
      position.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_Position");
    if (clip_distance_parameter_count) {
      {
        auto clip_distances = reinterpret_cast<dxbc::SignatureParameter*>(
            shader_object_.data() + clip_and_cull_distance_position);
        for (u32 i = 0; i < clip_distance_parameter_count; ++i) {
          clip_distances[i].semantic_name_ptr = semantic_offset;
        }
      }
      semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_ClipDistance");
    }
    if (cull_distance_parameter_count) {
      {
        auto cull_distances = reinterpret_cast<dxbc::SignatureParameter*>(
                                  shader_object_.data() + clip_and_cull_distance_position) +
                              clip_distance_parameter_count;
        for (u32 i = 0; i < cull_distance_parameter_count; ++i) {
          cull_distances[i].semantic_name_ptr = semantic_offset;
        }
      }
      semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_CullDistance");
    }
    if (out_reg_vs_point_size_ != UINT32_MAX) {
      {
        auto& point_size = *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                        point_size_position);
        point_size.semantic_name_ptr = semantic_offset;
      }
      semantic_offset += dxbc::AppendAlignedString(shader_object_, "XEPSIZE");
    }
  } else if (IsPixelShader()) {
    if (!edram_rov_used_) {
      u32 color_targets_written = GetCurrentShader().GetWritesColorTargets();

      size_t target_position = SIZE_MAX;
      u32 color_targets_written_count = rex::BitCount(color_targets_written);
      if (color_targets_written) {
        target_position = shader_object_.size();
        shader_object_.resize(shader_object_.size() +
                              color_targets_written_count * kParameterDwords);
        parameter_count += color_targets_written_count;
        auto targets =
            reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + target_position);
        u32 target_index = 0;
        for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
          if (!(color_targets_written & (static_cast<u32>(1) << i))) {
            continue;
          }
          dxbc::SignatureParameter& target = targets[target_index++];
          target.semantic_index = i;
          target.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          target.register_index = i;
          target.mask = 0b1111;
        }
      }

      size_t coverage_position = SIZE_MAX;
      if ((color_targets_written & 0b1) && !IsForceEarlyDepthStencilGlobalFlagEnabled()) {
        coverage_position = shader_object_.size();
        shader_object_.resize(shader_object_.size() + kParameterDwords);
        ++parameter_count;
        auto& coverage =
            *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + coverage_position);
        coverage.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
        coverage.register_index = UINT32_MAX;
        coverage.mask = 0b0001;
        coverage.never_writes_mask = 0b1110;
      }

      size_t depth_position = SIZE_MAX;
      if (GetCurrentShader().GetWritesDepth() || DSVIsWritingFloat24Depth()) {
        depth_position = shader_object_.size();
        shader_object_.resize(shader_object_.size() + kParameterDwords);
        ++parameter_count;
        auto& depth =
            *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + depth_position);
        depth.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        depth.register_index = UINT32_MAX;
        depth.mask = 0b0001;
        depth.never_writes_mask = 0b1110;
      }

      u32 semantic_offset = static_cast<u32>((shader_object_.size() - blob_position) * sizeof(u32));
      if (target_position != SIZE_MAX) {
        {
          auto targets =
              reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + target_position);
          for (u32 i = 0; i < color_targets_written_count; ++i) {
            targets[i].semantic_name_ptr = semantic_offset;
          }
        }
        semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_Target");
      }
      if (coverage_position != SIZE_MAX) {
        {
          auto& coverage = *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() +
                                                                        coverage_position);
          coverage.semantic_name_ptr = semantic_offset;
        }
        semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_Coverage");
      }
      if (depth_position != SIZE_MAX) {
        {
          auto& depth =
              *reinterpret_cast<dxbc::SignatureParameter*>(shader_object_.data() + depth_position);
          depth.semantic_name_ptr = semantic_offset;
        }
        const char* depth_semantic_name;
        if (!GetCurrentShader().GetWritesDepth() &&
            shader_modification.pixel.depth_stencil_mode ==
                Modification::DepthStencilMode::kFloat24Truncating) {
          depth_semantic_name = "SV_DepthLessEqual";
        } else {
          depth_semantic_name = "SV_Depth";
        }
        semantic_offset += dxbc::AppendAlignedString(shader_object_, depth_semantic_name);
      }
    }
  }

  {
    auto& header = *reinterpret_cast<dxbc::Signature*>(shader_object_.data() + blob_position);
    header.parameter_count = parameter_count;
    header.parameter_info_ptr = sizeof(dxbc::Signature);
  }
}

void DXBCShaderTranslator::WriteShaderCode() {
  u32 blob_position_dwords = static_cast<u32>(shader_object_.size());

  dxbc::ProgramType program_type;
  if (IsDXBCVertexShader()) {
    program_type = dxbc::ProgramType::kVertexShader;
  } else if (IsDXBCDomainShader()) {
    program_type = dxbc::ProgramType::kDomainShader;
  } else {
    REX_ASSERT(IsPixelShader());
    program_type = dxbc::ProgramType::kPixelShader;
  }
  shader_object_.push_back(dxbc::VersionToken(program_type, 5, 1));
  shader_object_.push_back(0);

  Modification shader_modification = GetDXBCShaderModification();

  u32 control_point_count = 1;
  if (IsDXBCDomainShader()) {
    dxbc::TessellatorDomain tessellator_domain = dxbc::TessellatorDomain::kTriangle;
    switch (shader_modification.vertex.host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
        control_point_count = 3;
        tessellator_domain = dxbc::TessellatorDomain::kTriangle;
        break;
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        control_point_count = 1;
        tessellator_domain = dxbc::TessellatorDomain::kTriangle;
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
        control_point_count = 4;
        tessellator_domain = dxbc::TessellatorDomain::kQuad;
        break;
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        control_point_count = 1;
        tessellator_domain = dxbc::TessellatorDomain::kQuad;
        break;
      default:
        REX_UNHANDLED_CASE(shader_modification.vertex.host_vertex_shader_type);
        EmitTranslationError("Unsupported host vertex shader type in WriteShaderCode");
    }
    ao_.OpDclInputControlPointCount(control_point_count);
    ao_.OpDclTessDomain(tessellator_domain);
  }

  // Refactoring is disallowed to keep position invariance, needed for pixel oDepth invariance.
  bool global_flag_force_early_depth_stencil = IsForceEarlyDepthStencilGlobalFlagEnabled();
  ao_.OpDclGlobalFlags(
      global_flag_force_early_depth_stencil ? dxbc::kGlobalFlagForceEarlyDepthStencil : 0);

  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    const Shader::ConstantRegisterMap& constant_register_map =
        GetCurrentShader().GetConstantRegisterMap();
    REX_ASSERT(constant_register_map.float_count != 0);
    ao_.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_float_constants_,
                                          std::to_underlying(CbufferRegister::kFloatConstants),
                                          std::to_underlying(CbufferRegister::kFloatConstants)),
                            constant_register_map.float_count,
                            constant_register_map.float_dynamic_addressing
                                ? dxbc::ConstantBufferAccessPattern::kDynamicIndexed
                                : dxbc::ConstantBufferAccessPattern::kImmediateIndexed);
  }
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    ao_.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_system_constants_,
                                          std::to_underlying(CbufferRegister::kSystemConstants),
                                          std::to_underlying(CbufferRegister::kSystemConstants)),
                            (sizeof(SystemConstants) + 15) >> 4);
  }
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    ao_.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_fetch_constants_,
                                          std::to_underlying(CbufferRegister::kFetchConstants),
                                          std::to_underlying(CbufferRegister::kFetchConstants)),
                            48);
  }
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    REX_ASSERT(GetBindlessResourceCount() != 0);
    ao_.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_descriptor_indices_,
                                          std::to_underlying(CbufferRegister::kDescriptorIndices),
                                          std::to_underlying(CbufferRegister::kDescriptorIndices)),
                            (GetBindlessResourceCount() + 3) >> 2);
  }
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    ao_.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_bool_loop_constants_,
                                          std::to_underlying(CbufferRegister::kBoolLoopConstants),
                                          std::to_underlying(CbufferRegister::kBoolLoopConstants)),
                            2 + 8);
  }

  if (!sampler_bindings_.empty()) {
    if (bindless_resources_used_) {
      ao_.OpDclSampler(dxbc::Src::S(dxbc::Src::kDcl, 0, 0, UINT32_MAX));
    } else {
      for (u32 i = 0; i < u32(sampler_bindings_.size()); ++i) {
        ao_.OpDclSampler(dxbc::Src::S(dxbc::Src::kDcl, i, i, i));
      }
    }
  }

  for (u32 i = 0; i < srv_count_; ++i) {
    if (i == srv_index_shared_memory_) {
      ao_.OpDclResourceRaw(dxbc::Src::T(dxbc::Src::kDcl, srv_index_shared_memory_,
                                        std::to_underlying(SRVMainRegister::kSharedMemory),
                                        std::to_underlying(SRVMainRegister::kSharedMemory)),
                           std::to_underlying(SRVSpace::kMain));
    } else {
      dxbc::ResourceDimension texture_dimension;
      u32 texture_register_lower_bound, texture_register_upper_bound;
      SRVSpace texture_register_space;
      if (bindless_resources_used_) {
        texture_register_lower_bound = 0;
        texture_register_upper_bound = UINT32_MAX;
        if (i == srv_index_bindless_textures_3d_) {
          texture_dimension = dxbc::ResourceDimension::kTexture3D;
          texture_register_space = SRVSpace::kBindlessTextures3D;
        } else if (i == srv_index_bindless_textures_cube_) {
          texture_dimension = dxbc::ResourceDimension::kTextureCube;
          texture_register_space = SRVSpace::kBindlessTexturesCube;
        } else {
          REX_ASSERT(i == srv_index_bindless_textures_2d_);
          texture_dimension = dxbc::ResourceDimension::kTexture2DArray;
          texture_register_space = SRVSpace::kBindlessTextures2DArray;
        }
      } else {
        auto it = texture_bindings_for_bindful_srv_indices_.find(i);
        REX_ASSERT(it != texture_bindings_for_bindful_srv_indices_.end());
        u32 texture_binding_index = it->second;
        const TextureBinding& texture_binding = texture_bindings_[texture_binding_index];
        switch (texture_binding.dimension) {
          case xenos::FetchOpDimension::k3DOrStacked:
            texture_dimension = dxbc::ResourceDimension::kTexture3D;
            break;
          case xenos::FetchOpDimension::kCube:
            texture_dimension = dxbc::ResourceDimension::kTextureCube;
            break;
          default:
            REX_ASSERT(texture_binding.dimension == xenos::FetchOpDimension::k2D);
            texture_dimension = dxbc::ResourceDimension::kTexture2DArray;
        }
        texture_register_lower_bound =
            std::to_underlying(SRVMainRegister::kBindfulTexturesStart) + texture_binding_index;
        texture_register_upper_bound = texture_register_lower_bound;
        texture_register_space = SRVSpace::kMain;
      }
      ao_.OpDclResource(texture_dimension,
                        dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
                        dxbc::Src::T(dxbc::Src::kDcl, i, texture_register_lower_bound,
                                     texture_register_upper_bound),
                        static_cast<u32>(texture_register_space));
    }
  }

  for (u32 i = 0; i < uav_count_; ++i) {
    if (i == uav_index_shared_memory_) {
      if (!IsPixelShader()) {
        shader_feature_info_.feature_flags[0] |= dxbc::kShaderFeature0UAVsAtEveryStage;
      }
      ao_.OpDclUnorderedAccessViewRaw(0,
                                      dxbc::Src::U(dxbc::Src::kDcl, uav_index_shared_memory_,
                                                   std::to_underlying(UAVRegister::kSharedMemory),
                                                   std::to_underlying(UAVRegister::kSharedMemory)));
    } else if (i == uav_index_edram_) {
      shader_feature_info_.feature_flags[0] |= dxbc::kShaderFeature0ROVs;
      ao_.OpDclUnorderedAccessViewTyped(
          dxbc::ResourceDimension::kBuffer, dxbc::kUAVFlagRasterizerOrderedAccess,
          dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
          dxbc::Src::U(dxbc::Src::kDcl, uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM),
                       std::to_underlying(UAVRegister::kEDRAM)));
    } else {
      REX_UNHANDLED_CASE(i);
    }
  }

  if (IsVertexShader()) {
    if (IsDXBCDomainShader()) {
      if (in_domain_location_used_) {
        ao_.OpDclInput(dxbc::Dest::VDomain(in_domain_location_used_));
      }
      if (in_control_point_index_used_) {
        ao_.OpDclInput(
            dxbc::Dest::VICP(control_point_count, kInRegisterDSControlPointIndex, 0b0001));
      }
    } else {
      if (GetRegisterCount()) {
        ao_.OpDclInputSGV(dxbc::Dest::V1D(kInRegisterVSVertexIndex, 0b0001), dxbc::Name::kVertexID);
      }
    }
    u32 interpolator_count = rex::BitCount(GetModificationInterpolatorMask());
    for (u32 i = 0; i < interpolator_count; ++i) {
      ao_.OpDclOutput(dxbc::Dest::O(out_reg_vs_interpolators_ + i));
    }
    ao_.OpDclOutputSIV(dxbc::Dest::O(out_reg_vs_position_), dxbc::Name::kPosition);
    u32 clip_distance_count = shader_modification.GetVertexClipDistanceCount();
    u32 cull_distance_count = shader_modification.GetVertexCullDistanceCount();
    u32 clip_and_cull_distance_count = clip_distance_count + cull_distance_count;
    for (u32 i = 0; i < clip_and_cull_distance_count; i += 4) {
      if (i < clip_distance_count) {
        ao_.OpDclOutputSIV(
            dxbc::Dest::O(out_reg_vs_clip_cull_distances_ + (i >> 2),
                          (UINT32_C(1) << std::min(clip_distance_count - i, UINT32_C(4))) - 1),
            dxbc::Name::kClipDistance);
      }
      if (cull_distance_count && i + 4 > clip_distance_count) {
        u32 cull_distance_mask =
            (UINT32_C(1) << std::min(clip_and_cull_distance_count - i, UINT32_C(4))) - 1;
        if (i < clip_distance_count) {
          cull_distance_mask &= ~((UINT32_C(1) << (clip_distance_count - i)) - 1);
        }
        ao_.OpDclOutputSIV(
            dxbc::Dest::O(out_reg_vs_clip_cull_distances_ + (i >> 2), cull_distance_mask),
            dxbc::Name::kCullDistance);
      }
    }
    if (out_reg_vs_point_size_ != UINT32_MAX) {
      ao_.OpDclOutput(dxbc::Dest::O(out_reg_vs_point_size_, 0b0001));
    }
  } else if (IsPixelShader()) {
    bool is_writing_float24_depth = DSVIsWritingFloat24Depth();
    bool shader_writes_depth = GetCurrentShader().GetWritesDepth();
    u32 interpolator_register_index = in_reg_ps_interpolators_;
    u32 interpolators_remaining = GetModificationInterpolatorMask();
    u32 interpolator_index;
    while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
      interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
      if (interpolator_index >= GetRegisterCount()) {
        break;
      }
      ao_.OpDclInputPS(
          (shader_modification.pixel.interpolators_centroid & (UINT32_C(1) << interpolator_index))
              ? dxbc::InterpolationMode::kLinearCentroid
              : dxbc::InterpolationMode::kLinear,
          dxbc::Dest::V1D(interpolator_register_index));
      ++interpolator_register_index;
    }
    if (in_reg_ps_point_coordinates_ != UINT32_MAX) {
      ao_.OpDclInputPS(dxbc::InterpolationMode::kLinear,
                       dxbc::Dest::V1D(in_reg_ps_point_coordinates_, 0b0011));
    }
    if (in_position_used_) {
      ao_.OpDclInputPSSIV((is_writing_float24_depth && !shader_writes_depth)
                              ? dxbc::InterpolationMode::kLinearNoPerspectiveSample
                              : dxbc::InterpolationMode::kLinearNoPerspective,
                          dxbc::Dest::V1D(in_reg_ps_position_, in_position_used_),
                          dxbc::Name::kPosition);
    }
    bool sample_rate_memexport = GetCurrentShader().GetMemExportEMWritten() && IsSampleRate();
    REX_ASSERT(!(sample_rate_memexport && edram_rov_used_));
    u32 front_face_and_sample_index_mask =
        static_cast<u32>(in_front_face_used_) | (static_cast<u32>(sample_rate_memexport) << 1);
    if (front_face_and_sample_index_mask) {
      ao_.OpDclInputPSSGV(
          dxbc::Dest::V1D(in_reg_ps_front_face_sample_index_, front_face_and_sample_index_mask),
          dxbc::Name::kIsFrontFace);
    }
    if (edram_rov_used_) {
      ao_.OpDclInput(dxbc::Dest::VCoverage());
    } else {
      if (sample_rate_memexport) {
        ao_.OpDclInput(dxbc::Dest::VCoverage());
      }
      u32 color_targets_written = GetCurrentShader().GetWritesColorTargets();
      for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        if (color_targets_written & (static_cast<u32>(1) << i)) {
          ao_.OpDclOutput(dxbc::Dest::O(i));
        }
      }
      if ((color_targets_written & 0b1) && !global_flag_force_early_depth_stencil) {
        ao_.OpDclOutput(dxbc::Dest::OMask());
      }
      if (is_writing_float24_depth || shader_writes_depth) {
        if (!shader_writes_depth && GetDXBCShaderModification().pixel.depth_stencil_mode ==
                                        Modification::DepthStencilMode::kFloat24Truncating) {
          ao_.OpDclOutput(dxbc::Dest::ODepthLE());
        } else {
          ao_.OpDclOutput(dxbc::Dest::ODepth());
        }
      }
    }
  }

  u32 temp_register_count = system_temp_count_max_;
  if (!is_depth_only_pixel_shader_ && !GetCurrentShader().UsesRegisterDynamicAddressing()) {
    temp_register_count += GetRegisterCount();
  }
  if (temp_register_count) {
    ao_.OpDclTemps(temp_register_count);
  }
  if (!is_depth_only_pixel_shader_ && GetCurrentShader().UsesRegisterDynamicAddressing()) {
    REX_ASSERT(GetRegisterCount() != 0);
    ao_.OpDclIndexableTemp(0, GetRegisterCount(), 4);
  }

  size_t code_size_dwords = shader_code_.size();
  if (code_size_dwords) {
    shader_object_.resize(shader_object_.size() + code_size_dwords);
    std::memcpy(shader_object_.data() + (shader_object_.size() - code_size_dwords),
                shader_code_.data(), code_size_dwords * sizeof(u32));
  }

  shader_object_[blob_position_dwords + 1] =
      static_cast<u32>(shader_object_.size()) - blob_position_dwords;
}

}  // namespace rex::gpu
