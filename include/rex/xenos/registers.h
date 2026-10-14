/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <utility>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/types.h>
#include <rex/xenos/xenos.h>

namespace rex::xenos {

/** Every GPU register, valued by its dword register index. */
enum class Register : u16 {
/**
 * Defines an enumerator of Register. The register table includes this once per register.
 *
 * @param index Dword register index.
 * @param type Value type of the register, unused here.
 * @param name Enumerator name.
 *
 * @code
 * REXGPU_REGISTER(0x05C8, kDword, WAIT_UNTIL)
 * @endcode
 */
#define REXGPU_REGISTER(index, type, name) name = (index),
#include <rex/xenos/register_table.inc>
#undef REXGPU_REGISTER
};

/**
 * Register layouts. Every bit, including unused ones, is a named field so register values compare
 * stably when constructed or modified. Bit fields use only 32 bit types (uint32_t, int32_t, float
 * or enums with a 32 bit underlying type) because Visual C++ restarts packing when a field needs
 * different alignment than the previous one.
 */
namespace reg {

/**
 * Host cache coherency request. The status bit is set by a write and cleared once the range in
 * COHER_BASE_HOST and COHER_SIZE_HOST has been made coherent.
 */
union alignas(u32) COHER_STATUS_HOST {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 matching_contexts : 8;     /**< Bits 0 to 7. No code reads it. */
    u32 rb_copy_dest_base_ena : 1; /**< No code reads it. */
    /**
     * @name Bits 9 to 16. Destination base enables dest_base_0_ena to dest_base_7_ena, 1 bit per
     * destination base register. No code reads them.
     */
    /** @{ */
    u32 dest_base_0_ena : 1;
    u32 dest_base_1_ena : 1;
    u32 dest_base_2_ena : 1;
    u32 dest_base_3_ena : 1;
    u32 dest_base_4_ena : 1;
    u32 dest_base_5_ena : 1;
    u32 dest_base_6_ena : 1;
    u32 dest_base_7_ena : 1;
    /** @} */
    u32 pad_17 : 7;
    /**
     * Requests a vertex cache action. The command processor only names it in the coherency
     * trace log.
     */
    u32 vc_action_ena : 1;
    /**
     * Requests a texture cache action. The command processor only names it in the coherency
     * trace log.
     */
    u32 tc_action_ena : 1;
    u32 pglb_action_ena : 1; /**< No code reads it. */
    u32 pad_27 : 4;
    /**
     * Set by a write to request coherency of the COHER_BASE_HOST range. The command
     * processor clears the register after logging the request.
     */
    u32 status : 1;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::COHER_STATUS_HOST;
};
REX_STATIC_ASSERT_SIZE(COHER_STATUS_HOST, sizeof(u32));

/** Events and idle states the command processor waits for before it continues. */
union alignas(u32) WAIT_UNTIL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 pad_0 : 1;
    u32 wait_re_vsync : 1;  /**< No code reads it. */
    u32 wait_fe_vsync : 1;  /**< No code reads it. */
    u32 wait_vsync : 1;     /**< No code reads it. */
    u32 wait_dsply_id0 : 1; /**< No code reads it. */
    u32 wait_dsply_id1 : 1; /**< No code reads it. */
    u32 wait_dsply_id2 : 1; /**< No code reads it. */
    u32 pad_7 : 3;
    u32 wait_cmdfifo : 1; /**< No code reads it. */
    u32 pad_11 : 3;
    u32 wait_2d_idle : 1;      /**< No code reads it. */
    u32 wait_3d_idle : 1;      /**< No code reads it. */
    u32 wait_2d_idleclean : 1; /**< No code reads it. */
    u32 wait_3d_idleclean : 1; /**< No code reads it. */
    u32 pad_18 : 2;
    u32 cmdfifo_entries : 4; /**< Bits 20 to 23. No code reads it. */
    u32 pad_24 : 8;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::WAIT_UNTIL;
};
REX_STATIC_ASSERT_SIZE(WAIT_UNTIL, sizeof(u32));

/**
 * Shader program setup with the register counts, export counts and export modes of the vertex and
 * pixel shaders.
 */
union alignas(u32) SQ_PROGRAM_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** GPR count. The field is 6 bits wide and bits 7:6 are never set. */
    u32 vs_num_reg : 6; /**< Bits 0 to 5. Vertex shader GPR count minus 1. */
    u32 pad_6 : 2;
    u32 ps_num_reg : 6; /**< Bits 8 to 13. Pixel shader GPR count minus 1. */
    u32 pad_14 : 2;
    u32 vs_resource : 1; /**< No code reads it. */
    u32 ps_resource : 1; /**< No code reads it. */
    /**
     * Enables pixel parameter generation, which the shader translator applies at
     * SQ_CONTEXT_MISC::param_gen_pos.
     */
    u32 param_gen : 1;
    u32 gen_index_pix : 1;   /**< No code reads it. */
    u32 vs_export_count : 4; /**< Bits 20 to 23. Interpolator output count minus 1. */
    /**
     * Bits 24 to 26. Vertex shader export mode. The draw code rejects kMultipass and the pipeline
     * caches assert the edge flag modes are unused.
     */
    xenos::VertexShaderExportMode vs_export_mode : 3;
    u32 ps_export_mode : 4; /**< Bits 27 to 30. No code reads it. */
    /** Vertex index generation. The pipeline caches assert it is clear. */
    u32 gen_index_vtx : 1;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::SQ_PROGRAM_CNTL;
};
REX_STATIC_ASSERT_SIZE(SQ_PROGRAM_CNTL, sizeof(u32));

/**
 * Shader context settings, including sample control and the interpolator that receives the pixel
 * parameters.
 */
union alignas(u32) SQ_CONTEXT_MISC {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 inst_pred_optimize : 1;  /**< No code reads it. */
    u32 sc_output_screen_xy : 1; /**< No code reads it. */
    /** Bits 2 to 3. Interpolation sample location mode, passed to the pipeline caches. */
    xenos::SampleControl sc_sample_cntl : 2;
    u32 pad_4 : 4;
    /**
     * Pixel shader interpolator index that receives the pixel parameters. It is limited to the
     * interpolator count of 16, not the total register count of 64.
     * - XY is the screen position (vPos) passed as floats. In Direct3D 9 pixel center mode it
     *   holds 0, 1, 2 rather than 0.5, 1.5, 2.5.
     * - ZW is the UV within a point sprite in [0, 1], 0 at the top and 1 at the bottom, matching
     *   gl_PointCoord. Titles receive the whole register through the SPRITETEXCOORD input semantic
     *   and take abs(SPRITETEXCOORD.zw).
     * - The sign bit of X is set for back faces and clear for front faces. Shaders use rcpc to
     *   account for the signedness of 0 in cndge.
     * - The sign bit of Y is set for a point.
     * - The sign bit of Z is set for a line.
     */
    u32 param_gen_pos : 8;
    u32 perfcounter_ref : 1; /**< No code reads it. */
    u32 yield_optimize : 1;  /**< No code reads it. */
    u32 tx_cache_sel : 1;    /**< No code reads it. */
    u32 pad_19 : 13;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::SQ_CONTEXT_MISC;
};
REX_STATIC_ASSERT_SIZE(SQ_CONTEXT_MISC, sizeof(u32));

/** Per interpolator shading and sampling pattern bits. */
union alignas(u32) SQ_INTERPOLATOR_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 param_shade : 16; /**< Bits 0 to 15. No code reads it. */
    /**
     * SampleLocation bits, 0 for centroid and 1 for center, when SQ_CONTEXT_MISC::sc_sample_cntl is
     * kCentroidsAndCenters.
     */
    u32 sampling_pattern : 16;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::SQ_INTERPOLATOR_CNTL;
};
REX_STATIC_ASSERT_SIZE(SQ_INTERPOLATOR_CNTL, sizeof(u32));

/** Range of float constants used by the vertex shader. */
union alignas(u32) SQ_VS_CONST {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 8. Index of the first vec4 float constant of the range. The Direct3D 12 command
     * processor asserts the register is 0x000FF000 or 0.
     */
    u32 base : 9;
    u32 pad_9 : 3;
    u32 size : 9; /**< Bits 12 to 20. Number of vec4 float constants in the range minus 1. */
    u32 pad_21 : 11;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::SQ_VS_CONST;
};
REX_STATIC_ASSERT_SIZE(SQ_VS_CONST, sizeof(u32));

/** Same layout as SQ_VS_CONST. */
union alignas(u32) SQ_PS_CONST {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 8. Index of the first vec4 float constant of the range. The Direct3D 12 command
     * processor asserts the register is 0x000FF100 or 0.
     */
    u32 base : 9;
    u32 pad_9 : 3;
    u32 size : 9; /**< Bits 12 to 20. Number of vec4 float constants in the range minus 1. */
    u32 pad_21 : 11;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::SQ_PS_CONST;
};
REX_STATIC_ASSERT_SIZE(SQ_PS_CONST, sizeof(u32));

/** Size of the index buffer fetched by DMA and its endian swap mode. */
union alignas(u32) VGT_DMA_SIZE {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 23. Number of 32 bit words of the index buffer fetched by DMA. No code reads it.
     */
    u32 num_words : 24;
    u32 pad_24 : 6;
    /**
     * Bits 30 to 31. Endian swap applied to the DMA index buffer, passed to the index buffer info
     * of the draw packet handler.
     */
    xenos::Endian swap_mode : 2;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_DMA_SIZE;
};

/** Draw parameters with the primitive type, index source, index size and index count. */
union alignas(u32) VGT_DRAW_INITIATOR {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    xenos::PrimitiveType prim_type : 6; /**< Bits 0 to 5. Primitive type, passed to IssueDraw. */
    /**
     * Bits 6 to 7. Index source, which the draw packet handler switches on to choose DMA or auto
     * generated indices.
     */
    xenos::SourceSelect source_select : 2;
    /**
     * Titles set the explicit mode mainly for tessellated draws, because VGT_OUTPUT_PATH_CNTL with
     * tessellation enabled is ignored in the implicit major mode.
     */
    xenos::MajorMode major_mode : 2;
    u32 pad_10 : 1;
    /** Index element format, 16 bit or 32 bit, which sets the index buffer element size. */
    xenos::IndexFormat index_size : 1;
    u32 not_eop : 1; /**< No code reads it. */
    u32 pad_13 : 3;
    /**
     * Bits 16 to 31. Number of indices to draw, passed to IssueDraw and used as the DMA index
     * count.
     */
    u32 num_indices : 16;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_DRAW_INITIATOR;
};
REX_STATIC_ASSERT_SIZE(VGT_DRAW_INITIATOR, sizeof(u32));

/**
 * Vertex index registers are 24 bit. Indices are unsigned and the GPU uses only their lower 24
 * bits. Operations apply in the order primitive reset index check, offsetting, clamping.
 */
union alignas(u32) VGT_MULTI_PRIM_IB_RESET_INDX {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * The upper 8 bits of the index buffer value are ignored. The primitive processor resets on
     * elements whose lower 24 bits equal this value, so 0xFFFFFF, 0x1FFFFFF and 0xFFFFFFFF all
     * reset when this is 0xFFFFFF.
     */
    u32 reset_indx : 24;
    u32 pad_24 : 8;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_MULTI_PRIM_IB_RESET_INDX;
};
REX_STATIC_ASSERT_SIZE(VGT_MULTI_PRIM_IB_RESET_INDX, sizeof(u32));

/** Offset added to vertex indices. */
union alignas(u32) VGT_INDX_OFFSET {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Signedness is irrelevant because index calculations use 24 bit integers, so
     * ((0xFFFFFE + 3) & 0xFFFFFF) == 1. Direct3D 9 writes BaseVertexIndex as a signed int32 to the
     * entire register and the upper 8 bits are ignored.
     */
    u32 indx_offset : 24;
    u32 pad_24 : 8;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_INDX_OFFSET;
};
REX_STATIC_ASSERT_SIZE(VGT_INDX_OFFSET, sizeof(u32));

/** Lower bound that vertex indices are clamped to. */
union alignas(u32) VGT_MIN_VTX_INDX {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 23. Lower clamp for vertex indices, read by the Direct3D 12 command processor and
     * the draw extent estimator.
     */
    u32 min_indx : 24;
    u32 pad_24 : 8;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_MIN_VTX_INDX;
};
REX_STATIC_ASSERT_SIZE(VGT_MIN_VTX_INDX, sizeof(u32));

/** Upper bound that vertex indices are clamped to. */
union alignas(u32) VGT_MAX_VTX_INDX {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 23. Upper clamp for vertex indices, passed to the shaders and read by the draw
     * extent estimator. */
    u32 max_indx : 24;
    u32 pad_24 : 8;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_MAX_VTX_INDX;
};
REX_STATIC_ASSERT_SIZE(VGT_MAX_VTX_INDX, sizeof(u32));

/** Selects the vertex output path, with or without tessellation. */
union alignas(u32) VGT_OUTPUT_PATH_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 1. Vertex output path, read by the primitive processing and logged by the draw
     * packet handler.
     */
    xenos::VGTOutputPath path_select : 2;
    u32 pad_2 : 30;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_OUTPUT_PATH_CNTL;
};
REX_STATIC_ASSERT_SIZE(VGT_OUTPUT_PATH_CNTL, sizeof(u32));

/** Tessellation mode. */
union alignas(u32) VGT_HOS_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 1. Tessellation mode, which the Direct3D 12 command processor compares with
     * kAdaptive.
     */
    xenos::TessellationMode tess_mode : 2;
    u32 pad_2 : 30;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::VGT_HOS_CNTL;
};
REX_STATIC_ASSERT_SIZE(VGT_HOS_CNTL, sizeof(u32));

/** Minimum and maximum point sprite radius. */
union alignas(u32) PA_SU_POINT_MINMAX {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 15. Lower bound of the point radius (half the size) in 12.4 fixed point. */
    u32 min_size : 16;
    /** Bits 16 to 31. Upper bound of the point radius (half the size) in 12.4 fixed point. */
    u32 max_size : 16;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_POINT_MINMAX;
};
REX_STATIC_ASSERT_SIZE(PA_SU_POINT_MINMAX, sizeof(u32));

/** Default point sprite half height and half width. */
union alignas(u32) PA_SU_POINT_SIZE {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 height : 16; /**< Bits 0 to 15. Default point half height in 12.4 fixed point. */
    u32 width : 16;  /**< Bits 16 to 31. Default point half width in 12.4 fixed point. */
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_POINT_SIZE;
};
REX_STATIC_ASSERT_SIZE(PA_SU_POINT_SIZE, sizeof(u32));

/** Setup unit and scanline converter mode control. */
union alignas(u32) PA_SU_SC_MODE_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Culls front faces. The pipeline caches assert it is not set together with cull_back.
     */
    u32 cull_front : 1;
    /**
     * Culls back faces, read by the pipeline caches and the Direct3D 12 command processor.
     */
    u32 cull_back : 1;
    /** Front face winding, where 0 is counter clockwise front and 1 is clockwise front. */
    u32 face : 1;
    /**
     * Titles may use poly_mode 2 for triangles, with polymode_front_ptype and polymode_back_ptype
     * 0 (points). Those types must be ignored outside
     * kDualMode because filled triangles are drawn.
     */
    xenos::PolygonModeEnable poly_mode : 2;
    /**
     * Bits 5 to 7. Type front faces are drawn as when poly_mode is kDualMode, read by the pipeline
     * caches.
     */
    xenos::PolygonType polymode_front_ptype : 3;
    /**
     * Bits 8 to 10. Type back faces are drawn as when poly_mode is kDualMode, read by the pipeline
     * caches.
     */
    xenos::PolygonType polymode_back_ptype : 3;
    /**
     * Applies the front polygon offset scale and offset registers to polygonal primitives.
     */
    u32 poly_offset_front_enable : 1;
    /**
     * Applies the back polygon offset scale and offset registers to polygonal primitives.
     */
    u32 poly_offset_back_enable : 1;
    /** Applies the front polygon offset registers to non polygonal primitives. */
    u32 poly_offset_para_enable : 1;
    u32 pad_14 : 1;
    u32 msaa_enable : 1; /**< No code reads it. */
    /**
     * Adds PA_SC_WINDOW_OFFSET to vertex positions, read by the draw code and the draw
     * extent estimator.
     */
    u32 vtx_window_offset_enable : 1;
    u32 pad_17 : 2;
    u32 provoking_vtx_last : 1; /**< No code reads it. */
    u32 persp_corr_dis : 1;     /**< No code reads it. */
    /**
     * Enables the primitive reset index, read by the primitive processor and the draw
     * extent estimator.
     */
    u32 multi_prim_ib_ena : 1;
    u32 pad_22 : 1;
    u32 quad_order_enable : 1;     /**< No code reads it. */
    u32 sc_one_quad_per_clock : 1; /**< No code reads it. */
    u32 pad_25 : 7;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_SC_MODE_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_SU_SC_MODE_CNTL, sizeof(u32));

/** Setup unit vertex control. */
union alignas(u32) PA_SU_VTX_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Pixel center convention. kD3DZero makes the draw code shift positions by half a pixel.
     */
    xenos::PixelCenter pix_center : 1;
    xenos::VertexRounding round_mode : 2;     /**< Bits 1 to 2. No code reads it. */
    xenos::VertexQuantization quant_mode : 3; /**< Bits 3 to 5. No code reads it. */
    u32 pad_6 : 26;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SU_VTX_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_SU_VTX_CNTL, sizeof(u32));

/** Multipass pixel shader control with the pixel vector count per pass and an enable bit. */
union alignas(u32) PA_SC_MPASS_PS_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 mpass_pix_vec_per_pass : 20; /**< Bits 0 to 19. No code reads it. */
    u32 pad_20 : 11;
    u32 mpass_ps_ena : 1; /**< No code reads it. */
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_MPASS_PS_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_SC_MPASS_PS_CNTL, sizeof(u32));

/** Scanline converter visibility query, used by D3D for GPU side conditional rendering. */
union alignas(u32) PA_SC_VIZ_QUERY {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Enables the visibility query. The draw packet handler skips the draw when
     * kill_pix_post_hi_z is also set.
     */
    u32 viz_query_ena : 1;
    u32 viz_query_id : 6; /**< Bits 1 to 6. No code reads it. */
    /** Discards geometry after the test but still uses it for testing. */
    u32 kill_pix_post_hi_z : 1;
    u32 kill_pix_post_detail_mask : 1; /**< Unused by Direct3D. */
    u32 pad_9 : 23;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_VIZ_QUERY;
};
REX_STATIC_ASSERT_SIZE(PA_SC_VIZ_QUERY, sizeof(u32));

/** Clipper clip control. */
union alignas(u32) PA_CL_CLIP_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * @name Bits 0 to 5. User clip plane enables ucp_ena_0 to ucp_ena_5, 1 bit per plane, read
     * through the ucp_ena mask.
     */
    /** @{ */
    u32 ucp_ena_0 : 1;
    u32 ucp_ena_1 : 1;
    u32 ucp_ena_2 : 1;
    u32 ucp_ena_3 : 1;
    u32 ucp_ena_4 : 1;
    u32 ucp_ena_5 : 1;
    /** @} */
    u32 pad_6 : 8;
    /**
     * Bits 14 to 15. User clip plane mode for point sprites, passed to the vertex shader
     * modification.
     */
    u32 ps_ucp_mode : 2;
    /** Disables clipping, which also disables the user clip planes and depth clip. */
    u32 clip_disable : 1;
    /**
     * Makes the user clip planes cull primitives instead of clipping them, read by the
     * pipeline caches.
     */
    u32 ucp_cull_only_ena : 1;
    u32 boundary_edge_flag_ena : 1; /**< No code reads it. */
    /**
     * Selects the Direct3D clip space Z range, which the viewport computation of the draw
     * code honors.
     */
    u32 dx_clip_space_def : 1;
    u32 dis_clip_err_detect : 1; /**< No code reads it. */
    /**
     * Read by the pipeline caches together with the kill vertex export of the vertex
     * shader.
     */
    u32 vtx_kill_or : 1;
    u32 xy_nan_retain : 1; /**< No code reads it. */
    u32 z_nan_retain : 1;  /**< No code reads it. */
    u32 w_nan_retain : 1;  /**< No code reads it. */
    u32 pad_25 : 7;
  };
  struct {
    /**
     * Bits 0 to 5. Mask of ucp_ena_0 to ucp_ena_5, read by the command processors and the pipeline
     * caches.
     */
    u32 ucp_ena : 6;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_CL_CLIP_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_CL_CLIP_CNTL, sizeof(u32));

/** Viewport transform engine control. */
union alignas(u32) PA_CL_VTE_CNTL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Applies PA_CL_VPORT_XSCALE, otherwise the draw code uses a scale of 1. */
    u32 vport_x_scale_ena : 1;
    /** Applies PA_CL_VPORT_XOFFSET, otherwise the draw code uses an offset of 0. */
    u32 vport_x_offset_ena : 1;
    /** Applies PA_CL_VPORT_YSCALE, otherwise the draw code uses a scale of 1. */
    u32 vport_y_scale_ena : 1;
    /** Applies PA_CL_VPORT_YOFFSET, otherwise the draw code uses an offset of 0. */
    u32 vport_y_offset_ena : 1;
    /** Applies PA_CL_VPORT_ZSCALE, otherwise the draw code uses a scale of 1. */
    u32 vport_z_scale_ena : 1;
    /** Applies PA_CL_VPORT_ZOFFSET, otherwise the draw code uses an offset of 0. */
    u32 vport_z_offset_ena : 1;
    u32 pad_6 : 2;
    /**
     * Set when vertex X and Y are already divided by W, which sets the XYDividedByW system
     * flag of the shader translators.
     */
    u32 vtx_xy_fmt : 1;
    /**
     * Set when vertex Z is already divided by W, which sets the ZDividedByW system flag of
     * the shader translators.
     */
    u32 vtx_z_fmt : 1;
    /**
     * Set when W0 is not the reciprocal of W, which sets the WNotReciprocal system flag of
     * the shader translators.
     */
    u32 vtx_w0_fmt : 1;
    u32 perfcounter_ref : 1; /**< No code reads it. */
    u32 pad_12 : 20;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_CL_VTE_CNTL;
};
REX_STATIC_ASSERT_SIZE(PA_CL_VTE_CNTL, sizeof(u32));

/** Top left corner of the screen scissor. */
union alignas(u32) PA_SC_SCREEN_SCISSOR_TL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 14. Left edge in pixels, signed, read by the draw code to clip the window scissor.
     */
    i32 tl_x : 15;
    u32 pad_15 : 1;
    /**
     * Bits 16 to 30. Top edge in pixels, signed, read by the draw code to clip the window scissor.
     */
    i32 tl_y : 15;
    u32 pad_31 : 1;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_SCREEN_SCISSOR_TL;
};
REX_STATIC_ASSERT_SIZE(PA_SC_SCREEN_SCISSOR_TL, sizeof(u32));

/** Bottom right corner of the screen scissor. */
union alignas(u32) PA_SC_SCREEN_SCISSOR_BR {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 14. Right edge in pixels, signed, read by the draw code to clip the window scissor.
     */
    i32 br_x : 15;
    u32 pad_15 : 1;
    /**
     * Bits 16 to 30. Bottom edge in pixels, signed, read by the draw code to clip the window
     * scissor.
     */
    i32 br_y : 15;
    u32 pad_31 : 1;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_SCREEN_SCISSOR_BR;
};
REX_STATIC_ASSERT_SIZE(PA_SC_SCREEN_SCISSOR_BR, sizeof(u32));

/** Offset of the window origin within the screen. */
union alignas(u32) PA_SC_WINDOW_OFFSET {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 14. Signed X offset in pixels added to vertex positions and the window scissor by
     * the draw code.
     */
    i32 window_x_offset : 15;
    u32 pad_15 : 1;
    /**
     * Bits 16 to 30. Signed Y offset in pixels added to vertex positions and the window scissor by
     * the draw code.
     */
    i32 window_y_offset : 15;
    u32 pad_31 : 1;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_WINDOW_OFFSET;
};
REX_STATIC_ASSERT_SIZE(PA_SC_WINDOW_OFFSET, sizeof(u32));

/** Top left corner of the window scissor and a flag disabling the window offset. */
union alignas(u32) PA_SC_WINDOW_SCISSOR_TL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 tl_x : 14; /**< Bits 0 to 13. Left edge in pixels, read by the draw code. */
    u32 pad_14 : 2;
    u32 tl_y : 14; /**< Bits 16 to 29. Top edge in pixels, read by the draw code. */
    u32 pad_30 : 1;
    /**
     * Skips PA_SC_WINDOW_OFFSET for the window scissor, read by the draw code and the draw
     * extent estimator.
     */
    u32 window_offset_disable : 1;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_WINDOW_SCISSOR_TL;
};
REX_STATIC_ASSERT_SIZE(PA_SC_WINDOW_SCISSOR_TL, sizeof(u32));

/** Bottom right corner of the window scissor. */
union alignas(u32) PA_SC_WINDOW_SCISSOR_BR {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 br_x : 14; /**< Bits 0 to 13. Right edge in pixels, read by the draw code. */
    u32 pad_14 : 2;
    u32 br_y : 14; /**< Bits 16 to 29. Bottom edge in pixels, read by the draw code. */
    u32 pad_30 : 2;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::PA_SC_WINDOW_SCISSOR_BR;
};
REX_STATIC_ASSERT_SIZE(PA_SC_WINDOW_SCISSOR_BR, sizeof(u32));

/** Selects the operation of the render backend, such as drawing or copying. */
union alignas(u32) RB_MODECONTROL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 2. Render backend operation, read by the command processors and the draw code to
     * tell drawing from copying.
     */
    xenos::EDRAMMode edram_mode : 3;
    u32 pad_3 : 29;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_MODECONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_MODECONTROL, sizeof(u32));

/** Render target surface pitch, multisample count and hierarchical Z pitch. */
union alignas(u32) RB_SURFACE_INFO {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 13. Render target pitch in pixels. The draw code skips draws when it is 0. */
    u32 surface_pitch : 14;
    u32 pad_14 : 2;
    /**
     * Bits 16 to 17. Multisample count, read by the command processors, the pipeline caches and the
     * render target cache.
     */
    xenos::MSAASamples msaa_samples : 2;
    u32 hiz_pitch : 14; /**< Bits 18 to 31. No code reads it. */
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_SURFACE_INFO;
};
REX_STATIC_ASSERT_SIZE(RB_SURFACE_INFO, sizeof(u32));

/** Alpha test and alpha to mask settings. */
union alignas(u32) RB_COLORCONTROL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 2. Alpha test comparison, used when alpha_test_enable is set. */
    xenos::CompareFunction alpha_func : 3;
    /** Enables the alpha test, read by the command processors and the draw code. */
    u32 alpha_test_enable : 1;
    /**
     * Enables alpha to mask, which also passes bits 24 to 31 of the register to the shaders.
     */
    u32 alpha_to_mask_enable : 1;
    u32 pad_5 : 19;
    /**
     * Alpha to mask threshold offsets for the 4 pixels of each quad. Offsets lower the coverage
     * thresholds by offset / 4 / sample count. 2, 2, 2, 2 makes the test symmetric and is common
     * for undithered alpha to coverage, while 0, 0, 0, 0 leaves it asymmetric, fully transparent
     * for a quarter of the range (half for 2x and almost all for 1x) and fully opaque at a single
     * value.
     * Dithered alpha to mask commonly uses 3, 1, 0, 2.
     *
     * offset0 is for pixel (0, 0), offset1 for (0, 1), offset2 for (1, 0) and offset3 for (1, 1).
     */
    u32 alpha_to_mask_offset0 : 2;
    /**
     * Bits 26 to 27. Threshold offset for pixel (0, 1) of each quad, passed to the shaders in bits
     * 24 to 31 of the register.
     */
    u32 alpha_to_mask_offset1 : 2;
    /**
     * Bits 28 to 29. Threshold offset for pixel (1, 0) of each quad, passed to the shaders in bits
     * 24 to 31 of the register.
     */
    u32 alpha_to_mask_offset2 : 2;
    /**
     * Bits 30 to 31. Threshold offset for pixel (1, 1) of each quad, passed to the shaders in bits
     * 24 to 31 of the register.
     */
    u32 alpha_to_mask_offset3 : 2;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_COLORCONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_COLORCONTROL, sizeof(u32));

/** RB_COLOR1_INFO through RB_COLOR3_INFO use this format too. */
union alignas(u32) RB_COLOR_INFO {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Split from the 12 bit R400 field because EDRAM tile addressing is periodic with 11 bits. */
    /** Bits 0 to 10. EDRAM base of the render target in tiles, low 11 bits. */
    u32 color_base : 11;
    u32 color_base_bit_11 : 1; /**< Bit 11 of the EDRAM base in tiles. */
    u32 pad_12 : 4;
    /**
     * Bits 16 to 19. Render target format, read by the command processors and the render target
     * cache.
     */
    xenos::ColorRenderTargetFormat color_format : 4;
    /**
     * Bits 20 to 25. Signed exponent bias applied to color written to the render target, read by
     * the Direct3D 12 command processor.
     */
    i32 color_exp_bias : 6;
    u32 pad_26 : 6;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_COLOR_INFO;
  /** Registers of render targets 0 to 3 in order, for use with RegisterFile::Get. */
  REX_API static const Register kRtRegisterIndices[4];
};
REX_STATIC_ASSERT_SIZE(RB_COLOR_INFO, sizeof(u32));

/** Per render target color channel write enables. */
union alignas(u32) RB_COLOR_MASK {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * @name Bits 0 to 15. Channel write enables, 4 bits per render target 0 to 3 in red, green,
     * blue, alpha order. The draw code reads them through the raw register.
     */
    /** @{ */
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
    /** @} */
    u32 pad_16 : 16;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_COLOR_MASK;
};
REX_STATIC_ASSERT_SIZE(RB_COLOR_MASK, sizeof(u32));

/** RB_BLENDCONTROL0 through RB_BLENDCONTROL3 use this format. */
union alignas(u32) RB_BLENDCONTROL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    xenos::BlendFactor color_srcblend : 5; /**< Bits 0 to 4. Source factor of the color blend. */
    /** Bits 5 to 7. Operation combining source and destination in the color blend. */
    xenos::BlendOp color_comb_fcn : 3;
    /** Bits 8 to 12. Destination factor of the color blend. */
    xenos::BlendFactor color_destblend : 5;
    u32 pad_13 : 3;
    xenos::BlendFactor alpha_srcblend : 5; /**< Bits 16 to 20. Source factor of the alpha blend. */
    /** Bits 21 to 23. Operation combining source and destination in the alpha blend. */
    xenos::BlendOp alpha_comb_fcn : 3;
    /** Bits 24 to 28. Destination factor of the alpha blend. */
    xenos::BlendFactor alpha_destblend : 5;
    u32 pad_29 : 3;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_BLENDCONTROL0;
  /** Registers of render targets 0 to 3 in order, for use with RegisterFile::Get. */
  REX_API static const Register kRtRegisterIndices[4];
};
REX_STATIC_ASSERT_SIZE(RB_BLENDCONTROL, sizeof(u32));

/** Depth and stencil test settings for front and back faces. */
union alignas(u32) RB_DEPTHCONTROL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 stencil_enable : 1; /**< Enables the stencil test. */
    u32 z_enable : 1;       /**< Enables the depth test. */
    u32 z_write_enable : 1; /**< Allows depth writes. */
    u32 pad_3 : 1;
    xenos::CompareFunction zfunc : 3; /**< Bits 4 to 6. Depth test comparison. */
    /** Uses the _bf stencil fields for back faces of polygons. */
    u32 backface_enable : 1;
    xenos::CompareFunction stencilfunc : 3; /**< Bits 8 to 10. Front face stencil comparison. */
    /** Bits 11 to 13. Front face operation when the stencil test fails. */
    xenos::StencilOp stencilfail : 3;
    /** Bits 14 to 16. Front face operation when the stencil and depth tests pass. */
    xenos::StencilOp stencilzpass : 3;
    /**
     * Bits 17 to 19. Front face operation when the stencil test passes and the depth test fails.
     */
    xenos::StencilOp stencilzfail : 3;
    xenos::CompareFunction stencilfunc_bf : 3; /**< Bits 20 to 22. Back face stencil comparison. */
    /** Bits 23 to 25. Back face operation when the stencil test fails. */
    xenos::StencilOp stencilfail_bf : 3;
    /** Bits 26 to 28. Back face operation when the stencil and depth tests pass. */
    xenos::StencilOp stencilzpass_bf : 3;
    /** Bits 29 to 31. Back face operation when the stencil test passes and the depth test fails. */
    xenos::StencilOp stencilzfail_bf : 3;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_DEPTHCONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_DEPTHCONTROL, sizeof(u32));

/** RB_STENCILREFMASK_BF uses this format too. */
union alignas(u32) RB_STENCILREFMASK {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 7. Stencil reference value, read by the Direct3D 12 command processor. */
    u32 stencilref : 8;
    /** Bits 8 to 15. Mask applied to the stencil reference and buffer values in the test. */
    u32 stencilmask : 8;
    u32 stencilwritemask : 8; /**< Bits 16 to 23. Mask of stencil bits that can be written. */
    u32 pad_24 : 8;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_STENCILREFMASK;
};
REX_STATIC_ASSERT_SIZE(RB_STENCILREFMASK, sizeof(u32));

/** Depth render target base in EDRAM and format. */
union alignas(u32) RB_DEPTH_INFO {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Split from the 12 bit R400 field because EDRAM tile addressing is periodic with 11 bits. */
    /** Bits 0 to 10. EDRAM base of the depth target in tiles, low 11 bits. */
    u32 depth_base : 11;
    u32 depth_base_bit_11 : 1; /**< Bit 11 of the EDRAM base in tiles. */
    u32 pad_12 : 4;
    /**
     * Depth target format, read by the command processors, the pipeline caches and the
     * render target cache.
     */
    xenos::DepthRenderTargetFormat depth_format : 1;
    u32 pad_17 : 15;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_DEPTH_INFO;
};
REX_STATIC_ASSERT_SIZE(RB_DEPTH_INFO, sizeof(u32));

/** Source selection, sample selection, clear enables and command for an EDRAM copy. */
union alignas(u32) RB_COPY_CONTROL {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 2. Source of the copy, where 4 selects depth. The draw code reads it. */
    u32 copy_src_select : 3;
    u32 pad_3 : 1;
    /**
     * Bits 4 to 6. Which samples the copy resolves, read by the draw code and the render target
     * cache.
     */
    xenos::CopySampleSelect copy_sample_select : 3;
    u32 pad_7 : 1;
    /** Clears the color target after the copy, read by the draw code. */
    u32 color_clear_enable : 1;
    /** Clears the depth target after the copy, read by the draw code. */
    u32 depth_clear_enable : 1;
    u32 pad_10 : 10;
    /** Bits 20 to 21. Copy operation, where the draw code accepts only kRaw and kConvert. */
    xenos::CopyCommand copy_command : 2;
    u32 pad_22 : 10;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_COPY_CONTROL;
};
REX_STATIC_ASSERT_SIZE(RB_COPY_CONTROL, sizeof(u32));

/** Destination endianness, layout, format, number format and exponent bias of an EDRAM copy. */
union alignas(u32) RB_COPY_DEST_INFO {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 2. Endian swap applied to the copy destination. */
    xenos::Endian128 copy_dest_endian : 3;
    /**
     * Destination is an array layout, which changes how the draw code computes its tiled
     * extent.
     */
    u32 copy_dest_array : 1;
    /**
     * Bits 4 to 6. Z layer offset of 3D destinations, read by the draw code when it computes their
     * extent.
     */
    u32 copy_dest_layer : 3;
    /**
     * Bits 7 to 12. Destination color format, read by the draw code and the Direct3D 12 command
     * processor.
     */
    xenos::ColorFormat copy_dest_format : 6;
    /** Bits 13 to 15. Number format of the destination. */
    xenos::SurfaceNumberFormat copy_dest_number : 3;
    /**
     * Bits 16 to 21. Signed exponent bias applied to color written to the destination, read by the
     * draw code.
     */
    i32 copy_dest_exp_bias : 6;
    u32 pad_22 : 2;
    /** Written as 0 by the draw code when it builds copy info. */
    u32 copy_dest_swap : 1;
    u32 pad_25 : 7;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_COPY_DEST_INFO;
};
REX_STATIC_ASSERT_SIZE(RB_COPY_DEST_INFO, sizeof(u32));

/** Destination pitch and height of an EDRAM copy. */
union alignas(u32) RB_COPY_DEST_PITCH {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /** Bits 0 to 13. Destination pitch in pixels, read by the draw code. */
    u32 copy_dest_pitch : 14;
    u32 pad_14 : 2;
    /** Bits 16 to 29. Destination height in pixels, read by the draw code. */
    u32 copy_dest_height : 14;
    u32 pad_30 : 2;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::RB_COPY_DEST_PITCH;
};
REX_STATIC_ASSERT_SIZE(RB_COPY_DEST_PITCH, sizeof(u32));

/** Table index that gamma ramp reads and writes address. */
union alignas(u32) DC_LUT_RW_INDEX {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Absolute index for the 256 entry table, without a lower or upper 10 bits selection in bit
     * 0. For PWL, bit 7 is ignored.
     */
    u32 rw_index : 8;
    u32 pad_8 : 24;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_RW_INDEX;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_RW_INDEX, sizeof(u32));

/** Sequential write of 1 color channel of the gamma ramp entry at DC_LUT_RW_INDEX. */
union alignas(u32) DC_LUT_SEQ_COLOR {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * Bits 0 to 15. Value written to 1 channel of the entry at DC_LUT_RW_INDEX, of which the
     * command processor keeps bits 6 to 15. Bits 0 to 5 are hardwired to 0.
     */
    u32 seq_color : 16;
    u32 pad_16 : 16;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_SEQ_COLOR;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_SEQ_COLOR, sizeof(u32));

/** Piecewise linear gamma ramp entry with a base and a delta. */
union alignas(u32) DC_LUT_PWL_DATA {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    /**
     * The output is base + (multiplier * delta) / 2^increment, where increment is the
     * DC_LUTA_CONTROL value for the color channel, base is 7 bits of the front buffer value above
     * the increment bits and multiplier is its lower increment bits. The increment is nonzero,
     * otherwise the 256 entry table is used.
     */
    /**
     * Bits 0 to 15. Base of the entry. Bits 0 to 5 are hardwired to 0 and the Direct3D 12 upload
     * swaps it with delta.
     */
    u32 base : 16;
    u32 delta : 16; /**< Bits 16 to 31. Delta of the entry. Bits 0 to 5 are hardwired to 0. */
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_PWL_DATA;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_PWL_DATA, sizeof(u32));

/** Gamma ramp entry holding 3 channels of 10 bits. */
union alignas(u32) DC_LUT_30_COLOR {
  u32 value; /**< Raw 32 bit register value, sharing storage with the bit fields. */
  struct {
    u32 color_10_blue : 10;  /**< Bits 0 to 9. Blue channel of the entry, 10 bits. */
    u32 color_10_green : 10; /**< Bits 10 to 19. Green channel of the entry, 10 bits. */
    u32 color_10_red : 10;   /**< Bits 20 to 29. Red channel of the entry, 10 bits. */
    u32 pad_30 : 2;
  };
  /** Register that RegisterFile::Get reads this layout from. */
  static constexpr Register kRegisterIndex = xenos::Register::DC_LUT_30_COLOR;
};
REX_STATIC_ASSERT_SIZE(DC_LUT_30_COLOR, sizeof(u32));

}  // namespace reg

}  // namespace rex::xenos
