/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/render_target/cache.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include "gpu/xenos/pipeline/texture/cache.h"
#include "gpu/xenos/util/draw.h"
#include <rex/math.h>

#include "gpu/xenos/cvar_table.h"

namespace rex::gpu {
using namespace xenos::ucode;

void DxbcShaderTranslator::StartPixelShader_LoadROVParameters() {
  bool any_color_targets_written = current_shader().writes_color_targets() != 0;

  // system_temp_rov_params_.y is the absolute depth offset, .z 32bpp and .w 64bpp base relative.

  // 64bpp color uses 40x16 samples per 1280 byte tile, depth tiles have 40 sample halves swapped.

  in_position_used_ |= 0b0011;
  a_.OpFToU(dxbc::Dest::R(system_temp_rov_params_, 0b0011), dxbc::Src::V1D(in_reg_ps_position_));
  a_.OpIShL(dxbc::Dest::R(system_temp_rov_params_, 0b0011), dxbc::Src::R(system_temp_rov_params_),
            LoadSystemConstant(SystemConstants::Index::kSampleCountLog2,
                               offsetof(SystemConstants, sample_count_log2), 0b0100));
  // Depth only uses full 80x16 tiles instead of 40x16 half tiles.
  u32 tile_width = xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x_;
  u32 tile_or_tile_half_width = tile_width >> static_cast<u32>(any_color_targets_written);
  u32 tile_height = xenos::kEDRAMTileHeightSamples * draw_resolution_scale_y_;
  a_.OpUDiv(
      dxbc::Dest::R(system_temp_rov_params_, 0b1100),
      dxbc::Dest::R(system_temp_rov_params_, 0b0011),
      dxbc::Src::R(system_temp_rov_params_, 0b01000100),
      dxbc::Src::LU(tile_or_tile_half_width, tile_height, tile_or_tile_half_width, tile_height));

  a_.OpUMul(dxbc::Dest::Null(), dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), dxbc::Src::LU(tile_width));

  u32 tile_size = tile_width * tile_height;
  u32 tile_half_width = tile_width >> 1;
  if (any_color_targets_written) {
    // 64bpp uses X in the 80x16 dword region, 32bpp color and depth use z & 1, flipped for depth.

    u32 rov_address_temp = PushSystemTemp();

    a_.OpUMul(dxbc::Dest::Null(), dxbc::Dest::R(system_temp_rov_params_, 0b1000),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
              LoadSystemConstant(SystemConstants::Index::kEDRAM32bppTilePitchDwordsScaled,
                                 offsetof(SystemConstants, edram_32bpp_tile_pitch_dwords_scaled),
                                 dxbc::Src::kXXXX));

    a_.OpUShR(dxbc::Dest::R(rov_address_temp, 0b0001),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ), dxbc::Src::LU(1));
    a_.OpUMAd(dxbc::Dest::R(rov_address_temp, 0b0001),
              dxbc::Src::R(rov_address_temp, dxbc::Src::kXXXX), dxbc::Src::LU(tile_size),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));
    a_.OpIAdd(dxbc::Dest::R(rov_address_temp, 0b0001),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
              dxbc::Src::R(rov_address_temp, dxbc::Src::kXXXX));

    a_.OpUMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ), dxbc::Src::LU(tile_size),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));
    a_.OpUMAd(dxbc::Dest::R(system_temp_rov_params_, 0b1000),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW), dxbc::Src::LU(2),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));

    a_.OpUMAd(dxbc::Dest::R(system_temp_rov_params_, 0b1000),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX), dxbc::Src::LU(2),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW));

    a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ), dxbc::Src::LU(1));

    a_.OpUMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0100),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::LU(tile_half_width),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0100),
              dxbc::Src::R(rov_address_temp, dxbc::Src::kXXXX),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ));

    // Depth and stencil flip the 40x16 half tiles relative to 32bpp color.
    a_.OpMovC(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::LI(-static_cast<i32>(tile_half_width)),
              dxbc::Src::LI(static_cast<i32>(tile_half_width)));
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));

    PopSystemTemp();
  } else {
    a_.OpUMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0100),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ), dxbc::Src::LU(tile_size),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));
    a_.OpUMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
              LoadSystemConstant(SystemConstants::Index::kEDRAM32bppTilePitchDwordsScaled,
                                 offsetof(SystemConstants, edram_32bpp_tile_pitch_dwords_scaled),
                                 dxbc::Src::kXXXX),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ));
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
    a_.OpUGE(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(tile_half_width));
    a_.OpMovC(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
              dxbc::Src::LI(-static_cast<i32>(tile_half_width)),
              dxbc::Src::LI(static_cast<i32>(tile_half_width)));
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
  }

  a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
            LoadSystemConstant(SystemConstants::Index::kEDRAMDepthBaseDwordsScaled,
                               offsetof(SystemConstants, edram_depth_base_dwords_scaled),
                               dxbc::Src::kXXXX));
  a_.OpUDiv(dxbc::Dest::Null(), dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
            dxbc::Src::LU(tile_size * xenos::kEDRAMTileCount));

  // ForcedSampleCount is 4 as 2 is unsupported on Nvidia, 2x MSAA samples 0 and 3 become 0 and 1.

  a_.OpIf(true, LoadSystemConstant(SystemConstants::Index::kSampleCountLog2,
                                   offsetof(SystemConstants, sample_count_log2), dxbc::Src::kXXXX));
  {
    a_.OpBFRev(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::VCoverage());
    a_.OpUShR(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX), dxbc::Src::LU(29));
    a_.OpBFI(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(1),
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX), dxbc::Src::VCoverage());
  }
  a_.OpElse();
  {
    a_.OpUBFE(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(1), dxbc::Src::LU(3),
              dxbc::Src::VCoverage());
    a_.OpBFI(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX), dxbc::Src::VCoverage());
  }
  a_.OpEndIf();
}

void DxbcShaderTranslator::ROV_DepthStencilTest() {
  u32 temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  dxbc::Dest temp_y_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Dest temp_z_dest(dxbc::Dest::R(temp, 0b0100));
  dxbc::Src temp_z_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));
  dxbc::Dest temp_w_dest(dxbc::Dest::R(temp, 0b1000));
  dxbc::Src temp_w_src(dxbc::Src::R(temp, dxbc::Src::kWWWW));

  a_.OpAnd(temp_x_dest, LoadFlagsSystemConstant(), dxbc::Src::LU(kSysFlag_ROVDepthStencil));
  a_.OpIf(true, temp_x_src);

  bool shader_writes_depth = current_shader().writes_depth();
  bool depth_stencil_early = ROV_IsDepthStencilEarly();

  dxbc::Src z_ddx_src(dxbc::Src::LF(0.0f)), z_ddy_src(dxbc::Src::LF(0.0f));

  if (shader_writes_depth) {
    // Adreno 200 has no PA_SC_VPORT_ZMIN/ZMAX, so there is no clamp to the viewport depth bounds.
    ROV_DepthTo24Bit(system_temp_depth_stencil_, 0, system_temp_depth_stencil_, 0, temp, 0);
  } else {
    dxbc::Src in_position_z(dxbc::Src::V1D(in_reg_ps_position_, dxbc::Src::kZZZZ));
    if (depth_stencil_early) {
      z_ddx_src = dxbc::Src::R(temp, dxbc::Src::kXXXX);
      z_ddy_src = dxbc::Src::R(temp, dxbc::Src::kYYYY);
      in_position_used_ |= 0b0100;
      a_.OpDerivRTXCoarse(temp_x_dest, in_position_z);
      a_.OpDerivRTYCoarse(temp_y_dest, in_position_z);
    } else {
      REX_ASSERT(system_temp_depth_stencil_ != UINT32_MAX);
      z_ddx_src = dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kXXXX);
      z_ddy_src = dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kYYYY);
    }
    a_.OpMax(temp_z_dest, z_ddx_src.Abs(), z_ddy_src.Abs());
    in_front_face_used_ = true;
    a_.OpIf(true, dxbc::Src::V1D(in_reg_ps_front_face_sample_index_, dxbc::Src::kXXXX));
    a_.OpMAd(
        temp_z_dest, temp_z_src,
        LoadSystemConstant(SystemConstants::Index::kEDRAMPolyOffsetFront,
                           offsetof(SystemConstants, edram_poly_offset_front), dxbc::Src::kXXXX),
        LoadSystemConstant(SystemConstants::Index::kEDRAMPolyOffsetFront,
                           offsetof(SystemConstants, edram_poly_offset_front), dxbc::Src::kYYYY));
    a_.OpElse();
    a_.OpMAd(
        temp_z_dest, temp_z_src,
        LoadSystemConstant(SystemConstants::Index::kEDRAMPolyOffsetBack,
                           offsetof(SystemConstants, edram_poly_offset_back), dxbc::Src::kXXXX),
        LoadSystemConstant(SystemConstants::Index::kEDRAMPolyOffsetBack,
                           offsetof(SystemConstants, edram_poly_offset_back), dxbc::Src::kYYYY));
    a_.OpEndIf();
    // Do not clamp the center depth, sample derivatives are added to it and it must stay linear.
    in_position_used_ |= 0b0100;
    a_.OpAdd(temp_z_dest, temp_z_src, in_position_z);
  }

  for (u32 i = 0; i < 4; ++i) {
    // Early depth/stencil writes may defer to shader end, so write system_temp_depth_stencil_.
    dxbc::Dest sample_depth_stencil_dest(
        depth_stencil_early ? dxbc::Dest::R(system_temp_depth_stencil_, 1 << i) : temp_w_dest);
    dxbc::Src sample_depth_stencil_src(
        depth_stencil_early ? dxbc::Src::R(system_temp_depth_stencil_).Select(i) : temp_w_src);

    a_.OpAnd(temp_w_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(1 << i));
    a_.OpIf(true, temp_w_src);

    u32 sample_temp = PushSystemTemp();
    dxbc::Dest sample_temp_x_dest(dxbc::Dest::R(sample_temp, 0b0001));
    dxbc::Src sample_temp_x_src(dxbc::Src::R(sample_temp, dxbc::Src::kXXXX));
    dxbc::Dest sample_temp_y_dest(dxbc::Dest::R(sample_temp, 0b0010));
    dxbc::Src sample_temp_y_src(dxbc::Src::R(sample_temp, dxbc::Src::kYYYY));
    dxbc::Dest sample_temp_z_dest(dxbc::Dest::R(sample_temp, 0b0100));
    dxbc::Src sample_temp_z_src(dxbc::Src::R(sample_temp, dxbc::Src::kZZZZ));
    dxbc::Dest sample_temp_w_dest(dxbc::Dest::R(sample_temp, 0b1000));
    dxbc::Src sample_temp_w_src(dxbc::Src::R(sample_temp, dxbc::Src::kWWWW));

    if (shader_writes_depth) {
      REX_ASSERT(!depth_stencil_early);
      a_.OpMov(sample_depth_stencil_dest,
               dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kXXXX));
    } else {
      switch (i) {
        case 0:
          a_.OpMAd(
              sample_depth_stencil_dest, z_ddx_src,
              dxbc::Src::LF(static_cast<f32>(draw_util::kD3D10StandardSamplePositions4x[0][0]) *
                            (1.0f / 16.0f)),
              temp_z_src);
          a_.OpMAd(
              sample_depth_stencil_dest, z_ddy_src,
              dxbc::Src::LF(static_cast<f32>(draw_util::kD3D10StandardSamplePositions4x[0][1]) *
                            (1.0f / 16.0f)),
              sample_depth_stencil_src);
          a_.OpMovC(
              sample_depth_stencil_dest,
              LoadSystemConstant(SystemConstants::Index::kSampleCountLog2,
                                 offsetof(SystemConstants, sample_count_log2), dxbc::Src::kYYYY),
              sample_depth_stencil_src, temp_z_src, true);
          break;
        case 1:
          // With ForcedSampleCount 4 the 2x MSAA bottom sample is 3 and the 4x bottom left is 2.
          a_.OpIf(true, LoadSystemConstant(SystemConstants::Index::kSampleCountLog2,
                                           offsetof(SystemConstants, sample_count_log2),
                                           dxbc::Src::kXXXX));
          a_.OpMAd(
              sample_depth_stencil_dest, z_ddx_src,
              dxbc::Src::LF(static_cast<f32>(draw_util::kD3D10StandardSamplePositions4x[2][0]) *
                            (1.0f / 16.0f)),
              temp_z_src);
          a_.OpMAd(
              sample_depth_stencil_dest, z_ddy_src,
              dxbc::Src::LF(static_cast<f32>(draw_util::kD3D10StandardSamplePositions4x[2][1]) *
                            (1.0f / 16.0f)),
              sample_depth_stencil_src, true);
          a_.OpElse();
          a_.OpMAd(
              sample_depth_stencil_dest, z_ddx_src,
              dxbc::Src::LF(static_cast<f32>(draw_util::kD3D10StandardSamplePositions4x[3][0]) *
                            (1.0f / 16.0f)),
              temp_z_src);
          a_.OpMAd(
              sample_depth_stencil_dest, z_ddy_src,
              dxbc::Src::LF(static_cast<f32>(draw_util::kD3D10StandardSamplePositions4x[3][1]) *
                            (1.0f / 16.0f)),
              sample_depth_stencil_src, true);
          a_.OpEndIf();
          break;
        default: {
          // Samples 2 and 3 (top right and bottom right) map to Direct3D 11 samples 1 and 3.
          const i8* sample_position =
              draw_util::kD3D10StandardSamplePositions4x[i ^ (((i & 1) ^ (i >> 1)) * 0b11)];
          a_.OpMAd(sample_depth_stencil_dest, z_ddx_src,
                   dxbc::Src::LF(static_cast<f32>(sample_position[0]) * (1.0f / 16.0f)),
                   temp_z_src);
          a_.OpMAd(sample_depth_stencil_dest, z_ddy_src,
                   dxbc::Src::LF(static_cast<f32>(sample_position[1]) * (1.0f / 16.0f)),
                   sample_depth_stencil_src, true);
        } break;
      }
      ROV_DepthTo24Bit(sample_depth_stencil_src.index_1d_.index_,
                       sample_depth_stencil_src.swizzle_ & 3,
                       sample_depth_stencil_src.index_1d_.index_,
                       sample_depth_stencil_src.swizzle_ & 3, sample_temp, 0);
    }

    if (uav_index_edram_ == kBindingIndexUnallocated) {
      uav_index_edram_ = uav_count_++;
    }
    a_.OpLdUAVTyped(
        sample_temp_x_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1,
        dxbc::Src::U(uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM), dxbc::Src::kXXXX));

    a_.OpUShR(sample_temp_y_dest, sample_temp_x_src, dxbc::Src::LU(8));
    a_.OpIAdd(sample_temp_z_dest, sample_depth_stencil_src, -sample_temp_y_src);
    a_.OpILT(sample_temp_w_dest, sample_temp_z_src, dxbc::Src::LI(0));
    a_.OpMovC(sample_temp_w_dest, sample_temp_w_src, dxbc::Src::LU(kSysFlag_ROVDepthPassIfLess),
              dxbc::Src::LU(kSysFlag_ROVDepthPassIfGreater));
    a_.OpMovC(sample_temp_z_dest, sample_temp_z_src, sample_temp_w_src,
              dxbc::Src::LU(kSysFlag_ROVDepthPassIfEqual));
    a_.OpAnd(sample_temp_z_dest, sample_temp_z_src, LoadFlagsSystemConstant());
    a_.OpIf(true, sample_temp_z_src);
    {
      a_.OpAnd(sample_temp_z_dest, LoadFlagsSystemConstant(),
               dxbc::Src::LU(kSysFlag_ROVDepthWrite));
      a_.OpMovC(sample_depth_stencil_dest, sample_temp_z_src, sample_depth_stencil_src,
                sample_temp_y_src);
    }
    a_.OpElse();
    {
      a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
               dxbc::Src::LU(~static_cast<u32>(1 << i)));
    }
    a_.OpEndIf();
    a_.OpBFI(sample_depth_stencil_dest, dxbc::Src::LU(24), dxbc::Src::LU(8),
             sample_depth_stencil_src, sample_temp_x_src);

    a_.OpAnd(sample_temp_y_dest, LoadFlagsSystemConstant(), dxbc::Src::LU(kSysFlag_ROVStencilTest));
    a_.OpIf(true, sample_temp_y_src);
    {
      in_front_face_used_ = true;
      a_.OpIf(true, dxbc::Src::V1D(in_reg_ps_front_face_sample_index_, dxbc::Src::kXXXX));
      for (u32 j = 0; j < 2; ++j) {
        if (j) {
          a_.OpElse();
        }
        dxbc::Src stencil_read_mask_src(
            LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                               j ? offsetof(SystemConstants, edram_stencil_back_read_mask)
                                 : offsetof(SystemConstants, edram_stencil_front_read_mask),
                               dxbc::Src::kXXXX));
        a_.OpAnd(sample_temp_y_dest,
                 LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                                    j ? offsetof(SystemConstants, edram_stencil_back_reference)
                                      : offsetof(SystemConstants, edram_stencil_front_reference),
                                    dxbc::Src::kXXXX),
                 stencil_read_mask_src);
        a_.OpAnd(sample_temp_z_dest, sample_temp_x_src, stencil_read_mask_src);
      }
      a_.OpEndIf();
      a_.OpIAdd(sample_temp_y_dest, sample_temp_y_src, -sample_temp_z_src);
      a_.OpILT(sample_temp_z_dest, sample_temp_y_src, dxbc::Src::LI(0));
      a_.OpMovC(sample_temp_z_dest, sample_temp_z_src,
                dxbc::Src::LU(std::to_underlying(xenos::CompareFunction::kLess)),
                dxbc::Src::LU(std::to_underlying(xenos::CompareFunction::kGreater)));
      a_.OpMovC(sample_temp_y_dest, sample_temp_y_src, sample_temp_z_src,
                dxbc::Src::LU(std::to_underlying(xenos::CompareFunction::kEqual)));
      in_front_face_used_ = true;
      a_.OpMovC(sample_temp_z_dest,
                dxbc::Src::V1D(in_reg_ps_front_face_sample_index_, dxbc::Src::kXXXX),
                LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                                   offsetof(SystemConstants, edram_stencil_front_func_ops),
                                   dxbc::Src::kXXXX),
                LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                                   offsetof(SystemConstants, edram_stencil_back_func_ops),
                                   dxbc::Src::kXXXX));
      a_.OpAnd(sample_temp_y_dest, sample_temp_y_src, sample_temp_z_src);
      a_.OpIf(true, sample_temp_y_src);
      {
        a_.OpAnd(sample_temp_y_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                 dxbc::Src::LU(1 << i));
        a_.OpMovC(sample_temp_y_dest, sample_temp_y_src, dxbc::Src::LU(6), dxbc::Src::LU(9));
        a_.OpUBFE(sample_temp_y_dest, dxbc::Src::LU(3), sample_temp_y_src, sample_temp_z_src);
      }
      a_.OpElse();
      {
        a_.OpUBFE(sample_temp_y_dest, dxbc::Src::LU(3), dxbc::Src::LU(3), sample_temp_z_src);
        a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
                 dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                 dxbc::Src::LU(~static_cast<u32>(1 << i)));
      }
      a_.OpEndIf();

      a_.OpSwitch(sample_temp_y_src);
      {
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::StencilOp::kZero)));
        a_.OpMov(sample_temp_y_dest, dxbc::Src::LU(0));
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::StencilOp::kReplace)));
        in_front_face_used_ = true;
        a_.OpMovC(sample_temp_y_dest,
                  dxbc::Src::V1D(in_reg_ps_front_face_sample_index_, dxbc::Src::kXXXX),
                  LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                                     offsetof(SystemConstants, edram_stencil_front_reference),
                                     dxbc::Src::kXXXX),
                  LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                                     offsetof(SystemConstants, edram_stencil_back_reference),
                                     dxbc::Src::kXXXX));
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::StencilOp::kIncrementClamp)));
        {
          a_.OpAnd(sample_temp_y_dest, sample_temp_x_src, dxbc::Src::LU(UINT8_MAX));
          a_.OpIAdd(sample_temp_y_dest, sample_temp_y_src, dxbc::Src::LI(1));
          a_.OpIMin(sample_temp_y_dest, sample_temp_y_src, dxbc::Src::LI(UINT8_MAX));
        }
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::StencilOp::kDecrementClamp)));
        {
          a_.OpAnd(sample_temp_y_dest, sample_temp_x_src, dxbc::Src::LU(UINT8_MAX));
          a_.OpIAdd(sample_temp_y_dest, sample_temp_y_src, dxbc::Src::LI(-1));
          a_.OpIMax(sample_temp_y_dest, sample_temp_y_src, dxbc::Src::LI(0));
        }
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::StencilOp::kInvert)));
        a_.OpNot(sample_temp_y_dest, sample_temp_x_src);
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::StencilOp::kIncrementWrap)));
        a_.OpIAdd(sample_temp_y_dest, sample_temp_x_src, dxbc::Src::LI(1));
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::StencilOp::kDecrementWrap)));
        a_.OpIAdd(sample_temp_y_dest, sample_temp_x_src, dxbc::Src::LI(-1));
        a_.OpBreak();
        a_.OpDefault();
        a_.OpMov(sample_temp_y_dest, sample_temp_x_src);
        a_.OpBreak();
      }
      a_.OpEndSwitch();

      in_front_face_used_ = true;
      a_.OpMovC(sample_temp_z_dest,
                dxbc::Src::V1D(in_reg_ps_front_face_sample_index_, dxbc::Src::kXXXX),
                LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                                   offsetof(SystemConstants, edram_stencil_front_write_mask),
                                   dxbc::Src::kXXXX),
                LoadSystemConstant(SystemConstants::Index::kEDRAMStencil,
                                   offsetof(SystemConstants, edram_stencil_back_write_mask),
                                   dxbc::Src::kXXXX));
      a_.OpAnd(sample_temp_y_dest, sample_temp_y_src, sample_temp_z_src);
      a_.OpNot(sample_temp_z_dest, sample_temp_z_src);
      a_.OpAnd(sample_depth_stencil_dest, sample_depth_stencil_src, sample_temp_z_src);
      a_.OpOr(sample_depth_stencil_dest, sample_depth_stencil_src, sample_temp_y_src);
    }
    a_.OpEndIf();

    a_.OpAnd(sample_temp_y_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(1 << i));
    a_.OpIf(false, sample_temp_y_src);
    {
      a_.OpBFI(sample_depth_stencil_dest, dxbc::Src::LU(8), dxbc::Src::LU(0),
               sample_depth_stencil_src, sample_temp_x_src);
    }
    a_.OpEndIf();
    a_.OpINE(sample_temp_x_dest, sample_depth_stencil_src, sample_temp_x_src);
    if (depth_stencil_early && !current_shader().implicit_early_z_write_allowed()) {
      // The late write sample bits are bits 4:7 of system_temp_rov_params_.x.
      a_.OpBFI(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(1),
               dxbc::Src::LU(4 + i), sample_temp_x_src,
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
    } else {
      a_.OpIf(true, sample_temp_x_src);
      {
        if (depth_stencil_early) {
          a_.OpAnd(sample_temp_x_dest, LoadFlagsSystemConstant(),
                   dxbc::Src::LU(kSysFlag_ROVDepthStencilEarlyWrite));
          a_.OpIf(true, sample_temp_x_src);
        }
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(dxbc::Dest::U(uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM)),
                           dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1,
                           sample_depth_stencil_src);
        if (depth_stencil_early) {
          a_.OpElse();
          a_.OpOr(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
                  dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                  dxbc::Src::LU(1 << (4 + i)));
          a_.OpEndIf();
        }
      }
      a_.OpEndIf();
    }

    PopSystemTemp();

    a_.OpEndIf();

    // Samples are at +0, +(80*scale_x), +1 and +(80*scale_x+1).
    u32 tile_width = xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x_;
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::LI((i & 1) ? -static_cast<i32>(tile_width) + 2 - static_cast<i32>(i)
                                    : static_cast<i32>(tile_width)));
  }

  if (ROV_IsDepthStencilEarly()) {
    // Rejection must be at 2x2 quad granularity because texture fetches need derivatives.

    a_.OpAnd(dxbc::Dest::R(temp, 0b0001), dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(0b11111111));
    a_.OpMovC(dxbc::Dest::R(temp, 0b0001), dxbc::Src::R(temp, dxbc::Src::kXXXX),
              dxbc::Src::LF(1.0f), dxbc::Src::LF(0.0f));
    a_.OpDerivRTXFine(dxbc::Dest::R(temp, 0b0010), dxbc::Src::R(temp, dxbc::Src::kXXXX));
    a_.OpMovC(dxbc::Dest::R(temp, 0b0001), dxbc::Src::R(temp, dxbc::Src::kYYYY),
              dxbc::Src::LF(1.0f), dxbc::Src::R(temp, dxbc::Src::kXXXX));
    a_.OpDerivRTYCoarse(dxbc::Dest::R(temp, 0b0010), dxbc::Src::R(temp, dxbc::Src::kXXXX));
    a_.OpMovC(dxbc::Dest::R(temp, 0b0001), dxbc::Src::R(temp, dxbc::Src::kYYYY),
              dxbc::Src::LF(1.0f), dxbc::Src::R(temp, dxbc::Src::kXXXX));
    a_.OpRetC(false, dxbc::Src::R(temp, dxbc::Src::kXXXX));
  }

  a_.OpEndIf();

  PopSystemTemp();
}

void DxbcShaderTranslator::ROV_UnpackColor(u32 rt_index, u32 packed_temp,
                                           u32 packed_temp_components, u32 color_temp, u32 temp1,
                                           u32 temp1_component, u32 temp2, u32 temp2_component) {
  REX_ASSERT(color_temp != packed_temp || packed_temp_components == 0);

  dxbc::Src packed_temp_low(dxbc::Src::R(packed_temp).Select(packed_temp_components));

  a_.OpMov(dxbc::Dest::R(color_temp, 0b1100), dxbc::Src::LF(0.0f, 0.0f, 0.0f, 1.0f));

  a_.OpSwitch(LoadSystemConstant(
      SystemConstants::Index::kEDRAMRTFormatFlags,
      offsetof(SystemConstants, edram_rt_format_flags) + sizeof(u32) * rt_index, dxbc::Src::kXXXX));

  for (u32 i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA
          : xenos::ColorRenderTargetFormat::k_8_8_8_8)));
    a_.OpUBFE(dxbc::Dest::R(color_temp), dxbc::Src::LU(8), dxbc::Src::LU(0, 8, 16, 24),
              packed_temp_low);
    a_.OpUToF(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp));
    a_.OpMul(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp), dxbc::Src::LF(1.0f / 255.0f));
    if (i) {
      for (u32 j = 0; j < 3; ++j) {
        PWLGammaToLinear(a_, color_temp, j, color_temp, j, true, temp1, temp1_component, temp2,
                         temp2_component);
      }
    }
    a_.OpBreak();
  }

  a_.OpCase(dxbc::Src::LU(
      RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_2_10_10_10)));
  a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)));
  {
    a_.OpUBFE(dxbc::Dest::R(color_temp), dxbc::Src::LU(10, 10, 10, 2), dxbc::Src::LU(0, 10, 20, 30),
              packed_temp_low);
    a_.OpUToF(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp));
    a_.OpMul(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp),
             dxbc::Src::LF(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 3.0f));
  }
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)));
  a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)));
  {
    a_.OpUBFE(dxbc::Dest::R(color_temp, 0b1000), dxbc::Src::LU(2), dxbc::Src::LU(30),
              packed_temp_low);
    a_.OpUToF(dxbc::Dest::R(color_temp, 0b1000), dxbc::Src::R(color_temp, dxbc::Src::kWWWW));
    a_.OpMul(dxbc::Dest::R(color_temp, 0b1000), dxbc::Src::R(color_temp, dxbc::Src::kWWWW),
             dxbc::Src::LF(1.0f / 3.0f));
    // Process in reverse, color_temp.r holds the packed color when packed_temp is color_temp.
    for (i32 i = 2; i >= 0; --i) {
      Float7e3To32(a_, dxbc::Dest::R(color_temp, 1 << i), packed_temp, packed_temp_components,
                   i * 10, color_temp, i, temp1, temp1_component);
    }
  }
  a_.OpBreak();

  for (u32 i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(
        RenderTargetCache::AddPSIColorFormatFlags(i ? xenos::ColorRenderTargetFormat::k_16_16_16_16
                                                    : xenos::ColorRenderTargetFormat::k_16_16)));
    dxbc::Dest color_components_dest(dxbc::Dest::R(color_temp, i ? 0b1111 : 0b0011));
    a_.OpIBFE(color_components_dest, dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 16),
              dxbc::Src::R(packed_temp, 0b01010000 + packed_temp_components * 0b01010101));
    a_.OpIToF(color_components_dest, dxbc::Src::R(color_temp));
    a_.OpMul(color_components_dest, dxbc::Src::R(color_temp), dxbc::Src::LF(32.0f / 32767.0f));
    a_.OpBreak();
  }

  for (u32 i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT
          : xenos::ColorRenderTargetFormat::k_16_16_FLOAT)));
    dxbc::Dest color_components_dest(dxbc::Dest::R(color_temp, i ? 0b1111 : 0b0011));
    a_.OpUBFE(color_components_dest, dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 16),
              dxbc::Src::R(packed_temp, 0b01010000 + packed_temp_components * 0b01010101));
    a_.OpF16ToF32(color_components_dest, dxbc::Src::R(color_temp));
    a_.OpBreak();
  }

  if (packed_temp != color_temp) {
    a_.OpDefault();
    a_.OpMov(dxbc::Dest::R(color_temp, 0b0011),
             dxbc::Src::R(packed_temp, 0b0100 + packed_temp_components * 0b0101));
    a_.OpBreak();
  }

  a_.OpEndSwitch();
}

void DxbcShaderTranslator::ROV_PackPreClampedColor(u32 rt_index, u32 color_temp, u32 packed_temp,
                                                   u32 packed_temp_components, u32 temp1,
                                                   u32 temp1_component, u32 temp2,
                                                   u32 temp2_component) {
  // The caller must clamp normalized formats before packing.

  REX_ASSERT(color_temp != packed_temp || packed_temp_components == 0);

  dxbc::Dest packed_dest_low(dxbc::Dest::R(packed_temp, 1 << packed_temp_components));
  dxbc::Src packed_src_low(dxbc::Src::R(packed_temp).Select(packed_temp_components));
  dxbc::Dest temp1_dest(dxbc::Dest::R(temp1, 1 << temp1_component));
  dxbc::Src temp1_src(dxbc::Src::R(temp1).Select(temp1_component));
  dxbc::Dest temp2_dest(dxbc::Dest::R(temp2, 1 << temp2_component));
  dxbc::Src temp2_src(dxbc::Src::R(temp2).Select(temp2_component));

  a_.OpMov(dxbc::Dest::R(packed_temp, 1 << (packed_temp_components + 1)), dxbc::Src::LU(0));

  a_.OpSwitch(LoadSystemConstant(
      SystemConstants::Index::kEDRAMRTFormatFlags,
      offsetof(SystemConstants, edram_rt_format_flags) + sizeof(u32) * rt_index, dxbc::Src::kXXXX));

  for (u32 i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA
          : xenos::ColorRenderTargetFormat::k_8_8_8_8)));
    for (u32 j = 0; j < 4; ++j) {
      if (i && j < 3) {
        PreSaturatedLinearToPWLGamma(a_, temp1, temp1_component, color_temp, j, temp1,
                                     temp1_component, temp2, temp2_component);
        a_.OpMAd(temp1_dest, temp1_src, dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
      } else {
        a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp).Select(j), dxbc::Src::LF(255.0f),
                 dxbc::Src::LF(0.5f));
      }
      a_.OpFToU(j ? temp1_dest : packed_dest_low, temp1_src);
      if (j) {
        a_.OpBFI(packed_dest_low, dxbc::Src::LU(8), dxbc::Src::LU(j * 8), temp1_src,
                 packed_src_low);
      }
    }
    a_.OpBreak();
  }

  a_.OpCase(dxbc::Src::LU(
      RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_2_10_10_10)));
  a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)));
  for (u32 i = 0; i < 4; ++i) {
    a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp).Select(i), dxbc::Src::LF(i < 3 ? 1023.0f : 3.0f),
             dxbc::Src::LF(0.5f));
    a_.OpFToU(i ? temp1_dest : packed_dest_low, temp1_src);
    if (i) {
      a_.OpBFI(packed_dest_low, dxbc::Src::LU(i < 3 ? 10 : 2), dxbc::Src::LU(i * 10), temp1_src,
               packed_src_low);
    }
  }
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)));
  a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)));
  {
    // PreClampedFloat32To7e3 allows the destination to be the source.
    PreClampedFloat32To7e3(a_, packed_temp, packed_temp_components, color_temp, 0, temp1,
                           temp1_component);
    for (u32 i = 1; i < 3; ++i) {
      PreClampedFloat32To7e3(a_, temp1, temp1_component, color_temp, i, temp2, temp2_component);
      a_.OpBFI(packed_dest_low, dxbc::Src::LU(10), dxbc::Src::LU(i * 10), temp1_src,
               packed_src_low);
    }
    a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp, dxbc::Src::kWWWW), dxbc::Src::LF(3.0f),
             dxbc::Src::LF(0.5f));
    a_.OpFToU(temp1_dest, temp1_src);
    a_.OpBFI(packed_dest_low, dxbc::Src::LU(2), dxbc::Src::LU(30), temp1_src, packed_src_low);
  }
  a_.OpBreak();

  for (u32 i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(
        RenderTargetCache::AddPSIColorFormatFlags(i ? xenos::ColorRenderTargetFormat::k_16_16_16_16
                                                    : xenos::ColorRenderTargetFormat::k_16_16)));
    for (u32 j = 0; j < (static_cast<u32>(2) << i); ++j) {
      a_.OpGE(temp2_dest, dxbc::Src::R(color_temp).Select(j), dxbc::Src::LF(0.0f));
      a_.OpMovC(temp2_dest, temp2_src, dxbc::Src::LF(0.5f), dxbc::Src::LF(-0.5f));
      a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp).Select(j), dxbc::Src::LF(32767.0f / 32.0f),
               temp2_src);
      dxbc::Dest packed_dest_half(
          dxbc::Dest::R(packed_temp, 1 << (packed_temp_components + (j >> 1))));
      a_.OpFToI((j & 1) ? temp1_dest : packed_dest_half, temp1_src);
      if (j & 1) {
        a_.OpBFI(packed_dest_half, dxbc::Src::LU(16), dxbc::Src::LU(16), temp1_src,
                 dxbc::Src::R(packed_temp).Select(packed_temp_components + (j >> 1)));
      }
    }
    a_.OpBreak();
  }

  for (u32 i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(RenderTargetCache::AddPSIColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT
          : xenos::ColorRenderTargetFormat::k_16_16_FLOAT)));
    for (u32 j = 0; j < (static_cast<u32>(2) << i); ++j) {
      dxbc::Dest packed_dest_half(
          dxbc::Dest::R(packed_temp, 1 << (packed_temp_components + (j >> 1))));
      a_.OpF32ToF16((j & 1) ? temp1_dest : packed_dest_half, dxbc::Src::R(color_temp).Select(j));
      if (j & 1) {
        a_.OpBFI(packed_dest_half, dxbc::Src::LU(16), dxbc::Src::LU(16), temp1_src,
                 dxbc::Src::R(packed_temp).Select(packed_temp_components + (j >> 1)));
      }
    }
    a_.OpBreak();
  }

  if (packed_temp != color_temp) {
    a_.OpDefault();
    a_.OpMov(dxbc::Dest::R(packed_temp, 0b11 << packed_temp_components),
             dxbc::Src::R(color_temp, 0b0100 << (packed_temp_components * 2)));
    a_.OpBreak();
  }

  a_.OpEndSwitch();
}

void DxbcShaderTranslator::ROV_HandleColorBlendFactorCases(u32 src_temp, u32 dst_temp,
                                                           u32 factor_temp) {
  dxbc::Dest factor_dest(dxbc::Dest::R(factor_temp, 0b0111));
  dxbc::Src one_src(dxbc::Src::LF(1.0f));

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOne)));
  a_.OpMov(factor_dest, one_src);
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kSrcColor)));
  if (factor_temp != src_temp) {
    a_.OpMov(factor_dest, dxbc::Src::R(src_temp));
  }
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusSrcColor)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(src_temp));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kSrcAlpha)));
  a_.OpMov(factor_dest, dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusSrcAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kDstColor)));
  if (factor_temp != dst_temp) {
    a_.OpMov(factor_dest, dxbc::Src::R(dst_temp));
  }
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusDstColor)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(dst_temp));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kDstAlpha)));
  a_.OpMov(factor_dest, dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusDstAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kConstantColor)));
  a_.OpMov(factor_dest,
           LoadSystemConstant(SystemConstants::Index::kEDRAMBlendConstant,
                              offsetof(SystemConstants, edram_blend_constant), dxbc::Src::kXYZW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusConstantColor)));
  a_.OpAdd(factor_dest, one_src,
           -LoadSystemConstant(SystemConstants::Index::kEDRAMBlendConstant,
                               offsetof(SystemConstants, edram_blend_constant), dxbc::Src::kXYZW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kConstantAlpha)));
  a_.OpMov(factor_dest,
           LoadSystemConstant(SystemConstants::Index::kEDRAMBlendConstant,
                              offsetof(SystemConstants, edram_blend_constant), dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusConstantAlpha)));
  a_.OpAdd(factor_dest, one_src,
           -LoadSystemConstant(SystemConstants::Index::kEDRAMBlendConstant,
                               offsetof(SystemConstants, edram_blend_constant), dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kSrcAlphaSaturate)));
  a_.OpAdd(dxbc::Dest::R(factor_temp, 0b0001), one_src, -dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpMin(factor_dest, dxbc::Src::R(src_temp, dxbc::Src::kWWWW),
           dxbc::Src::R(factor_temp, dxbc::Src::kXXXX));
  a_.OpBreak();

  a_.OpDefault();
  a_.OpMov(factor_dest, dxbc::Src::LF(0.0f));
  a_.OpBreak();
}

void DxbcShaderTranslator::ROV_HandleAlphaBlendFactorCases(u32 src_temp, u32 dst_temp,
                                                           u32 factor_temp, u32 factor_component) {
  dxbc::Dest factor_dest(dxbc::Dest::R(factor_temp, 1 << factor_component));
  dxbc::Src one_src(dxbc::Src::LF(1.0f));

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOne)));
  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kSrcAlphaSaturate)));
  a_.OpMov(factor_dest, one_src);
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kSrcColor)));
  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kSrcAlpha)));
  if (factor_temp != src_temp || factor_component != 3) {
    a_.OpMov(factor_dest, dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  }
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusSrcColor)));
  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusSrcAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kDstColor)));
  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kDstAlpha)));
  if (factor_temp != dst_temp || factor_component != 3) {
    a_.OpMov(factor_dest, dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  }
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusDstColor)));
  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusDstAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kConstantColor)));
  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kConstantAlpha)));
  a_.OpMov(factor_dest,
           LoadSystemConstant(SystemConstants::Index::kEDRAMBlendConstant,
                              offsetof(SystemConstants, edram_blend_constant), dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusConstantColor)));
  a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::BlendFactor::kOneMinusConstantAlpha)));
  a_.OpAdd(factor_dest, one_src,
           -LoadSystemConstant(SystemConstants::Index::kEDRAMBlendConstant,
                               offsetof(SystemConstants, edram_blend_constant), dxbc::Src::kWWWW));
  a_.OpBreak();

  a_.OpDefault();
  a_.OpMov(factor_dest, dxbc::Src::LF(0.0f));
  a_.OpBreak();
}

void DxbcShaderTranslator::CompletePixelShader_WriteToRTVs() {
  u32 shader_writes_color_targets = current_shader().writes_color_targets();
  if (!shader_writes_color_targets) {
    return;
  }

  u32 gamma_temp = PushSystemTemp();
  for (u32 i = 0; i < 4; ++i) {
    if (!(shader_writes_color_targets & (1 << i))) {
      continue;
    }
    u32 system_temp_color = system_temps_color_[i];
    a_.OpMul(dxbc::Dest::R(system_temp_color), dxbc::Src::R(system_temp_color),
             LoadSystemConstant(SystemConstants::Index::kColorExpBias,
                                offsetof(SystemConstants, color_exp_bias) + sizeof(f32) * i,
                                dxbc::Src::kXXXX));
    if (gamma_render_target_as_unorm8_) {
      a_.OpAnd(dxbc::Dest::R(gamma_temp, 0b0001), LoadFlagsSystemConstant(),
               dxbc::Src::LU(kSysFlag_ConvertColor0ToGamma << i));
      a_.OpIf(true, dxbc::Src::R(gamma_temp, dxbc::Src::kXXXX));
      a_.OpMov(dxbc::Dest::R(system_temp_color, 0b0111), dxbc::Src::R(system_temp_color), true);
      for (u32 j = 0; j < 3; ++j) {
        PreSaturatedLinearToPWLGamma(a_, system_temp_color, j, system_temp_color, j, gamma_temp, 0,
                                     gamma_temp, 1);
      }
      a_.OpEndIf();
    }
    a_.OpMov(dxbc::Dest::O(i), dxbc::Src::R(system_temp_color));
  }
  PopSystemTemp();
}

void DxbcShaderTranslator::CompletePixelShader_DSV_DepthTo24Bit() {
  bool shader_writes_depth = current_shader().writes_depth();

  if (!DSV_IsWritingFloat24Depth()) {
    if (shader_writes_depth) {
      // Float24 oDepth is scaled 0...1 to host 0...0.5, viewport scaling skips oDepth.
      a_.OpAnd(dxbc::Dest::R(system_temp_depth_stencil_, 0b0010), LoadFlagsSystemConstant(),
               dxbc::Src::LU(kSysFlag_DepthFloat24));
      a_.OpIf(true, dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kYYYY));
      a_.OpMul(dxbc::Dest::R(system_temp_depth_stencil_, 0b0001),
               dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kXXXX), dxbc::Src::LF(0.5f));
      a_.OpEndIf();
      a_.OpMov(dxbc::Dest::ODepth(), dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kXXXX));
    }
    return;
  }

  u32 temp;
  if (shader_writes_depth) {
    temp = system_temp_depth_stencil_;
  } else {
    // Remap the sample depth from host 0...0.5 back to 0...1 and saturate it for the conversion.
    temp = PushSystemTemp();
    in_position_used_ |= 0b0100;
    a_.OpMul(dxbc::Dest::R(temp, 0b0001), dxbc::Src::V1D(in_reg_ps_position_, dxbc::Src::kZZZZ),
             dxbc::Src::LF(2.0f), true);
  }

  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  dxbc::Dest temp_y_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src temp_y_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));

  if (GetDxbcShaderModification().pixel.depth_stencil_mode ==
      Modification::DepthStencilMode::kFloat24Truncating) {
    // 20e4 drops 3 mantissa bits at exponent -14 or above, up to 23 at -34, anything lower is 0.
    dxbc::Dest truncate_dest(shader_writes_depth ? dxbc::Dest::ODepth() : dxbc::Dest::ODepthLE());
    a_.OpUGE(temp_y_dest, temp_x_src, dxbc::Src::LU(0x2E800000));
    a_.OpIf(true, temp_y_src);
    {
      a_.OpUBFE(temp_y_dest, dxbc::Src::LU(8), dxbc::Src::LU(23), temp_x_src);
      a_.OpIAdd(temp_y_dest, dxbc::Src::LI(116), -temp_y_src);
      a_.OpIMax(temp_y_dest, temp_y_src, dxbc::Src::LI(3));
      a_.OpBFI(temp_x_dest, temp_y_src, dxbc::Src::LU(0), dxbc::Src::LU(0), temp_x_src);
      a_.OpMul(truncate_dest, temp_x_src, dxbc::Src::LF(0.5f));
    }
    a_.OpElse();
    a_.OpMov(truncate_dest, dxbc::Src::LF(0.0f));
    a_.OpEndIf();
  } else {
    PreClampedDepthTo20e4(a_, temp, 0, temp, 0, temp, 1, true, false);
    Depth20e4To32(a_, dxbc::Dest::ODepth(), temp, 0, 0, temp, 0, temp, 1, true);
  }

  if (!shader_writes_depth) {
    PopSystemTemp();
  }
}

void DxbcShaderTranslator::CompletePixelShader_AlphaToMaskSample(
    bool initialize, u32 sample_index, f32 threshold_base, dxbc::Src threshold_offset,
    f32 threshold_offset_scale, u32 coverage_temp, u32 coverage_temp_component, u32 temp,
    u32 temp_component) {
  dxbc::Dest temp_dest(dxbc::Dest::R(temp, 1 << temp_component));
  dxbc::Src temp_src(dxbc::Src::R(temp).Select(temp_component));
  a_.OpMAd(temp_dest, threshold_offset, dxbc::Src::LF(-threshold_offset_scale),
           dxbc::Src::LF(threshold_base));
  // A NaN alpha counts as not covered.
  a_.OpGE(temp_dest, dxbc::Src::R(system_temps_color_[0], dxbc::Src::kWWWW), temp_src);
  dxbc::Dest coverage_dest(dxbc::Dest::R(coverage_temp, 1 << coverage_temp_component));
  dxbc::Src coverage_src(dxbc::Src::R(coverage_temp).Select(coverage_temp_component));
  if (edram_rov_used_) {
    REX_ASSERT(coverage_temp != temp || coverage_temp_component != temp_component);
    // A failed alpha test clears both the coverage and the deferred depth/stencil write bits.
    a_.OpOr(temp_dest, temp_src, dxbc::Src::LU(~(static_cast<u32>(0b00010001) << sample_index)));
    a_.OpAnd(coverage_dest, coverage_src, temp_src);
  } else {
    if (initialize) {
      REX_ASSERT(coverage_temp != temp || coverage_temp_component != temp_component);
      a_.OpAnd(coverage_dest, temp_src, dxbc::Src::LU(static_cast<u32>(1) << sample_index));
    } else {
      a_.OpAnd(temp_dest, temp_src, dxbc::Src::LU(static_cast<u32>(1) << sample_index));
      a_.OpOr(coverage_dest, coverage_src, temp_src);
    }
  }
}

void DxbcShaderTranslator::CompletePixelShader_AlphaToMask() {
  if (!current_shader().writes_color_target(0) || IsForceEarlyDepthStencilGlobalFlagEnabled()) {
    return;
  }

  if (!edram_rov_used_) {
    a_.OpMov(dxbc::Dest::OMask(), dxbc::Src::LU(UINT32_MAX));
  }

  dxbc::Src alpha_to_mask_constant_src(LoadSystemConstant(SystemConstants::Index::kAlphaToMask,
                                                          offsetof(SystemConstants, alpha_to_mask),
                                                          dxbc::Src::kXXXX));
  a_.OpIf(true, alpha_to_mask_constant_src);

  u32 temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));

  in_position_used_ |= 0b0011;
  a_.OpFToU(dxbc::Dest::R(temp, 0b0011), dxbc::Src::V1D(in_reg_ps_position_));
  a_.OpAnd(dxbc::Dest::R(temp, 0b0010), dxbc::Src::R(temp, dxbc::Src::kYYYY), dxbc::Src::LU(1));
  a_.OpBFI(temp_x_dest, dxbc::Src::LU(1), dxbc::Src::LU(1), temp_x_src,
           dxbc::Src::R(temp, dxbc::Src::kYYYY));
  a_.OpIShL(temp_x_dest, temp_x_src, dxbc::Src::LU(1));
  a_.OpUBFE(temp_x_dest, dxbc::Src::LU(2), temp_x_src, alpha_to_mask_constant_src);
  a_.OpUToF(temp_x_dest, temp_x_src);

  u32 coverage_temp = edram_rov_used_ ? system_temp_rov_params_ : temp;
  u32 coverage_temp_component = edram_rov_used_ ? 0 : 2;

  a_.OpIf(true, LoadSystemConstant(SystemConstants::Index::kSampleCountLog2,
                                   offsetof(SystemConstants, sample_count_log2), dxbc::Src::kYYYY));
  {
    a_.OpIf(true,
            LoadSystemConstant(SystemConstants::Index::kSampleCountLog2,
                               offsetof(SystemConstants, sample_count_log2), dxbc::Src::kXXXX));
    // Check sample 0 first, CompletePixelShader_AlphaToMaskSample initializes the result with it.
    CompletePixelShader_AlphaToMaskSample(true, 0, 0.75f, temp_x_src, 1.0f / 16.0f, coverage_temp,
                                          coverage_temp_component, temp, 1);
    CompletePixelShader_AlphaToMaskSample(false, 1, 0.25f, temp_x_src, 1.0f / 16.0f, coverage_temp,
                                          coverage_temp_component, temp, 1);
    CompletePixelShader_AlphaToMaskSample(false, 2, 0.5f, temp_x_src, 1.0f / 16.0f, coverage_temp,
                                          coverage_temp_component, temp, 1);
    CompletePixelShader_AlphaToMaskSample(false, 3, 1.0f, temp_x_src, 1.0f / 16.0f, coverage_temp,
                                          coverage_temp_component, temp, 1);
    // Without ROV, native 2x maps top to 1 and bottom to 0, 2x as 4x maps top to 0, bottom to 3.
    a_.OpElse();
    CompletePixelShader_AlphaToMaskSample(true, (!edram_rov_used_ && msaa_2x_supported_) ? 1 : 0,
                                          0.5f, temp_x_src, 1.0f / 8.0f, coverage_temp,
                                          coverage_temp_component, temp, 1);
    CompletePixelShader_AlphaToMaskSample(false, edram_rov_used_ ? 1 : (msaa_2x_supported_ ? 0 : 3),
                                          1.0f, temp_x_src, 1.0f / 8.0f, coverage_temp,
                                          coverage_temp_component, temp, 1);
    a_.OpEndIf();
  }
  a_.OpElse();
  CompletePixelShader_AlphaToMaskSample(true, 0, 1.0f, temp_x_src, 1.0f / 4.0f, coverage_temp,
                                        coverage_temp_component, temp, 1);
  a_.OpEndIf();

  if (edram_rov_used_) {
    // The mask covers bits 0:3 and 4:7 as stencil may need writing for samples dropped in 0:3.
    a_.OpAnd(temp_x_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(0b11111111));
    a_.OpRetC(false, temp_x_src);
  } else {
    dxbc::Src coverage_src(dxbc::Src::R(coverage_temp, coverage_temp_component));
    a_.OpDiscard(false, coverage_src);
    a_.OpMov(dxbc::Dest::OMask(), coverage_src);
  }

  PopSystemTemp();

  a_.OpEndIf();
}

void DxbcShaderTranslator::CompletePixelShader_WriteToROV() {
  u32 temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  dxbc::Dest temp_y_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src temp_y_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));
  dxbc::Dest temp_z_dest(dxbc::Dest::R(temp, 0b0100));
  dxbc::Src temp_z_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));
  dxbc::Dest temp_w_dest(dxbc::Dest::R(temp, 0b1000));
  dxbc::Src temp_w_src(dxbc::Src::R(temp, dxbc::Src::kWWWW));

  u32 tile_width = xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x_;

  if (ROV_IsDepthStencilEarly()) {
    for (u32 i = 0; i < 4; ++i) {
      a_.OpAnd(temp_x_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
               dxbc::Src::LU(1 << (4 + i)));
      a_.OpIf(true, temp_x_src);
      {
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(dxbc::Dest::U(uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM)),
                           dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1,
                           dxbc::Src::R(system_temp_depth_stencil_).Select(i));
      }
      a_.OpEndIf();
      // Samples are at +0, +(80*scale_x), +1 and +(80*scale_x+1).
      if (i < 3) {
        a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
                  dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
                  dxbc::Src::LI((i & 1) ? -static_cast<i32>(tile_width) + 2 - static_cast<i32>(i)
                                        : static_cast<i32>(tile_width)));
      }
    }
  } else {
    ROV_DepthStencilTest();
  }

  if (!is_depth_only_pixel_shader_) {
    a_.OpAnd(temp_x_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(0b1111));
    a_.OpRetC(false, temp_x_src);
  }

  u32 shader_writes_color_targets = current_shader().writes_color_targets();
  u32 edram_size_32bpp_samples = (xenos::kEDRAMTileHeightSamples * draw_resolution_scale_y_) *
                                 tile_width * xenos::kEDRAMTileCount;
  for (u32 i = 0; i < 4; ++i) {
    if (!(shader_writes_color_targets & (1 << i))) {
      continue;
    }

    // Even render targets use XY and odd ones use ZW.
    dxbc::Src keep_mask_src(LoadSystemConstant(
        SystemConstants::Index::kEDRAMRTKeepMask,
        offsetof(SystemConstants, edram_rt_keep_mask) + sizeof(u32) * 2 * i, 0b0100));

    a_.OpAnd(temp_x_dest, keep_mask_src.SelectFromSwizzled(0), keep_mask_src.SelectFromSwizzled(1));
    a_.OpNot(temp_x_dest, temp_x_src);
    a_.OpMovC(temp_x_dest, temp_x_src, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
              dxbc::Src::LU(0));
    a_.OpAnd(temp_x_dest, temp_x_src, dxbc::Src::LU(1 << (8 + i)));
    a_.OpIf(true, temp_x_src);

    a_.OpMul(dxbc::Dest::R(system_temps_color_[i]), dxbc::Src::R(system_temps_color_[i]),
             LoadSystemConstant(SystemConstants::Index::kColorExpBias,
                                offsetof(SystemConstants, color_exp_bias) + sizeof(f32) * i,
                                dxbc::Src::kXXXX));

    dxbc::Src rt_format_flags_src(LoadSystemConstant(
        SystemConstants::Index::kEDRAMRTFormatFlags,
        offsetof(SystemConstants, edram_rt_format_flags) + sizeof(u32) * i, dxbc::Src::kXXXX));

    a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b0010), rt_format_flags_src,
             dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_64bpp));
    a_.OpMovC(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ));
    a_.OpIAdd(
        dxbc::Dest::R(system_temp_rov_params_, 0b0010),
        dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
        LoadSystemConstant(SystemConstants::Index::kEDRAMRTBaseDwordsScaled,
                           offsetof(SystemConstants, edram_rt_base_dwords_scaled) + sizeof(u32) * i,
                           dxbc::Src::kXXXX));
    a_.OpUDiv(dxbc::Dest::Null(), dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::LU(edram_size_32bpp_samples));

    dxbc::Src rt_blend_factors_ops_src(LoadSystemConstant(
        SystemConstants::Index::kEDRAMRTBlendFactorsOps,
        offsetof(SystemConstants, edram_rt_blend_factors_ops) + sizeof(u32) * i, dxbc::Src::kXXXX));
    dxbc::Src rt_clamp_vec_src(LoadSystemConstant(
        SystemConstants::Index::kEDRAMRTClamp,
        offsetof(SystemConstants, edram_rt_clamp) + sizeof(f32) * 4 * i, dxbc::Src::kXYZW));
    a_.OpIEq(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(0x00010001));
    a_.OpIf(true, temp_x_src);
    {
      a_.OpMax(dxbc::Dest::R(system_temps_color_[i]), dxbc::Src::R(system_temps_color_[i]),
               rt_clamp_vec_src.Swizzle(0b01000000));
      a_.OpMin(dxbc::Dest::R(system_temps_color_[i]), dxbc::Src::R(system_temps_color_[i]),
               rt_clamp_vec_src.Swizzle(0b11101010));
      ROV_PackPreClampedColor(i, system_temps_color_[i], temp, 0, temp, 2, temp, 3);
    }
    a_.OpElse();
    {
      a_.OpAnd(temp_x_dest, rt_format_flags_src,
               dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_FixedPointColor));
      a_.OpIf(true, temp_x_src);
      {
        a_.OpMax(dxbc::Dest::R(system_temps_color_[i], 0b0111),
                 dxbc::Src::R(system_temps_color_[i]), rt_clamp_vec_src.Select(0));
        a_.OpMin(dxbc::Dest::R(system_temps_color_[i], 0b0111),
                 dxbc::Src::R(system_temps_color_[i]), rt_clamp_vec_src.Select(2));
      }
      a_.OpEndIf();

      a_.OpAnd(temp_x_dest, rt_format_flags_src,
               dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_FixedPointAlpha));
      a_.OpIf(true, temp_x_src);
      {
        a_.OpMax(dxbc::Dest::R(system_temps_color_[i], 0b1000),
                 dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                 rt_clamp_vec_src.Select(1));
        a_.OpMin(dxbc::Dest::R(system_temps_color_[i], 0b1000),
                 dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                 rt_clamp_vec_src.Select(3));
      }
      a_.OpEndIf();
      a_.OpMov(dxbc::Dest::R(temp, 0b0011), dxbc::Src::LU(0));
    }
    a_.OpEndIf();

    for (u32 j = 0; j < 4; ++j) {
      a_.OpAnd(temp_z_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
               dxbc::Src::LU(1 << j));

      a_.OpIf(true, temp_z_src);

      a_.OpOr(temp_z_dest, keep_mask_src.SelectFromSwizzled(0),
              keep_mask_src.SelectFromSwizzled(1));
      a_.OpMovC(temp_z_dest, temp_z_src, dxbc::Src::LU(0), rt_blend_factors_ops_src);
      a_.OpINE(temp_z_dest, temp_z_src, dxbc::Src::LU(0x00010001));
      a_.OpIf(true, temp_z_src);
      {
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpLdUAVTyped(temp_z_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1,
                        dxbc::Src::U(uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM),
                                     dxbc::Src::kXXXX));
        a_.OpAnd(temp_w_dest, rt_format_flags_src,
                 dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_64bpp));
        a_.OpIf(true, temp_w_src);
        {
          a_.OpIAdd(temp_w_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
                    dxbc::Src::LU(1));
          if (uav_index_edram_ == kBindingIndexUnallocated) {
            uav_index_edram_ = uav_count_++;
          }
          a_.OpLdUAVTyped(temp_w_dest, temp_w_src, 1,
                          dxbc::Src::U(uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM),
                                       dxbc::Src::kXXXX));
        }
        a_.OpElse();
        { a_.OpMov(temp_w_dest, dxbc::Src::LU(0)); }
        a_.OpEndIf();

        u32 color_temp = PushSystemTemp();
        dxbc::Dest color_temp_rgb_dest(dxbc::Dest::R(color_temp, 0b0111));
        dxbc::Dest color_temp_a_dest(dxbc::Dest::R(color_temp, 0b1000));
        dxbc::Src color_temp_src(dxbc::Src::R(color_temp));
        dxbc::Src color_temp_a_src(dxbc::Src::R(color_temp, dxbc::Src::kWWWW));

        a_.OpINE(dxbc::Dest::R(color_temp, 0b0001), rt_blend_factors_ops_src,
                 dxbc::Src::LU(0x00010001));
        a_.OpIf(true, dxbc::Src::R(color_temp, dxbc::Src::kXXXX));
        {
          ROV_UnpackColor(i, temp, 2, color_temp, temp, 0, temp, 1);

          a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << (5 + 1)));
          a_.OpIf(false, temp_x_src);
          {
            u32 blend_src_temp = PushSystemTemp();
            dxbc::Dest blend_src_temp_rgb_dest(dxbc::Dest::R(blend_src_temp, 0b0111));
            dxbc::Src blend_src_temp_src(dxbc::Src::R(blend_src_temp));

            a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU((1 << 5) - 1));
            a_.OpIf(true, temp_x_src);
            {
              a_.OpSwitch(temp_x_src);
              ROV_HandleColorBlendFactorCases(system_temps_color_[i], color_temp, blend_src_temp);
              a_.OpEndSwitch();
              a_.OpAnd(temp_x_dest, rt_format_flags_src,
                       dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_FixedPointColor));
              a_.OpIf(true, temp_x_src);
              {
                a_.OpMax(blend_src_temp_rgb_dest, blend_src_temp_src, rt_clamp_vec_src.Select(0));
                a_.OpMin(blend_src_temp_rgb_dest, blend_src_temp_src, rt_clamp_vec_src.Select(2));
              }
              a_.OpEndIf();
              a_.OpMul(blend_src_temp_rgb_dest, dxbc::Src::R(system_temps_color_[i]),
                       blend_src_temp_src);
              a_.OpIf(true, temp_x_src);
              {
                a_.OpMax(blend_src_temp_rgb_dest, blend_src_temp_src, rt_clamp_vec_src.Select(0));
                a_.OpMin(blend_src_temp_rgb_dest, blend_src_temp_src, rt_clamp_vec_src.Select(2));
              }
              a_.OpEndIf();
              a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << (5 + 2)));
              a_.OpMovC(blend_src_temp_rgb_dest, temp_x_src, -blend_src_temp_src,
                        blend_src_temp_src);
            }
            a_.OpElse();
            { a_.OpMov(blend_src_temp_rgb_dest, dxbc::Src::LF(0.0f)); }
            a_.OpEndIf();

            a_.OpUBFE(temp_x_dest, dxbc::Src::LU(5), dxbc::Src::LU(8), rt_blend_factors_ops_src);
            a_.OpIf(true, temp_x_src);
            {
              u32 blend_dest_factor_temp = PushSystemTemp();
              dxbc::Src blend_dest_factor_temp_src(dxbc::Src::R(blend_dest_factor_temp));
              a_.OpSwitch(temp_x_src);
              ROV_HandleColorBlendFactorCases(system_temps_color_[i], color_temp,
                                              blend_dest_factor_temp);
              a_.OpEndSwitch();
              a_.OpAnd(temp_x_dest, rt_format_flags_src,
                       dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_FixedPointColor));
              a_.OpIf(true, temp_x_src);
              {
                a_.OpMax(dxbc::Dest::R(blend_dest_factor_temp, 0b0111), blend_dest_factor_temp_src,
                         rt_clamp_vec_src.Select(0));
                a_.OpMin(dxbc::Dest::R(blend_dest_factor_temp, 0b0111), blend_dest_factor_temp_src,
                         rt_clamp_vec_src.Select(2));
              }
              a_.OpEndIf();
              a_.OpMul(color_temp_rgb_dest, color_temp_src, blend_dest_factor_temp_src);
              PopSystemTemp();
              a_.OpIf(true, temp_x_src);
              {
                a_.OpMax(color_temp_rgb_dest, color_temp_src, rt_clamp_vec_src.Select(0));
                a_.OpMin(color_temp_rgb_dest, color_temp_src, rt_clamp_vec_src.Select(2));
              }
              a_.OpEndIf();
              a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << 5));
              a_.OpMovC(temp_x_dest, temp_x_src, dxbc::Src::LF(-1.0f), dxbc::Src::LF(1.0f));
              a_.OpMAd(color_temp_rgb_dest, color_temp_src, temp_x_src, blend_src_temp_src);
            }
            a_.OpElse();
            { a_.OpMov(color_temp_rgb_dest, blend_src_temp_src); }
            a_.OpEndIf();

            PopSystemTemp();

            a_.OpMax(color_temp_rgb_dest, color_temp_src, rt_clamp_vec_src.Select(0));
            a_.OpMin(color_temp_rgb_dest, color_temp_src, rt_clamp_vec_src.Select(2));
          }
          a_.OpElse();
          {
            a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << 5));
            a_.OpIf(true, temp_x_src);
            { a_.OpMax(color_temp_rgb_dest, dxbc::Src::R(system_temps_color_[i]), color_temp_src); }
            a_.OpElse();
            { a_.OpMin(color_temp_rgb_dest, dxbc::Src::R(system_temps_color_[i]), color_temp_src); }
            a_.OpEndIf();
          }
          a_.OpEndIf();

          a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << (21 + 1)));
          a_.OpIf(false, temp_x_src);
          {
            a_.OpUBFE(temp_x_dest, dxbc::Src::LU(5), dxbc::Src::LU(16), rt_blend_factors_ops_src);
            a_.OpIf(true, temp_x_src);
            {
              a_.OpSwitch(temp_x_src);
              ROV_HandleAlphaBlendFactorCases(system_temps_color_[i], color_temp, temp, 0);
              a_.OpEndSwitch();
              a_.OpAnd(temp_y_dest, rt_format_flags_src,
                       dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_FixedPointAlpha));
              a_.OpIf(true, temp_y_src);
              {
                a_.OpMax(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(1));
                a_.OpMin(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(3));
              }
              a_.OpEndIf();
              a_.OpMul(temp_x_dest, dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                       temp_x_src);
              a_.OpIf(true, temp_y_src);
              {
                a_.OpMax(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(1));
                a_.OpMin(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(3));
              }
              a_.OpEndIf();
              a_.OpAnd(temp_y_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << (21 + 2)));
              a_.OpMovC(temp_x_dest, temp_y_src, -temp_x_src, temp_x_src);
            }
            a_.OpElse();
            { a_.OpMov(temp_x_dest, dxbc::Src::LF(0.0f)); }
            a_.OpEndIf();

            a_.OpUBFE(temp_y_dest, dxbc::Src::LU(5), dxbc::Src::LU(24), rt_blend_factors_ops_src);
            a_.OpIf(true, temp_y_src);
            {
              a_.OpSwitch(temp_y_src);
              ROV_HandleAlphaBlendFactorCases(system_temps_color_[i], color_temp, temp, 1);
              a_.OpEndSwitch();
              u32 alpha_is_fixed_temp = PushSystemTemp();
              a_.OpAnd(dxbc::Dest::R(alpha_is_fixed_temp, 0b0001), rt_format_flags_src,
                       dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_FixedPointAlpha));
              a_.OpIf(true, dxbc::Src::R(alpha_is_fixed_temp, dxbc::Src::kXXXX));
              {
                a_.OpMax(temp_y_dest, temp_y_src, rt_clamp_vec_src.Select(1));
                a_.OpMin(temp_y_dest, temp_y_src, rt_clamp_vec_src.Select(3));
              }
              a_.OpEndIf();
              a_.OpMul(color_temp_a_dest, color_temp_a_src, temp_y_src);
              a_.OpIf(true, dxbc::Src::R(alpha_is_fixed_temp, dxbc::Src::kXXXX));
              PopSystemTemp();
              {
                a_.OpMax(color_temp_a_dest, color_temp_a_src, rt_clamp_vec_src.Select(1));
                a_.OpMin(color_temp_a_dest, color_temp_a_src, rt_clamp_vec_src.Select(3));
              }
              a_.OpEndIf();
              a_.OpAnd(temp_y_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << 21));
              a_.OpMovC(temp_y_dest, temp_y_src, dxbc::Src::LF(-1.0f), dxbc::Src::LF(1.0f));
              a_.OpMAd(color_temp_a_dest, color_temp_a_src, temp_y_src, temp_x_src);
            }
            a_.OpElse();
            { a_.OpMov(color_temp_a_dest, temp_x_src); }
            a_.OpEndIf();

            a_.OpMax(color_temp_a_dest, color_temp_a_src, rt_clamp_vec_src.Select(1));
            a_.OpMin(color_temp_a_dest, color_temp_a_src, rt_clamp_vec_src.Select(3));
          }
          a_.OpElse();
          {
            a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(1 << 21));
            a_.OpIf(true, temp_x_src);
            {
              a_.OpMax(color_temp_a_dest, dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                       color_temp_a_src);
            }
            a_.OpElse();
            {
              a_.OpMin(color_temp_a_dest, dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                       color_temp_a_src);
            }
            a_.OpEndIf();
          }
          a_.OpEndIf();

          u32 color_pack_temp = PushSystemTemp();
          ROV_PackPreClampedColor(i, color_temp, temp, 0, color_pack_temp, 0, color_pack_temp, 1);
          PopSystemTemp();
        }
        a_.OpEndIf();

        a_.OpAnd(dxbc::Dest::R(temp, 0b1100), dxbc::Src::R(temp),
                 keep_mask_src.SwizzleSwizzled(0b0100 << 4));
        a_.OpNot(dxbc::Dest::R(color_temp, 0b0011), keep_mask_src);
        PopSystemTemp();
        a_.OpAnd(dxbc::Dest::R(temp, 0b0011), dxbc::Src::R(temp), dxbc::Src::R(color_temp));
        a_.OpOr(dxbc::Dest::R(temp, 0b0011), dxbc::Src::R(temp), dxbc::Src::R(temp, 0b1110));
      }
      a_.OpEndIf();

      if (uav_index_edram_ == kBindingIndexUnallocated) {
        uav_index_edram_ = uav_count_++;
      }
      a_.OpStoreUAVTyped(dxbc::Dest::U(uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM)),
                         dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1, temp_x_src);
      a_.OpAnd(temp_z_dest, rt_format_flags_src,
               dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_64bpp));
      a_.OpIf(true, temp_z_src);
      {
        // temp.x must be preserved since packing is done once for all samples when not blending.
        a_.OpIAdd(temp_z_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
                  dxbc::Src::LU(1));
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(dxbc::Dest::U(uav_index_edram_, std::to_underlying(UAVRegister::kEDRAM)),
                           temp_z_src, 1, temp_y_src);
      }
      a_.OpEndIf();

      a_.OpEndIf();

      // Samples are at +0, +(80*scale_x), +dwpp and +(80*scale_x+dwpp).
      if (j < 3) {
        if (j & 1) {
          a_.OpAnd(temp_z_dest, rt_format_flags_src,
                   dxbc::Src::LU(RenderTargetCache::kPSIColorFormatFlag_64bpp));
          a_.OpMovC(temp_z_dest, temp_z_src,
                    dxbc::Src::LI(-static_cast<i32>(tile_width) + 2 * (2 - static_cast<i32>(j))),
                    dxbc::Src::LI(-static_cast<i32>(tile_width) + (2 - static_cast<i32>(j))));
          a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
                    dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), temp_z_src);
        } else {
          a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
                    dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
                    dxbc::Src::LU(tile_width));
        }
      }
    }

    a_.OpEndIf();
  }

  PopSystemTemp();
}

void DxbcShaderTranslator::CompletePixelShader() {
  if (is_depth_only_pixel_shader_) {
    if (edram_rov_used_) {
      CompletePixelShader_WriteToROV();
    }
    return;
  }

  if (current_shader().writes_color_target(0) && !IsForceEarlyDepthStencilGlobalFlagEnabled()) {
    if (edram_rov_used_) {
      u32 rt_0_written_temp = PushSystemTemp();
      a_.OpAnd(dxbc::Dest::R(rt_0_written_temp, 0b0001),
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX), dxbc::Src::LU(1 << 8));
      a_.OpIf(true, dxbc::Src::R(rt_0_written_temp, dxbc::Src::kXXXX));
      PopSystemTemp();
    }

    u32 alpha_test_temp = PushSystemTemp();
    dxbc::Dest alpha_test_mask_dest(dxbc::Dest::R(alpha_test_temp, 0b0001));
    dxbc::Src alpha_test_mask_src(dxbc::Src::R(alpha_test_temp, dxbc::Src::kXXXX));
    dxbc::Dest alpha_test_op_dest(dxbc::Dest::R(alpha_test_temp, 0b0010));
    dxbc::Src alpha_test_op_src(dxbc::Src::R(alpha_test_temp, dxbc::Src::kYYYY));
    dxbc::Dest alpha_test_fuzzy_diff_dest(dxbc::Dest::R(alpha_test_temp, 0b0100));
    dxbc::Src alpha_test_fuzzy_diff_src(dxbc::Src::R(alpha_test_temp, dxbc::Src::kZZZZ));
    a_.OpUBFE(alpha_test_mask_dest, dxbc::Src::LU(3), dxbc::Src::LU(kSysFlag_AlphaPassIfLess_Shift),
              LoadFlagsSystemConstant());
    a_.OpINE(alpha_test_op_dest, alpha_test_mask_src,
             dxbc::Src::LU(std::to_underlying(xenos::CompareFunction::kAlways)));
    a_.OpIf(true, alpha_test_op_src);
    {
      dxbc::Src alpha_src(dxbc::Src::R(system_temps_color_[0], dxbc::Src::kWWWW));
      dxbc::Src alpha_test_reference_src(
          LoadSystemConstant(SystemConstants::Index::kAlphaTestReference,
                             offsetof(SystemConstants, alpha_test_reference), dxbc::Src::kXXXX));
      dxbc::Src fuzzy_epsilon = dxbc::Src::LF(1e-3f);
      // Not equal must be true for NaN, unlike less or greater.
      a_.OpIEq(alpha_test_op_dest, alpha_test_mask_src,
               dxbc::Src::LU(std::to_underlying(xenos::CompareFunction::kNotEqual)));
      a_.OpIf(true, alpha_test_op_src);
      {
        if (REXCVAR_GET(::rex::gpu::cvars::use_fuzzy_alpha_epsilon)) {
          a_.OpAdd(alpha_test_fuzzy_diff_dest, alpha_src, -alpha_test_reference_src);
          a_.OpLT(alpha_test_mask_dest, alpha_test_fuzzy_diff_src.Abs(), fuzzy_epsilon);
          a_.OpNot(alpha_test_mask_dest, alpha_test_mask_src);
        } else {
          a_.OpNE(alpha_test_mask_dest, alpha_src, alpha_test_reference_src);
        }
      }
      a_.OpElse();
      {
        if (REXCVAR_GET(::rex::gpu::cvars::use_fuzzy_alpha_epsilon)) {
          a_.OpAdd(alpha_test_fuzzy_diff_dest, alpha_src, -fuzzy_epsilon);
          a_.OpLT(alpha_test_op_dest, alpha_test_fuzzy_diff_src, alpha_test_reference_src);
          a_.OpOr(alpha_test_op_dest, alpha_test_op_src, dxbc::Src::LU(~static_cast<u32>(1 << 0)));
          a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
          a_.OpAdd(alpha_test_fuzzy_diff_dest, alpha_src, -alpha_test_reference_src);
          a_.OpLT(alpha_test_op_dest, alpha_test_fuzzy_diff_src.Abs(), fuzzy_epsilon);
          a_.OpOr(alpha_test_op_dest, alpha_test_op_src, dxbc::Src::LU(~static_cast<u32>(1 << 1)));
          a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
          a_.OpAdd(alpha_test_fuzzy_diff_dest, alpha_src, fuzzy_epsilon);
          a_.OpLT(alpha_test_op_dest, alpha_test_reference_src, alpha_test_fuzzy_diff_src);
          a_.OpOr(alpha_test_op_dest, alpha_test_op_src, dxbc::Src::LU(~static_cast<u32>(1 << 2)));
          a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
        } else {
          a_.OpLT(alpha_test_op_dest, alpha_src, alpha_test_reference_src);
          a_.OpOr(alpha_test_op_dest, alpha_test_op_src, dxbc::Src::LU(~static_cast<u32>(1 << 0)));
          a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
          a_.OpEq(alpha_test_op_dest, alpha_src, alpha_test_reference_src);
          a_.OpOr(alpha_test_op_dest, alpha_test_op_src, dxbc::Src::LU(~static_cast<u32>(1 << 1)));
          a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
          a_.OpLT(alpha_test_op_dest, alpha_test_reference_src, alpha_src);
          a_.OpOr(alpha_test_op_dest, alpha_test_op_src, dxbc::Src::LU(~static_cast<u32>(1 << 2)));
          a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
        }
      }
      a_.OpEndIf();
      if (edram_rov_used_) {
        a_.OpRetC(false, alpha_test_mask_src);
      } else {
        a_.OpDiscard(false, alpha_test_mask_src);
      }
    }
    a_.OpEndIf();
    PopSystemTemp();

    CompletePixelShader_AlphaToMask();

    if (edram_rov_used_) {
      a_.OpEndIf();
    }
  }

  if (edram_rov_used_) {
    CompletePixelShader_WriteToROV();
  } else {
    CompletePixelShader_WriteToRTVs();
    CompletePixelShader_DSV_DepthTo24Bit();
  }
}

void DxbcShaderTranslator::PreClampedFloat32To7e3(dxbc::Assembler& a, u32 f10_temp,
                                                  u32 f10_temp_component, u32 f32_temp,
                                                  u32 f32_temp_component, u32 temp_temp,
                                                  u32 temp_temp_component) {
  REX_ASSERT(temp_temp != f10_temp || temp_temp_component != f10_temp_component);
  REX_ASSERT(temp_temp != f32_temp || temp_temp_component != f32_temp_component);
  // Source and destination may be the same.
  dxbc::Dest f10_dest(dxbc::Dest::R(f10_temp, 1 << f10_temp_component));
  dxbc::Src f10_src(dxbc::Src::R(f10_temp).Select(f10_temp_component));
  dxbc::Src f32_src(dxbc::Src::R(f32_temp).Select(f32_temp_component));
  dxbc::Dest temp_dest(dxbc::Dest::R(temp_temp, 1 << temp_temp_component));
  dxbc::Src temp_src(dxbc::Src::R(temp_temp).Select(temp_temp_component));

  // The color must be clamped to [0, 31.875].

  a.OpULT(temp_dest, f32_src, dxbc::Src::LU(0x3E800000));
  a.OpIf(true, temp_src);
  {
    a.OpUShR(temp_dest, f32_src, dxbc::Src::LU(23));
    a.OpIAdd(temp_dest, dxbc::Src::LI(125), -temp_src);
    // DXBC shifts use the lower 5 bits of the amount, so clamp it to avoid overflow.
    a.OpUMin(temp_dest, temp_src, dxbc::Src::LU(24));
    a.OpBFI(f10_dest, dxbc::Src::LU(9), dxbc::Src::LU(23), dxbc::Src::LU(1), f32_src);
    a.OpUShR(f10_dest, f10_src, temp_src);
  }
  a.OpElse();
  { a.OpIAdd(f10_dest, f32_src, dxbc::Src::LU(0xC2000000u)); }
  a.OpEndIf();
  a.OpUBFE(temp_dest, dxbc::Src::LU(1), dxbc::Src::LU(16), f10_src);
  a.OpIAdd(f10_dest, f10_src, dxbc::Src::LU(0x7FFF));
  a.OpIAdd(f10_dest, f10_src, temp_src);
  a.OpUBFE(f10_dest, dxbc::Src::LU(10), dxbc::Src::LU(16), f10_src);
}

void DxbcShaderTranslator::UnclampedFloat32To7e3(dxbc::Assembler& a, u32 f10_temp,
                                                 u32 f10_temp_component, u32 f32_temp,
                                                 u32 f32_temp_component, u32 temp_temp,
                                                 u32 temp_temp_component) {
  // Source and destination may be the same, clamp to the destination and use it as the source.
  a.OpMax(dxbc::Dest::R(f10_temp, 1 << f10_temp_component),
          dxbc::Src::R(f32_temp).Select(f32_temp_component), dxbc::Src::LF(0.0f));
  a.OpMin(dxbc::Dest::R(f10_temp, 1 << f10_temp_component),
          dxbc::Src::R(f10_temp).Select(f10_temp_component), dxbc::Src::LF(31.875f));
  PreClampedFloat32To7e3(a, f10_temp, f10_temp_component, f10_temp, f10_temp_component, temp_temp,
                         temp_temp_component);
}

void DxbcShaderTranslator::Float7e3To32(dxbc::Assembler& a, const dxbc::Dest& f32, u32 f10_temp,
                                        u32 f10_temp_component, u32 f10_shift, u32 temp1_temp,
                                        u32 temp1_temp_component, u32 temp2_temp,
                                        u32 temp2_temp_component) {
  REX_ASSERT(f10_shift <= (32 - 10));
  REX_ASSERT(temp1_temp != temp2_temp || temp1_temp_component != temp2_temp_component);
  // Source may be the same as temp1 or temp2.
  dxbc::Dest exponent_dest(dxbc::Dest::R(temp1_temp, 1 << temp1_temp_component));
  dxbc::Src exponent_src(dxbc::Src::R(temp1_temp).Select(temp1_temp_component));
  dxbc::Dest mantissa_dest(dxbc::Dest::R(temp2_temp, 1 << temp2_temp_component));
  dxbc::Src mantissa_src(dxbc::Src::R(temp2_temp).Select(temp2_temp_component));

  if (!(f10_temp == temp1_temp && f10_temp_component == temp1_temp_component)) {
    // Unpack the exponent and the mantissa in the order that does not overwrite the source.
    a.OpUBFE(exponent_dest, dxbc::Src::LU(3), dxbc::Src::LU(f10_shift + 7),
             dxbc::Src::R(f10_temp).Select(f10_temp_component));
  }
  a.OpUBFE(mantissa_dest, dxbc::Src::LU(7), dxbc::Src::LU(f10_shift),
           dxbc::Src::R(f10_temp).Select(f10_temp_component));
  if (f10_temp == temp1_temp && f10_temp_component == temp1_temp_component) {
    a.OpUBFE(exponent_dest, dxbc::Src::LU(3), dxbc::Src::LU(f10_shift + 7),
             dxbc::Src::R(f10_temp).Select(f10_temp_component));
  }
  a.OpIf(false, exponent_src);
  {
    a.OpIf(true, mantissa_src);
    {
      // firstbithigh(x) is 31 - firstbit_hi(x) for non zero x and -1 for zero in DXBC.
      a.OpFirstBitHi(exponent_dest, mantissa_src);
      a.OpIAdd(exponent_dest, exponent_src, dxbc::Src::LI(7 - 31));
      a.OpIShL(mantissa_dest, mantissa_src, exponent_src);
      a.OpIAdd(exponent_dest, dxbc::Src::LI(1), -exponent_src);
    }
    a.OpElse();
    { a.OpMov(exponent_dest, dxbc::Src::LI(-124)); }
    a.OpEndIf();
  }
  a.OpEndIf();
  a.OpIMAd(exponent_dest, exponent_src, dxbc::Src::LI(1 << 23), dxbc::Src::LI(124 << 23));
  a.OpBFI(f32, dxbc::Src::LU(7), dxbc::Src::LU(23 - 7), mantissa_src, exponent_src);
}

void DxbcShaderTranslator::PreClampedDepthTo20e4(
    dxbc::Assembler& a, u32 f24_temp, u32 f24_temp_component, u32 f32_temp, u32 f32_temp_component,
    u32 temp_temp, u32 temp_temp_component, bool round_to_nearest_even, bool remap_from_0_to_0_5) {
  REX_ASSERT(temp_temp != f24_temp || temp_temp_component != f24_temp_component);
  REX_ASSERT(temp_temp != f32_temp || temp_temp_component != f32_temp_component);
  // Source and destination may be the same.
  dxbc::Dest f24_dest(dxbc::Dest::R(f24_temp, 1 << f24_temp_component));
  dxbc::Src f24_src(dxbc::Src::R(f24_temp).Select(f24_temp_component));
  dxbc::Src f32_src(dxbc::Src::R(f32_temp).Select(f32_temp_component));
  dxbc::Dest temp_dest(dxbc::Dest::R(temp_temp, 1 << temp_temp_component));
  dxbc::Src temp_src(dxbc::Src::R(temp_temp).Select(temp_temp_component));

  // The depth must be clamped to [0, 2).

  u32 remap_bias = static_cast<u32>(remap_from_0_to_0_5);

  a.OpULT(temp_dest, f32_src, dxbc::Src::LU(0x38800000 - (remap_bias << 23)));
  a.OpIf(true, temp_src);
  {
    a.OpUShR(temp_dest, f32_src, dxbc::Src::LU(23));
    a.OpIAdd(temp_dest, dxbc::Src::LI(113 - static_cast<i32>(remap_bias)), -temp_src);
    // DXBC shifts use the lower 5 bits of the amount, so clamp it to avoid overflow.
    a.OpUMin(temp_dest, temp_src, dxbc::Src::LU(24));
    a.OpBFI(f24_dest, dxbc::Src::LU(9), dxbc::Src::LU(23), dxbc::Src::LU(1), f32_src);
    a.OpUShR(f24_dest, f24_src, temp_src);
  }
  a.OpElse();
  { a.OpIAdd(f24_dest, f32_src, dxbc::Src::LU(0xC8000000u + (remap_bias << 23))); }
  a.OpEndIf();
  if (round_to_nearest_even) {
    a.OpUBFE(temp_dest, dxbc::Src::LU(1), dxbc::Src::LU(3), f24_src);
    a.OpIAdd(f24_dest, f24_src, dxbc::Src::LU(3));
    a.OpIAdd(f24_dest, f24_src, temp_src);
  }
  a.OpUBFE(f24_dest, dxbc::Src::LU(24), dxbc::Src::LU(3), f24_src);
}

void DxbcShaderTranslator::Depth20e4To32(dxbc::Assembler& a, const dxbc::Dest& f32, u32 f24_temp,
                                         u32 f24_temp_component, u32 f24_shift, u32 temp1_temp,
                                         u32 temp1_temp_component, u32 temp2_temp,
                                         u32 temp2_temp_component, bool remap_to_0_to_0_5) {
  REX_ASSERT(f24_shift <= (32 - 24));
  REX_ASSERT(temp1_temp != temp2_temp || temp1_temp_component != temp2_temp_component);
  // Source may be the same as temp1 or temp2.
  dxbc::Dest exponent_dest(dxbc::Dest::R(temp1_temp, 1 << temp1_temp_component));
  dxbc::Src exponent_src(dxbc::Src::R(temp1_temp).Select(temp1_temp_component));
  dxbc::Dest mantissa_dest(dxbc::Dest::R(temp2_temp, 1 << temp2_temp_component));
  dxbc::Src mantissa_src(dxbc::Src::R(temp2_temp).Select(temp2_temp_component));

  u32 remap_bias = static_cast<u32>(remap_to_0_to_0_5);

  if (!(f24_temp == temp1_temp && f24_temp_component == temp1_temp_component)) {
    // Unpack the exponent and the mantissa in the order that does not overwrite the source.
    a.OpUBFE(exponent_dest, dxbc::Src::LU(4), dxbc::Src::LU(f24_shift + 20),
             dxbc::Src::R(f24_temp).Select(f24_temp_component));
  }
  a.OpUBFE(mantissa_dest, dxbc::Src::LU(20), dxbc::Src::LU(f24_shift),
           dxbc::Src::R(f24_temp).Select(f24_temp_component));
  if (f24_temp == temp1_temp && f24_temp_component == temp1_temp_component) {
    a.OpUBFE(exponent_dest, dxbc::Src::LU(4), dxbc::Src::LU(f24_shift + 20),
             dxbc::Src::R(f24_temp).Select(f24_temp_component));
  }
  a.OpIf(false, exponent_src);
  {
    a.OpIf(true, mantissa_src);
    {
      // firstbithigh(x) is 31 - firstbit_hi(x) for non zero x and -1 for zero in DXBC.
      a.OpFirstBitHi(exponent_dest, mantissa_src);
      a.OpIAdd(exponent_dest, exponent_src, dxbc::Src::LI(20 - 31));
      a.OpIShL(mantissa_dest, mantissa_src, exponent_src);
      a.OpIAdd(exponent_dest, dxbc::Src::LI(1), -exponent_src);
    }
    a.OpElse();
    { a.OpMov(exponent_dest, dxbc::Src::LI(-static_cast<i32>(112 - remap_bias))); }
    a.OpEndIf();
  }
  a.OpEndIf();
  a.OpIMAd(exponent_dest, exponent_src, dxbc::Src::LI(1 << 23),
           dxbc::Src::LI(static_cast<i32>((112 - remap_bias) << 23)));
  a.OpBFI(f32, dxbc::Src::LU(20), dxbc::Src::LU(23 - 20), mantissa_src, exponent_src);
}

void DxbcShaderTranslator::ROV_DepthTo24Bit(u32 d24_temp, u32 d24_temp_component, u32 d32_temp,
                                            u32 d32_temp_component, u32 temp_temp,
                                            u32 temp_temp_component) {
  REX_ASSERT(temp_temp != d32_temp || temp_temp_component != d32_temp_component);
  // Source and destination may be the same.

  a_.OpAnd(dxbc::Dest::R(temp_temp, 1 << temp_temp_component), LoadFlagsSystemConstant(),
           dxbc::Src::LU(kSysFlag_DepthFloat24));
  a_.OpIf(true, dxbc::Src::R(temp_temp).Select(temp_temp_component));
  {
    PreClampedDepthTo20e4(a_, d24_temp, d24_temp_component, d32_temp, d32_temp_component, temp_temp,
                          temp_temp_component, true, false);
  }
  a_.OpElse();
  {
    dxbc::Dest d24_dest(dxbc::Dest::R(d24_temp, 1 << d24_temp_component));
    dxbc::Src d24_src(dxbc::Src::R(d24_temp).Select(d24_temp_component));
    a_.OpMul(d24_dest, dxbc::Src::R(d32_temp).Select(d32_temp_component),
             dxbc::Src::LF(static_cast<f32>(0xFFFFFF)));
    // Round to the nearest even integer, truncation gives 0xFF instead of 0x100 for clears.
    a_.OpRoundNE(d24_dest, d24_src);
    a_.OpFToU(d24_dest, d24_src);
  }
  a_.OpEndIf();
}

}  // namespace rex::gpu
