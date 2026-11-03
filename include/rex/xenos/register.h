/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xenos/register.h
 * @brief       Register layouts and the register file
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2014).
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <bit>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <optional>

#include <rex/platform.h>
#include <rex/assert.h>
#include <rex/types.h>
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>
#include <rex/xenos/fetch.h>
#include <rex/title.h>

namespace rex::xenos {

enum class Register : u16 {
/**
 * Defines an enumerator of Register, once per register in the register table.
 *
 * @param index Dword register index.
 * @param type Value type, unused here.
 * @param name Enumerator name.
 */
#define REXGPU_REGISTER(index, type, name) name = (index),
#include <rex/xenos/register_table.inc>
#undef REXGPU_REGISTER
};

/**
 * Register layouts. Every bit, including unused ones, is a named field so register values compare
 * stably. Bit fields use only 32 bit types because MSVC restarts packing when the alignment
 * changes.
 */
namespace reg {

/**
 * Host cache coherency request. A write sets status, which clears once the COHER_BASE_HOST and
 * COHER_SIZE_HOST range is coherent.
 */
union alignas(u32) COHER_STATUS_HOST {
  u32 value;
  struct {
    u32 matching_contexts : 8;
    u32 rb_copy_dest_base_ena : 1;
    u32 dest_base_0_ena : 1;
    u32 dest_base_1_ena : 1;
    u32 dest_base_2_ena : 1;
    u32 dest_base_3_ena : 1;
    u32 dest_base_4_ena : 1;
    u32 dest_base_5_ena : 1;
    u32 dest_base_6_ena : 1;
    u32 dest_base_7_ena : 1;
    u32 pad_17 : 7;
    u32 vc_action_ena : 1;
    u32 tc_action_ena : 1;
    u32 pglb_action_ena : 1;
    u32 pad_27 : 4;
    u32 status : 1;
  };
  static constexpr Register kRegisterIndex = xenos::Register::COHER_STATUS_HOST;
};
REX_STATIC_ASSERT_SIZE(COHER_STATUS_HOST, sizeof(u32));

/** Events and idle states the command processor waits for before it continues. */
union alignas(u32) WAIT_UNTIL {
  u32 value;
  struct {
    u32 pad_0 : 1;
    u32 wait_re_vsync : 1;
    u32 wait_fe_vsync : 1;
    u32 wait_vsync : 1;
    u32 wait_dsply_id0 : 1;
    u32 wait_dsply_id1 : 1;
    u32 wait_dsply_id2 : 1;
    u32 pad_7 : 3;
    u32 wait_cmdfifo : 1;
    u32 pad_11 : 3;
    u32 wait_2d_idle : 1;
    u32 wait_3d_idle : 1;
    u32 wait_2d_idleclean : 1;
    u32 wait_3d_idleclean : 1;
    u32 pad_18 : 2;
    u32 cmdfifo_entries : 4;
    u32 pad_24 : 8;
  };
  static constexpr Register kRegisterIndex = xenos::Register::WAIT_UNTIL;
};
REX_STATIC_ASSERT_SIZE(WAIT_UNTIL, sizeof(u32));

/** Shader register counts, export counts and export modes. */
union alignas(u32) SQ_PROGRAM_CNTL {
  u32 value;
  struct {
    u32 vs_num_reg : 6; /**< Vertex shader GPR count minus 1. */
    u32 pad_6 : 2;
    u32 ps_num_reg : 6; /**< Pixel shader GPR count minus 1. */
    u32 pad_14 : 2;
    u32 vs_resource : 1;
    u32 ps_resource : 1;
    /** Enables pixel parameter generation at SQ_CONTEXT_MISC::param_gen_pos. */
    u32 param_gen : 1;
    u32 gen_index_pix : 1;
    u32 vs_export_count : 4; /**< Interpolator output count minus 1. */
    xenos::VertexShaderExportMode vs_export_mode : 3;
    u32 ps_export_mode : 4;
    u32 gen_index_vtx : 1;
  };
  static constexpr Register kRegisterIndex = xenos::Register::SQ_PROGRAM_CNTL;
};
REX_STATIC_ASSERT_SIZE(SQ_PROGRAM_CNTL, sizeof(u32));

/** Shader context settings, including sample control and the pixel parameter interpolator. */
union alignas(u32) SQ_CONTEXT_MISC {
  u32 value;
  struct {
    u32 inst_pred_optimize : 1;
    u32 sc_output_screen_xy : 1;
    xenos::SampleControl sc_sample_cntl : 2;
    u32 pad_4 : 4;
    /**
     * Pixel shader interpolator index that receives the pixel parameters, below 16.
     * - XY is the screen position as floats, 0, 1, 2 rather than 0.5, 1.5, 2.5 in Direct3D 9 pixel
     *   center mode.
     * - ZW is the UV within a point sprite in [0, 1], 0 at the top and 1 at the bottom.
     * - The sign bit of X is set for back faces, of Y for a point and of Z for a line.
     */
    u32 param_gen_pos : 8;
    u32 perfcounter_ref : 1;
    u32 yield_optimize : 1;
    u32 tx_cache_sel : 1;
    u32 pad_19 : 13;
  };
  static constexpr Register kRegisterIndex = xenos::Register::SQ_CONTEXT_MISC;
};
REX_STATIC_ASSERT_SIZE(SQ_CONTEXT_MISC, sizeof(u32));

/** Per interpolator shading and sampling pattern bits. */
union alignas(u32) SQ_INTERPOLATOR_CNTL {
  u32 value;
  struct {
    u32 param_shade : 16;
    /** SampleLocation bits, used when SQ_CONTEXT_MISC::sc_sample_cntl is kCentroidsAndCenters. */
    u32 sampling_pattern : 16;
  };
  static constexpr Register kRegisterIndex = xenos::Register::SQ_INTERPOLATOR_CNTL;
};
REX_STATIC_ASSERT_SIZE(SQ_INTERPOLATOR_CNTL, sizeof(u32));

/** Range of float constants used by the vertex shader. */
union alignas(u32) SQ_VS_CONST {
  u32 value;
  struct {
    u32 base : 9; /**< Index of the first vec4 float constant. */
    u32 pad_9 : 3;
    u32 size : 9; /**< Number of vec4 float constants minus 1. */
    u32 pad_21 : 11;
  };
  static constexpr Register kRegisterIndex = xenos::Register::SQ_VS_CONST;
};
REX_STATIC_ASSERT_SIZE(SQ_VS_CONST, sizeof(u32));

/** Same layout as SQ_VS_CONST. */
union alignas(u32) SQ_PS_CONST {
  u32 value;
  struct {
    u32 base : 9; /**< Index of the first vec4 float constant. */
    u32 pad_9 : 3;
    u32 size : 9; /**< Number of vec4 float constants minus 1. */
    u32 pad_21 : 11;
  };
  static constexpr Register kRegisterIndex = xenos::Register::SQ_PS_CONST;
};
REX_STATIC_ASSERT_SIZE(SQ_PS_CONST, sizeof(u32));

/** Size of the index buffer fetched by DMA and its endian swap mode. */
union alignas(u32) VGT_DMA_SIZE {
  u32 value;
  struct {
    u32 num_words : 24;
    u32 pad_24 : 6;
    xenos::Endian swap_mode : 2;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_DMA_SIZE;
};

/** Draw parameters with the primitive type, index source, index size and index count. */
union alignas(u32) VGT_DRAW_INITIATOR {
  u32 value;
  struct {
    xenos::PrimitiveType prim_type : 6;
    xenos::SourceSelect source_select : 2;
    /** Titles use explicit mode for tessellated draws, which are ignored in implicit mode. */
    xenos::MajorMode major_mode : 2;
    u32 pad_10 : 1;
    xenos::IndexFormat index_size : 1;
    u32 not_eop : 1;
    u32 pad_13 : 3;
    u32 num_indices : 16;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_DRAW_INITIATOR;
};
REX_STATIC_ASSERT_SIZE(VGT_DRAW_INITIATOR, sizeof(u32));

/** Primitive reset index. Indices are unsigned and the GPU uses only their lower 24 bits. */
union alignas(u32) VGT_MULTI_PRIM_IB_RESET_INDX {
  u32 value;
  struct {
    u32 reset_indx : 24;
    u32 pad_24 : 8;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_MULTI_PRIM_IB_RESET_INDX;
};
REX_STATIC_ASSERT_SIZE(VGT_MULTI_PRIM_IB_RESET_INDX, sizeof(u32));

union alignas(u32) VGT_INDX_OFFSET {
  u32 value;
  struct {
    u32 indx_offset : 24; /**< Index math wraps at 24 bits, so signedness is irrelevant. */
    u32 pad_24 : 8;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_INDX_OFFSET;
};
REX_STATIC_ASSERT_SIZE(VGT_INDX_OFFSET, sizeof(u32));

/** Lower bound that vertex indices are clamped to. */
union alignas(u32) VGT_MIN_VTX_INDX {
  u32 value;
  struct {
    u32 min_indx : 24;
    u32 pad_24 : 8;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_MIN_VTX_INDX;
};
REX_STATIC_ASSERT_SIZE(VGT_MIN_VTX_INDX, sizeof(u32));

/** Upper bound that vertex indices are clamped to. */
union alignas(u32) VGT_MAX_VTX_INDX {
  u32 value;
  struct {
    u32 max_indx : 24;
    u32 pad_24 : 8;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_MAX_VTX_INDX;
};
REX_STATIC_ASSERT_SIZE(VGT_MAX_VTX_INDX, sizeof(u32));

/** Selects the vertex output path, with or without tessellation. */
union alignas(u32) VGT_OUTPUT_PATH_CNTL {
  u32 value;
  struct {
    xenos::VGTOutputPath path_select : 2;
    u32 pad_2 : 30;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_OUTPUT_PATH_CNTL;
};
REX_STATIC_ASSERT_SIZE(VGT_OUTPUT_PATH_CNTL, sizeof(u32));

union alignas(u32) VGT_HOS_CNTL {
  u32 value;
  struct {
    xenos::TessellationMode tess_mode : 2;
    u32 pad_2 : 30;
  };
  static constexpr Register kRegisterIndex = xenos::Register::VGT_HOS_CNTL;
};
REX_STATIC_ASSERT_SIZE(VGT_HOS_CNTL, sizeof(u32));

/** Minimum and maximum point sprite radius. */
union alignas(u32) PA_SU_POINT_MINMAX {
  u32 value;
  struct {
    u32 min_size : 16; /**< Lower bound of the point radius (half the size) in 12.4 fixed point. */
    u32 max_size : 16; /**< Upper bound of the point radius in 12.4 fixed point. */
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_POINT_MINMAX;
};
REX_STATIC_ASSERT_SIZE(PA_SU_POINT_MINMAX, sizeof(u32));

/** Default point sprite half height and half width. */
union alignas(u32) PA_SU_POINT_SIZE {
  u32 value;
  struct {
    u32 height : 16; /**< Half height in 12.4 fixed point. */
    u32 width : 16;  /**< Half width in 12.4 fixed point. */
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_POINT_SIZE;
};
REX_STATIC_ASSERT_SIZE(PA_SU_POINT_SIZE, sizeof(u32));

/** Setup unit and scanline converter mode. */
union alignas(u32) PA_SU_SC_MODE_CNTL {
  u32 value;
  struct {
    u32 cull_front : 1;
    u32 cull_back : 1;
    u32 face : 1; /**< Front face winding, 0 is counter clockwise and 1 is clockwise. */
    /** Titles may use 2 for triangles. The polygon types are ignored outside kDualMode. */
    xenos::PolygonModeEnable poly_mode : 2;
    xenos::PolygonType polymode_front_ptype : 3;
    xenos::PolygonType polymode_back_ptype : 3;
    /** Applies the front polygon offset registers to polygonal primitives. */
    u32 poly_offset_front_enable : 1;
    /** Applies the back polygon offset registers to polygonal primitives. */
    u32 poly_offset_back_enable : 1;
    /** Applies the front polygon offset registers to non polygonal primitives. */
    u32 poly_offset_para_enable : 1;
    u32 pad_14 : 1;
    u32 msaa_enable : 1;
    /** Adds PA_SC_WINDOW_OFFSET to vertex positions. */
    u32 vtx_window_offset_enable : 1;
    u32 pad_17 : 2;
    u32 provoking_vtx_last : 1;
    u32 persp_corr_dis : 1;
    u32 multi_prim_ib_ena : 1; /**< Enables the primitive reset index. */
    u32 pad_22 : 1;
    u32 quad_order_enable : 1;
    u32 sc_one_quad_per_clock : 1;
    u32 pad_25 : 7;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_SC_MODE_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_SU_SC_MODE_CNTL, sizeof(u32));

union alignas(u32) PA_SU_VTX_CNTL {
  u32 value;
  struct {
    xenos::PixelCenter pix_center : 1; /**< kD3DZero shifts positions by half a pixel. */
    xenos::VertexRounding round_mode : 2;
    xenos::VertexQuantization quant_mode : 3;
    u32 pad_6 : 26;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_VTX_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_SU_VTX_CNTL, sizeof(u32));

/** Multipass pixel shader control with the pixel vector count per pass and an enable bit. */
union alignas(u32) PA_SC_MPASS_PS_CNTL {
  u32 value;
  struct {
    u32 mpass_pix_vec_per_pass : 20;
    u32 pad_20 : 11;
    u32 mpass_ps_ena : 1;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_MPASS_PS_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_SC_MPASS_PS_CNTL, sizeof(u32));

/** Scanline converter visibility query, used by D3D for GPU side conditional rendering. */
union alignas(u32) PA_SC_VIZ_QUERY {
  u32 value;
  struct {
    u32 viz_query_ena : 1;
    u32 viz_query_id : 6;
    u32 kill_pix_post_hi_z : 1;        /**< Skips draws while viz_query_ena is set. */
    u32 kill_pix_post_detail_mask : 1; /**< Unused by Direct3D. */
    u32 pad_9 : 23;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_VIZ_QUERY;
};
REX_STATIC_ASSERT_SIZE(PA_SC_VIZ_QUERY, sizeof(u32));

union alignas(u32) PA_CL_CLIP_CNTL {
  u32 value;
  struct {
    u32 ucp_ena_0 : 1;
    u32 ucp_ena_1 : 1;
    u32 ucp_ena_2 : 1;
    u32 ucp_ena_3 : 1;
    u32 ucp_ena_4 : 1;
    u32 ucp_ena_5 : 1;
    u32 pad_6 : 8;
    u32 ps_ucp_mode : 2;
    u32 clip_disable : 1;      /**< Disables the user clip planes and depth clip. */
    u32 ucp_cull_only_ena : 1; /**< User clip planes cull primitives instead of clipping them. */
    u32 boundary_edge_flag_ena : 1;
    u32 dx_clip_space_def : 1; /**< Selects the Direct3D clip space Z range. */
    u32 dis_clip_err_detect : 1;
    u32 vtx_kill_or : 1; /**< Applies with the kill vertex export of the vertex shader. */
    u32 xy_nan_retain : 1;
    u32 z_nan_retain : 1;
    u32 w_nan_retain : 1;
    u32 pad_25 : 7;
  };
  struct {
    u32 ucp_ena : 6; /**< Mask of ucp_ena_0 to ucp_ena_5. */
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_CL_CLIP_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_CL_CLIP_CNTL, sizeof(u32));

union alignas(u32) PA_CL_VTE_CNTL {
  u32 value;
  struct {
    u32 vport_x_scale_ena : 1;  /**< Applies PA_CL_VPORT_XSCALE, otherwise the scale is 1. */
    u32 vport_x_offset_ena : 1; /**< Applies PA_CL_VPORT_XOFFSET, otherwise the offset is 0. */
    u32 vport_y_scale_ena : 1;  /**< Applies PA_CL_VPORT_YSCALE, otherwise the scale is 1. */
    u32 vport_y_offset_ena : 1; /**< Applies PA_CL_VPORT_YOFFSET, otherwise the offset is 0. */
    u32 vport_z_scale_ena : 1;  /**< Applies PA_CL_VPORT_ZSCALE, otherwise the scale is 1. */
    u32 vport_z_offset_ena : 1; /**< Applies PA_CL_VPORT_ZOFFSET, otherwise the offset is 0. */
    u32 pad_6 : 2;
    u32 vtx_xy_fmt : 1; /**< Set when vertex X and Y are divided by W. */
    u32 vtx_z_fmt : 1;  /**< Set when vertex Z is divided by W. */
    u32 vtx_w0_fmt : 1; /**< Set when W0 is not the reciprocal of W. */
    u32 perfcounter_ref : 1;
    u32 pad_12 : 20;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_CL_VTE_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_CL_VTE_CNTL, sizeof(u32));

/** Top left corner of the screen scissor. */
union alignas(u32) PA_SC_SCREEN_SCISSOR_TL {
  u32 value;
  struct {
    i32 tl_x : 15; /**< Left edge in pixels. */
    u32 pad_15 : 1;
    i32 tl_y : 15; /**< Top edge in pixels. */
    u32 pad_31 : 1;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_SCREEN_SCISSOR_TL;
};
REX_STATIC_ASSERT_SIZE(PA_SC_SCREEN_SCISSOR_TL, sizeof(u32));

/** Bottom right corner of the screen scissor. */
union alignas(u32) PA_SC_SCREEN_SCISSOR_BR {
  u32 value;
  struct {
    i32 br_x : 15; /**< Right edge in pixels. */
    u32 pad_15 : 1;
    i32 br_y : 15; /**< Bottom edge in pixels. */
    u32 pad_31 : 1;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_SCREEN_SCISSOR_BR;
};
REX_STATIC_ASSERT_SIZE(PA_SC_SCREEN_SCISSOR_BR, sizeof(u32));

/** Offset of the window origin within the screen. */
union alignas(u32) PA_SC_WINDOW_OFFSET {
  u32 value;
  struct {
    i32 window_x_offset : 15; /**< Added to vertex positions and the window scissor. */
    u32 pad_15 : 1;
    i32 window_y_offset : 15; /**< Added to vertex positions and the window scissor. */
    u32 pad_31 : 1;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_WINDOW_OFFSET;
};
REX_STATIC_ASSERT_SIZE(PA_SC_WINDOW_OFFSET, sizeof(u32));

/** Top left corner of the window scissor and a flag disabling the window offset. */
union alignas(u32) PA_SC_WINDOW_SCISSOR_TL {
  u32 value;
  struct {
    u32 tl_x : 14; /**< Left edge in pixels. */
    u32 pad_14 : 2;
    u32 tl_y : 14; /**< Top edge in pixels. */
    u32 pad_30 : 1;
    u32 window_offset_disable : 1; /**< Skips PA_SC_WINDOW_OFFSET for the window scissor. */
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_WINDOW_SCISSOR_TL;
};
REX_STATIC_ASSERT_SIZE(PA_SC_WINDOW_SCISSOR_TL, sizeof(u32));

/** Bottom right corner of the window scissor. */
union alignas(u32) PA_SC_WINDOW_SCISSOR_BR {
  u32 value;
  struct {
    u32 br_x : 14; /**< Right edge in pixels. */
    u32 pad_14 : 2;
    u32 br_y : 14; /**< Bottom edge in pixels. */
    u32 pad_30 : 2;
  };
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_WINDOW_SCISSOR_BR;
};
REX_STATIC_ASSERT_SIZE(PA_SC_WINDOW_SCISSOR_BR, sizeof(u32));

/** Selects the operation of the render backend, such as drawing or copying. */
union alignas(u32) RB_MODECONTROL {
  u32 value;
  struct {
    xenos::EDRAMMode edram_mode : 3;
    u32 pad_3 : 29;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_MODECONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_MODECONTROL, sizeof(u32));

/** Render target surface pitch, multisample count and hierarchical Z pitch. */
union alignas(u32) RB_SURFACE_INFO {
  u32 value;
  struct {
    u32 surface_pitch : 14; /**< Render target pitch in pixels. */
    u32 pad_14 : 2;
    xenos::MSAASamples msaa_samples : 2;
    u32 hiz_pitch : 14;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_SURFACE_INFO;
};
REX_STATIC_ASSERT_SIZE(RB_SURFACE_INFO, sizeof(u32));

/** Alpha test and alpha to mask settings. */
union alignas(u32) RB_COLORCONTROL {
  u32 value;
  struct {
    xenos::CompareFunction alpha_func : 3;
    u32 alpha_test_enable : 1;
    /** Passes bits 24 to 31 of the register to the shaders. */
    u32 alpha_to_mask_enable : 1;
    u32 pad_5 : 19;
    /**
     * Alpha to mask threshold offsets for the four pixels of each quad, lowering the coverage
     * thresholds by offset / 4 / sample count. Undithered alpha to coverage commonly uses 2, 2, 2,
     * 2 and dithered uses 3, 1, 0, 2. Offset 0 is for pixel (0, 0), 1 for (0, 1), 2 for (1, 0)
     * and 3 for (1, 1).
     */
    u32 alpha_to_mask_offset0 : 2;
    u32 alpha_to_mask_offset1 : 2;
    u32 alpha_to_mask_offset2 : 2;
    u32 alpha_to_mask_offset3 : 2;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_COLORCONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_COLORCONTROL, sizeof(u32));

/** RB_COLOR1_INFO through RB_COLOR3_INFO use this format too. */
union alignas(u32) RB_COLOR_INFO {
  u32 value;
  struct {
    /** Low 11 bits of the EDRAM base in tiles, which is periodic with 11 bits. */
    u32 color_base : 11;
    u32 color_base_bit_11 : 1;
    u32 pad_12 : 4;
    xenos::ColorRenderTargetFormat color_format : 4;
    i32 color_exp_bias : 6; /**< Signed exponent bias applied to written color. */
    u32 pad_26 : 6;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_COLOR_INFO;
  /** Dword indices of render targets 0 to 3 in order. */
  static const Register kRTRegisterIndices[4];
};
REX_STATIC_ASSERT_SIZE(RB_COLOR_INFO, sizeof(u32));

/** Per render target color channel write enables. */
union alignas(u32) RB_COLOR_MASK {
  u32 value;
  struct {
    /** Channel write enables, 4 bits per render target 0 to 3 in red, green, blue, alpha order. */
    u32 write_red0 : 1;
    u32 write_green0 : 1;
    u32 write_blue0 : 1;
    u32 write_alpha0 : 1;
    u32 write_red1 : 1;
    u32 write_green1 : 1;
    u32 write_blue1 : 1;
    u32 write_alpha1 : 1;
    u32 write_red2 : 1;
    u32 write_green2 : 1;
    u32 write_blue2 : 1;
    u32 write_alpha2 : 1;
    u32 write_red3 : 1;
    u32 write_green3 : 1;
    u32 write_blue3 : 1;
    u32 write_alpha3 : 1;
    u32 pad_16 : 16;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_COLOR_MASK;
};
REX_STATIC_ASSERT_SIZE(RB_COLOR_MASK, sizeof(u32));

/** RB_BLENDCONTROL0 through RB_BLENDCONTROL3 use this format. */
union alignas(u32) RB_BLENDCONTROL {
  u32 value;
  struct {
    xenos::BlendFactor color_srcblend : 5;
    xenos::BlendOp color_comb_fcn : 3;
    xenos::BlendFactor color_destblend : 5;
    u32 pad_13 : 3;
    xenos::BlendFactor alpha_srcblend : 5;
    xenos::BlendOp alpha_comb_fcn : 3;
    xenos::BlendFactor alpha_destblend : 5;
    u32 pad_29 : 3;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_BLENDCONTROL0;
  /** Dword indices of render targets 0 to 3 in order. */
  static const Register kRTRegisterIndices[4];
};
REX_STATIC_ASSERT_SIZE(RB_BLENDCONTROL, sizeof(u32));

/** Depth and stencil test settings for front and back faces. */
union alignas(u32) RB_DEPTHCONTROL {
  u32 value;
  struct {
    u32 stencil_enable : 1;
    u32 z_enable : 1;
    u32 z_write_enable : 1;
    u32 pad_3 : 1;
    xenos::CompareFunction zfunc : 3;
    u32 backface_enable : 1; /**< Uses the _bf stencil fields for back faces of polygons. */
    xenos::CompareFunction stencilfunc : 3;
    xenos::StencilOp stencilfail : 3;  /**< Front face operation when the stencil test fails. */
    xenos::StencilOp stencilzpass : 3; /**< Front face operation when both tests pass. */
    xenos::StencilOp stencilzfail : 3; /**< Front face operation when only the depth test fails. */
    xenos::CompareFunction stencilfunc_bf : 3;
    xenos::StencilOp stencilfail_bf : 3;
    xenos::StencilOp stencilzpass_bf : 3;
    xenos::StencilOp stencilzfail_bf : 3;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_DEPTHCONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_DEPTHCONTROL, sizeof(u32));

/** RB_STENCILREFMASK_BF uses this format too. */
union alignas(u32) RB_STENCILREFMASK {
  u32 value;
  struct {
    u32 stencilref : 8;
    u32 stencilmask : 8; /**< Mask applied to the reference and buffer values in the test. */
    u32 stencilwritemask : 8;
    u32 pad_24 : 8;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_STENCILREFMASK;
};
REX_STATIC_ASSERT_SIZE(RB_STENCILREFMASK, sizeof(u32));

/** Depth render target base in EDRAM and format. */
union alignas(u32) RB_DEPTH_INFO {
  u32 value;
  struct {
    /** Low 11 bits of the EDRAM base in tiles, which is periodic with 11 bits. */
    u32 depth_base : 11;
    u32 depth_base_bit_11 : 1;
    u32 pad_12 : 4;
    xenos::DepthRenderTargetFormat depth_format : 1;
    u32 pad_17 : 15;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_DEPTH_INFO;
};
REX_STATIC_ASSERT_SIZE(RB_DEPTH_INFO, sizeof(u32));

/** Source selection, sample selection, clear enables and command for an EDRAM copy. */
union alignas(u32) RB_COPY_CONTROL {
  u32 value;
  struct {
    u32 copy_src_select : 3; /**< 4 selects depth. */
    u32 pad_3 : 1;
    xenos::CopySampleSelect copy_sample_select : 3;
    u32 pad_7 : 1;
    u32 color_clear_enable : 1;
    u32 depth_clear_enable : 1;
    u32 pad_10 : 10;
    xenos::CopyCommand copy_command : 2;
    u32 pad_22 : 10;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_COPY_CONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_COPY_CONTROL, sizeof(u32));

/** Destination endianness, layout, format, number format and exponent bias of an EDRAM copy. */
union alignas(u32) RB_COPY_DEST_INFO {
  u32 value;
  struct {
    xenos::Endian128 copy_dest_endian : 3;
    u32 copy_dest_array : 1; /**< Array layout, which changes the tiled extent. */
    u32 copy_dest_layer : 3; /**< Z layer offset of 3D destinations. */
    xenos::ColorFormat copy_dest_format : 6;
    xenos::SurfaceNumberFormat copy_dest_number : 3;
    i32 copy_dest_exp_bias : 6; /**< Signed exponent bias applied to written color. */
    u32 pad_22 : 2;
    u32 copy_dest_swap : 1;
    u32 pad_25 : 7;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_COPY_DEST_INFO;
};
REX_STATIC_ASSERT_SIZE(RB_COPY_DEST_INFO, sizeof(u32));

/** Destination pitch and height of an EDRAM copy. */
union alignas(u32) RB_COPY_DEST_PITCH {
  u32 value;
  struct {
    u32 copy_dest_pitch : 14; /**< In pixels. */
    u32 pad_14 : 2;
    u32 copy_dest_height : 14; /**< In pixels. */
    u32 pad_30 : 2;
  };
  static constexpr Register kRegisterIndex = xenos::Register::RB_COPY_DEST_PITCH;
};
REX_STATIC_ASSERT_SIZE(RB_COPY_DEST_PITCH, sizeof(u32));

/** Table index for gamma ramp reads and writes. */
union alignas(u32) DC_LUT_RW_INDEX {
  u32 value;
  struct {
    u32 rw_index : 8; /**< Absolute index into the 256 entry table. Bit 7 is ignored for PWL. */
    u32 pad_8 : 24;
  };
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_RW_INDEX;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_RW_INDEX, sizeof(u32));

/** Sequential write of one color channel of the gamma ramp entry at DC_LUT_RW_INDEX. */
union alignas(u32) DC_LUT_SEQ_COLOR {
  u32 value;
  struct {
    u32 seq_color : 16; /**< The command processor keeps bits 6 to 15. */
    u32 pad_16 : 16;
  };
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_SEQ_COLOR;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_SEQ_COLOR, sizeof(u32));

/** Piecewise linear gamma ramp entry with a base and a delta. */
union alignas(u32) DC_LUT_PWL_DATA {
  u32 value;
  struct {
    /**
     * Output is base + (multiplier * delta) / 2^increment, where increment is the DC_LUTA_CONTROL
     * value of the channel, base is 7 bits of the front buffer value above the increment bits and
     * multiplier is its lower increment bits. Bits 0 to 5 of base and delta are hardwired to 0.
     */
    u32 base : 16;
    u32 delta : 16;
  };
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_PWL_DATA;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_PWL_DATA, sizeof(u32));

/** Gamma ramp entry holding three channels of 10 bits. */
union alignas(u32) DC_LUT_30_COLOR {
  u32 value;
  struct {
    u32 color_10_blue : 10;
    u32 color_10_green : 10;
    u32 color_10_red : 10;
    u32 pad_30 : 2;
  };
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_30_COLOR;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_30_COLOR, sizeof(u32));

}  // namespace reg

}  // namespace rex::xenos

namespace rex::xenos {

struct RegisterInfo {
  enum class Type : u8 {
    kDword,
    kFloat,
  };
  Type type;
  const char* name;
};

class RegisterFile {
 public:
  /**
   * Looks up a register in the register table.
   *
   * @param index Dword register index.
   * @return Name and type of the register, or null when the index is not in the table.
   */
  static const RegisterInfo* GetRegisterInfo(u32 index);

  /** Number of dword registers, 1 past the highest register index. */
  static constexpr size_t kRegisterCount = 0x5003;
  u32 values[kRegisterCount]{};

  /** Returns a register value by dword register index or by register. @{ */
  const u32& operator[](u32 reg) const { return values[reg]; }
  u32& operator[](u32 reg) { return values[reg]; }
  const u32& operator[](Register reg) const { return values[std::to_underlying(reg)]; }
  u32& operator[](Register reg) { return values[std::to_underlying(reg)]; }
  /** @} */

  /**
   * Reinterprets a register value as a register layout or other 32 bit type.
   *
   * @tparam T Type with the size of a register.
   * @param reg Dword register index.
   * @return The register bits as T.
   */
  template <typename T>
  T Get(u32 reg) const {
    return std::bit_cast<T>(values[reg]);
  }
  /**
   * Reinterprets a register value as a register layout or other 32 bit type.
   *
   * @tparam T Type with the size of a register.
   * @return The register bits as T.
   */
  template <typename T>
  T Get(Register reg) const {
    return Get<T>(std::to_underlying(reg));
  }
  /**
   * Reads the register that a register layout declares as its own.
   *
   * @tparam T Register layout declaring `kRegisterIndex`.
   * @return The register bits as T.
   */
  template <typename T>
  T Get() const {
    return Get<T>(T::kRegisterIndex);
  }

  /**
   * Reads a vertex fetch constant from the shader fetch constant registers.
   *
   * @param index Fetch constant slot, below 96.
   * @return Copy of the vertex fetch constant.
   */
  xenos::VertexFetch GetVertexFetch(u32 index) const {
    REX_ASSERT(index < 96);
    xenos::VertexFetch fetch;
    std::memcpy(&fetch,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) +
                        (sizeof(fetch) / sizeof(u32)) * index],
                sizeof(fetch));
    return fetch;
  }

  /**
   * Reads a texture fetch constant from the shader fetch constant registers.
   *
   * @param index Fetch constant slot, below 32.
   * @return Copy of the texture fetch constant.
   */
  xenos::TextureFetch GetTextureFetch(u32 index) const {
    REX_ASSERT(index < 32);
    xenos::TextureFetch fetch;
    std::memcpy(&fetch,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) +
                        (sizeof(fetch) / sizeof(u32)) * index],
                sizeof(fetch));
    return fetch;
  }

  /**
   * Reads a memory export stream constant from a float shader constant.
   *
   * @param float_constant_index Float constant index, below 512.
   * @return Copy of the memory export stream constant.
   */
  xenos::MemExportStream GetMemExportStream(u32 float_constant_index) const {
    REX_ASSERT(float_constant_index < 512);
    xenos::MemExportStream stream;
    std::memcpy(&stream,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X) +
                        4 * float_constant_index],
                sizeof(stream));
    return stream;
  }
};

/** Largest width or height the viewport size register reports. */
inline constexpr u32 kViewportLimit = 0x0FFF;

/**
 * Returns the value a title reads from a register that reports the display mode or a fixed value,
 * or empty when the register is not one of these.
 */
inline std::optional<u32> ReadFixedRegister(u32 index, const VideoMode& mode) {
  switch (index) {
    case static_cast<u32>(Register::RB_EDRAM_TIMING):
      return 0x08100748;
    case static_cast<u32>(Register::RB_BC_CONTROL):
      return 0x0000200E;
    case static_cast<u32>(Register::D1MODE_V_COUNTER):
      return std::min(mode.height, kViewportLimit);
    case static_cast<u32>(Register::D1MODE_VBLANK_VLINE_STATUS):
      return 1;
    case static_cast<u32>(Register::AVIVO_D1MODE_VIEWPORT_SIZE):
      return (std::min(mode.width, kViewportLimit) << 16) | std::min(mode.height, kViewportLimit);
    default:
      return std::nullopt;
  }
}

}  // namespace rex::xenos
