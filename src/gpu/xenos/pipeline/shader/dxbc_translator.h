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

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <rex/assert.h>
#include "gpu/xenos/format/dxbc.h"
#include <rex/xenos/ucode.h>
#include "gpu/xenos/pipeline/shader/translator.h"
#include <rex/math.h>
#include <rex/string/buffer.h>
#include "gpu/xenos/host/graphics_provider.h"

namespace rex::gpu {

/**
 * Generates shader model 5.1 bytecode for Direct3D 12.
 *
 * DXBC restricts operand usage, so emitted sequences must match what FXC produces.
 *
 * Stage linkage must hold between the VS and PS and for everything between them, such as geometry
 * shaders and built in pixel shaders. The head of the parameters must match between producer and
 * consumer, the tail may differ per stage or be cut off.
 * - TEXCOORD interpolators go VS to GS to PS and every used one is referenced in all stages.
 * - XESPRITETEXCOORD point coordinates go GS to PS and are present in none or all when drawing
 * points with PsParamGen.
 * - SV_Position goes VS to GS to PS and is placed last in the PS so it can be dropped without
 * affecting linkage.
 * - SV_ClipDistance goes VS to GS or RS.
 * - SV_CullDistance, then the vertex kill AND operator in another SV_CullDistance, go VS to GS or
 * RS.
 * - XEPSIZE point size goes VS to GS only.
 */
class DXBCShaderTranslator : public ShaderTranslator {
 public:
  DXBCShaderTranslator(gpu::GraphicsProvider::GPUVendorID vendor_id, bool bindless_resources_used,
                       bool edram_rov_used, bool gamma_render_target_as_unorm8 = false,
                       bool msaa_2x_supported = true, u32 draw_resolution_scale_x = 1,
                       u32 draw_resolution_scale_y = 1, bool force_emit_source_map = false);
  ~DXBCShaderTranslator() override;

  union Modification {
    /** Layout version in 0xYYYYMMDD form, increased on incompatible changes to invalidate pipeline
     * storage. */
    static constexpr u32 kVersion = 0x20260226;

    // NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
    enum class DepthStencilMode : u32 {
      kNoModifiers,
      /** Enables [earlydepthstencil] when alpha test and alpha to coverage are disabled, ignored if
         the shader blocks early Z writes. */
      kEarlyHint,
      /**
       * Converts depth to the nearest 32 bit float exactly representable as 20e4, rounding towards
       * zero, for invariance when the guest reuploads a resolved depth buffer to EDRAM.
       *
       * Allows SV_DepthLessEqual for early Z culling. MSAA needs per sample depth, so the pixel
       * shader runs at sample frequency. Viewport depth bounds must be snapped to float24.
       */
      kFloat24Truncating,
      /**
       * Like kFloat24Truncating but rounds to nearest even and always writes SV_Depth, since
       * rounding up gives a bigger value.
       */
      kFloat24Rounding,
    };

    u64 value;
    struct VertexShaderModification {
      u32 interpolator_mask : xenos::kMaxInterpolators;
      u32 user_clip_plane_count : 3;
      u32 user_clip_plane_cull : 1;
      u32 vertex_kill_and : 1;
      u32 output_point_size : 1;
      /** Value from SQ_PROGRAM_CNTL. */
      u32 dynamic_addressable_register_count : 8;
      /** Value of PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives. */
      u32 point_ps_ucp_mode : 2;
      Shader::HostVertexShaderType host_vertex_shader_type : Shader::kHostVertexShaderTypeBitCount;
    } vertex;
    struct PixelShaderModification {
      u32 interpolator_mask : xenos::kMaxInterpolators;
      u32 interpolators_centroid : xenos::kMaxInterpolators;
      u32 param_gen_enable : 1;
      u32 param_gen_interpolator : 4;
      /** Must be set for point primitives and clear otherwise when param_gen_enable is set. Enables
       * the point sprite coordinates input and changes the PsParamGen flag bits. */
      u32 param_gen_point : 1;
      u32 dynamic_addressable_register_count : 8;
      /** Used without ROV only. */
      DepthStencilMode depth_stencil_mode : 2;
    } pixel;

    explicit Modification(u64 modification_value = 0) : value(modification_value) {
      REX_STATIC_ASSERT_SIZE(*this, sizeof(value));
    }

    u32 GetVertexClipDistanceCount() const {
      return vertex.user_clip_plane_cull ? 0 : vertex.user_clip_plane_count;
    }
    u32 GetVertexCullDistanceCount() const {
      return (vertex.user_clip_plane_cull ? vertex.user_clip_plane_count : 0) +
             vertex.vertex_kill_and;
    }
  };

  /** Constant buffer registers in space 0. */
  enum class CbufferRegister : u8 {
    kSystemConstants,
    kFloatConstants,
    kBoolLoopConstants,
    kFetchConstants,
    kDescriptorIndices,
  };

  /** Some flags are referenced in xenos_draw.hlsli and must stay in sync with it. */
  enum : u32 {
    kSysFlag_SharedMemoryIsUAV_Shift,
    kSysFlag_XYDividedByW_Shift,
    kSysFlag_ZDividedByW_Shift,
    kSysFlag_WNotReciprocal_Shift,
    kSysFlag_PrimitivePolygonal_Shift,
    kSysFlag_PrimitiveLine_Shift,
    kSysFlag_DepthFloat24_Shift,
    kSysFlag_AlphaPassIfLess_Shift,
    kSysFlag_AlphaPassIfEqual_Shift,
    kSysFlag_AlphaPassIfGreater_Shift,
    kSysFlag_ConvertColor0ToGamma_Shift,
    kSysFlag_ConvertColor1ToGamma_Shift,
    kSysFlag_ConvertColor2ToGamma_Shift,
    kSysFlag_ConvertColor3ToGamma_Shift,

    kSysFlag_ROVDepthStencil_Shift,
    kSysFlag_ROVDepthPassIfLess_Shift,
    kSysFlag_ROVDepthPassIfEqual_Shift,
    kSysFlag_ROVDepthPassIfGreater_Shift,
    /** Set to write the new depth, clear to keep the old one when the depth test passes. */
    kSysFlag_ROVDepthWrite_Shift,
    kSysFlag_ROVStencilTest_Shift,
    /**
     * Writes a stencil value that differs from the depth buffer even when the depth and stencil
     * test failed, without running the rest of the shader. Used when alpha test and alpha to
     * coverage are disabled. Ignored when the shader has kill instructions or writes depth.
     */
    kSysFlag_ROVDepthStencilEarlyWrite_Shift,

    kSysFlag_Count,

    kSysFlag_SharedMemoryIsUAV = 1u << kSysFlag_SharedMemoryIsUAV_Shift,
    kSysFlag_XYDividedByW = 1u << kSysFlag_XYDividedByW_Shift,
    kSysFlag_ZDividedByW = 1u << kSysFlag_ZDividedByW_Shift,
    kSysFlag_WNotReciprocal = 1u << kSysFlag_WNotReciprocal_Shift,
    kSysFlag_PrimitivePolygonal = 1u << kSysFlag_PrimitivePolygonal_Shift,
    kSysFlag_PrimitiveLine = 1u << kSysFlag_PrimitiveLine_Shift,
    kSysFlag_DepthFloat24 = 1u << kSysFlag_DepthFloat24_Shift,
    kSysFlag_AlphaPassIfLess = 1u << kSysFlag_AlphaPassIfLess_Shift,
    kSysFlag_AlphaPassIfEqual = 1u << kSysFlag_AlphaPassIfEqual_Shift,
    kSysFlag_AlphaPassIfGreater = 1u << kSysFlag_AlphaPassIfGreater_Shift,
    kSysFlag_ConvertColor0ToGamma = 1u << kSysFlag_ConvertColor0ToGamma_Shift,
    kSysFlag_ConvertColor1ToGamma = 1u << kSysFlag_ConvertColor1ToGamma_Shift,
    kSysFlag_ConvertColor2ToGamma = 1u << kSysFlag_ConvertColor2ToGamma_Shift,
    kSysFlag_ConvertColor3ToGamma = 1u << kSysFlag_ConvertColor3ToGamma_Shift,
    kSysFlag_ROVDepthStencil = 1u << kSysFlag_ROVDepthStencil_Shift,
    kSysFlag_ROVDepthPassIfLess = 1u << kSysFlag_ROVDepthPassIfLess_Shift,
    kSysFlag_ROVDepthPassIfEqual = 1u << kSysFlag_ROVDepthPassIfEqual_Shift,
    kSysFlag_ROVDepthPassIfGreater = 1u << kSysFlag_ROVDepthPassIfGreater_Shift,
    kSysFlag_ROVDepthWrite = 1u << kSysFlag_ROVDepthWrite_Shift,
    kSysFlag_ROVStencilTest = 1u << kSysFlag_ROVStencilTest_Shift,
    kSysFlag_ROVDepthStencilEarlyWrite = 1u << kSysFlag_ROVDepthStencilEarlyWrite_Shift,
  };
  static_assert(kSysFlag_Count <= 32, "Too many flags in the system constants");

  /**
   * Layout changes require updating SystemConstants::Index, system_constant_rdef_ and
   * d3d12/shaders/xenos_draw.hlsli.
   */
  struct SystemConstants {
    u32 flags;
    union {
      struct {
        f32 tessellation_factor_range_min;
        f32 tessellation_factor_range_max;
      };
      f32 tessellation_factor_range[2];
    };
    u32 line_loop_closing_index;

    xenos::Endian vertex_index_endian;
    u32 vertex_index_offset;
    union {
      struct {
        u32 vertex_index_min;
        u32 vertex_index_max;
      };
      u32 vertex_index_min_max[2];
    };

    f32 user_clip_planes[6][4];

    f32 ndc_scale[3];
    f32 point_vertex_diameter_min;

    f32 ndc_offset[3];
    f32 point_vertex_diameter_max;

    f32 point_constant_diameter[2];
    /** Converts diameter in guest screen coordinates to radius in NDC for the host viewport. */
    f32 point_screen_diameter_to_ndc_radius[2];

    /** Post swizzle TextureSign values, 1 byte per needed component of each of the 32 texture fetch
     * constants. */
    u32 texture_swizzled_signs[8];

    u32 textures_resolution_scaled;
    /** Log2 of the X and Y sample size, used for alpha to mask and for EDRAM addressing with ROV
     * MSAA. */
    u32 sample_count_log2[2];
    f32 alpha_test_reference;

    /** 0 when alpha to mask is disabled. When enabled, bits 0 to 7 are sample offsets and bit 8 is
     * set. */
    u32 alpha_to_mask;
    u32 edram_32bpp_tile_pitch_dwords_scaled;
    u32 edram_depth_base_dwords_scaled;
    u32 padding_edram_depth_base_dwords_scaled;

    f32 color_exp_bias[4];

    union {
      struct {
        f32 edram_poly_offset_front_scale;
        f32 edram_poly_offset_front_offset;
      };
      f32 edram_poly_offset_front[2];
    };
    union {
      struct {
        f32 edram_poly_offset_back_scale;
        f32 edram_poly_offset_back_offset;
      };
      f32 edram_poly_offset_back[2];
    };

    /**
     * Stencil function and operations match the RB_DEPTHCONTROL layout.
     *
     * - Bits 0 to 2 are the comparison function (bit 0 less, bit 1 equal, bit 2 greater).
     * - Bits 3 to 5 are the fail operation.
     * - Bits 6 to 8 are the pass operation.
     * - Bits 9 to 11 are the depth fail operation.
     */
    union {
      struct {
        u32 edram_stencil_front_reference;
        u32 edram_stencil_front_read_mask;
        u32 edram_stencil_front_write_mask;
        u32 edram_stencil_front_func_ops;

        u32 edram_stencil_back_reference;
        u32 edram_stencil_back_read_mask;
        u32 edram_stencil_back_write_mask;
        u32 edram_stencil_back_func_ops;
      };
      struct {
        u32 edram_stencil_front[4];
        u32 edram_stencil_back[4];
      };
      u32 edram_stencil[2][4];
    };

    u32 edram_rt_base_dwords_scaled[4];

    /** Render target format combined with RenderTargetCache::kPSIColorFormatFlag values from
     * RenderTargetCache::AddPSIColorFormatFlags. */
    u32 edram_rt_format_flags[4];

    /** Color clamp values applied before blending or storing, ordered low color, low alpha, high
     * color, high alpha. */
    f32 edram_rt_clamp[4][4];

    /** Mask applied to the old packed render target data and, inverted, to the new packed data
     * before storing. When 0 and not blending, reading the old data from EDRAM is not required. */
    u32 edram_rt_keep_mask[4][2];

    /** RB_BLENDCONTROL factors and operations masked with 0x1FFF1FFF. A value of 0x00010001 (1 *
     * src + 0 * dst) disables blending for the render target. */
    u32 edram_rt_blend_factors_ops[4];

    f32 edram_blend_constant[4];

   private:
    friend class DXBCShaderTranslator;

    enum class Index : u8 {
      kFlags,
      kTessellationFactorRange,
      kLineLoopClosingIndex,

      kVertexIndexEndian,
      kVertexIndexOffset,
      kVertexIndexMinMax,

      kUserClipPlanes,

      kNDCScale,
      kPointVertexDiameterMin,

      kNDCOffset,
      kPointVertexDiameterMax,

      kPointConstantDiameter,
      kPointScreenDiameterToNDCRadius,

      kTextureSwizzledSigns,

      kTexturesResolutionScaled,
      kSampleCountLog2,
      kAlphaTestReference,

      kAlphaToMask,
      kEDRAM32bppTilePitchDwordsScaled,
      kEDRAMDepthBaseDwordsScaled,

      kColorExpBias,

      kEDRAMPolyOffsetFront,
      kEDRAMPolyOffsetBack,

      kEDRAMStencil,

      kEDRAMRTBaseDwordsScaled,

      kEDRAMRTFormatFlags,

      kEDRAMRTClamp,

      kEDRAMRTKeepMask,

      kEDRAMRTBlendFactorsOps,

      kEDRAMBlendConstant,

      kCount,
    };
    static_assert(std::to_underlying(Index::kCount) <= 64,
                  "Too many system constants, can't use uint64_t for usage bits");
  };

  /** Shader resource view binding spaces. */
  enum class SRVSpace : u8 {
    /** Uses the SRVMainRegister t# layout. */
    kMain,
    kBindlessTextures2DArray,
    kBindlessTextures3D,
    kBindlessTexturesCube,
  };

  /** Shader resource view registers in SRVSpace::kMain. */
  enum class SRVMainRegister : u8 {
    kSharedMemory,
    kBindfulTexturesStart,
  };

  /** At most 192 textures, from 32 fetch constants times 2D array, 3D or cube dimensions and signed
   * or unsigned. */
  static constexpr u32 kMaxTextureBindingIndexBits = 8;
  static constexpr u32 kMaxTextureBindings = (1 << kMaxTextureBindingIndexBits) - 1;
  struct TextureBinding {
    u32 bindful_srv_index;
    u32 bindful_srv_rdef_name_ptr;
    u32 bindless_descriptor_index;
    u32 fetch_constant;
    xenos::FetchOpDimension dimension;
    bool is_signed;
    std::string bindful_name;
  };

  /**
   * Limit of sampler bindings so the count packs into 32 bit map keys such as root signature keys.
   */
  static constexpr u32 kMaxSamplerBindingIndexBits = 7;
  static constexpr u32 kMaxSamplerBindings = (1 << kMaxSamplerBindingIndexBits) - 1;
  struct SamplerBinding {
    u32 bindless_descriptor_index;
    u32 fetch_constant;
    xenos::TextureFilter mag_filter;
    xenos::TextureFilter min_filter;
    xenos::TextureFilter mip_filter;
    xenos::AnisoFilter aniso_filter;
    std::string bindful_name;
  };

  /** Unordered access view registers in space 0. */
  enum class UAVRegister : u8 {
    kSharedMemory,
    kEDRAM,
  };

  u64 GetDefaultVertexShaderModification(u32 dynamic_addressable_register_count,
                                         Shader::HostVertexShaderType host_vertex_shader_type =
                                             Shader::HostVertexShaderType::kVertex) const override;
  u64 GetDefaultPixelShaderModification(u32 dynamic_addressable_register_count) const override;

  /**
   * Creates a pixel shader without color outputs.
   *
   * Resets the translator state.
   *
   * @return The translated shader binary.
   */
  std::vector<u8> CreateDepthOnlyPixelShader();

  /**
   * Converts a color externally clamped to [0, 31.875] to 7e3 floating point in bits 0 to 9,
   * rounding to nearest even.
   *
   * Source and destination may be the same, the temporary must differ from both.
   *
   * @param a Assembler receiving the instructions.
   * @param f10_temp Temporary register receiving the 7e3 value.
   * @param f10_temp_component Component of f10_temp to write.
   * @param f32_temp Temporary register holding the clamped float32 source.
   * @param f32_temp_component Component of f32_temp to read.
   * @param temp_temp Scratch temporary register.
   * @param temp_temp_component Component of temp_temp to use.
   */
  static void PreClampedFloat32To7e3(dxbc::Assembler& a, u32 f10_temp, u32 f10_temp_component,
                                     u32 f32_temp, u32 f32_temp_component, u32 temp_temp,
                                     u32 temp_temp_component);
  /**
   * Same as PreClampedFloat32To7e3 but clamps the input to [0, 31.875].
   *
   * @param a Assembler receiving the instructions.
   * @param f10_temp Temporary register receiving the 7e3 value.
   * @param f10_temp_component Component of f10_temp to write.
   * @param f32_temp Temporary register holding the float32 source.
   * @param f32_temp_component Component of f32_temp to read.
   * @param temp_temp Scratch temporary register.
   * @param temp_temp_component Component of temp_temp to use.
   */
  static void UnclampedFloat32To7e3(dxbc::Assembler& a, u32 f10_temp, u32 f10_temp_component,
                                    u32 f32_temp, u32 f32_temp_component, u32 temp_temp,
                                    u32 temp_temp_component);
  /**
   * Converts the 7e3 number in bits [f10_shift, f10_shift + 10) to a 32 bit float.
   *
   * The temporaries must differ from each other, 1 may be the source. The destination may be
   * anything writable.
   *
   * @param a Assembler receiving the instructions.
   * @param f32 Destination receiving the float32 result.
   * @param f10_temp Temporary register holding the 7e3 source.
   * @param f10_temp_component Component of f10_temp to read.
   * @param f10_shift Bit offset of the 7e3 number in the source.
   * @param temp1_temp First scratch temporary register.
   * @param temp1_temp_component Component of temp1_temp to use.
   * @param temp2_temp Second scratch temporary register.
   * @param temp2_temp_component Component of temp2_temp to use.
   */
  static void Float7e3To32(dxbc::Assembler& a, const dxbc::Dest& f32, u32 f10_temp,
                           u32 f10_temp_component, u32 f10_shift, u32 temp1_temp,
                           u32 temp1_temp_component, u32 temp2_temp, u32 temp2_temp_component);
  /**
   * Converts a depth externally clamped to [0, 2) to 20e4 floating point in bits 0 to 23, rounding
   * to nearest even or towards zero.
   *
   * Source and destination may be the same, the temporary must differ from both.
   *
   * @param a Assembler receiving the instructions.
   * @param f24_temp Temporary register receiving the 20e4 value.
   * @param f24_temp_component Component of f24_temp to write.
   * @param f32_temp Temporary register holding the clamped float32 source.
   * @param f32_temp_component Component of f32_temp to read.
   * @param temp_temp Scratch temporary register.
   * @param temp_temp_component Component of temp_temp to use.
   * @param round_to_nearest_even Whether to round to nearest even instead of towards zero.
   * @param remap_from_0_to_0_5 Whether the input has 0...1 pre-remapped to 0...0.5.
   */
  static void PreClampedDepthTo20e4(dxbc::Assembler& a, u32 f24_temp, u32 f24_temp_component,
                                    u32 f32_temp, u32 f32_temp_component, u32 temp_temp,
                                    u32 temp_temp_component, bool round_to_nearest_even,
                                    bool remap_from_0_to_0_5);
  /**
   * Converts the 20e4 number in bits [f24_shift, f24_shift + 10) to a 32 bit float.
   *
   * The temporaries must differ from each other, 1 may be the source. The destination may be
   * anything writable.
   *
   * @param a Assembler receiving the instructions.
   * @param f32 Destination receiving the float32 result.
   * @param f24_temp Temporary register holding the 20e4 source.
   * @param f24_temp_component Component of f24_temp to read.
   * @param f24_shift Bit offset of the 20e4 number in the source.
   * @param temp1_temp First scratch temporary register.
   * @param temp1_temp_component Component of temp1_temp to use.
   * @param temp2_temp Second scratch temporary register.
   * @param temp2_temp_component Component of temp2_temp to use.
   * @param remap_to_0_to_0_5 Whether 0...1 in float24 maps to 0...0.5 in float32.
   */
  static void Depth20e4To32(dxbc::Assembler& a, const dxbc::Dest& f32, u32 f24_temp,
                            u32 f24_temp_component, u32 f24_shift, u32 temp1_temp,
                            u32 temp1_temp_component, u32 temp2_temp, u32 temp2_temp_component,
                            bool remap_to_0_to_0_5);

  /**
   * Converts 1 scalar from piecewise linear gamma to linear, saturating the source internally
   * unless it is pre saturated.
   *
   * The target may be the same as the source, the temporaries must differ from both.
   *
   * @param a Assembler receiving the instructions.
   * @param target_temp Temporary register receiving the linear value.
   * @param target_temp_component Component of target_temp to write.
   * @param source_temp Temporary register holding the gamma source.
   * @param source_temp_component Component of source_temp to read.
   * @param source_pre_saturated Whether the source is already saturated.
   * @param temp1 First scratch temporary register.
   * @param temp1_component Component of temp1 to use.
   * @param temp2 Second scratch temporary register.
   * @param temp2_component Component of temp2 to use.
   */
  static void PWLGammaToLinear(dxbc::Assembler& a, u32 target_temp, u32 target_temp_component,
                               u32 source_temp, u32 source_temp_component,
                               bool source_pre_saturated, u32 temp1, u32 temp1_component, u32 temp2,
                               u32 temp2_component);
  /**
   * Converts 1 pre saturated scalar from linear to piecewise linear gamma.
   *
   * The target may be the same as the source or as temp_or_target but not both, and temp_or_target
   * must differ from the source. temp_non_target must differ from all others.
   *
   * @param a Assembler receiving the instructions.
   * @param target_temp Temporary register receiving the gamma value.
   * @param target_temp_component Component of target_temp to write.
   * @param source_temp Temporary register holding the linear source.
   * @param source_temp_component Component of source_temp to read.
   * @param temp_or_target Scratch temporary register that may be the target.
   * @param temp_or_target_component Component of temp_or_target to use.
   * @param temp_non_target Scratch temporary register distinct from all others.
   * @param temp_non_target_component Component of temp_non_target to use.
   */
  static void PreSaturatedLinearToPWLGamma(dxbc::Assembler& a, u32 target_temp,
                                           u32 target_temp_component, u32 source_temp,
                                           u32 source_temp_component, u32 temp_or_target,
                                           u32 temp_or_target_component, u32 temp_non_target,
                                           u32 temp_non_target_component);

 protected:
  void Reset() override;

  u32 GetModificationRegisterCount() const override;

  void StartTranslation() override;
  std::vector<u8> CompleteTranslation() override;
  void PostTranslation() override;

  void ProcessLabel(u32 cf_index) override;

  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr, u8 export_eM) override;

  void ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& instr) override;
  /**
   * Translates a texture fetch.
   *
   * Offsets get an extra 1.5/4 of the fixed point texture coordinate ULP, which resolves the
   * ambiguity between texels in point sampling for normalized and unnormalized coordinates. 1/4 is
   * not enough at 3x resolution scale. Correcting the division error instead fails because the
   * sampler's internal multiply rounds differently from `mul` on Nvidia.
   *
   * getCompTexLOD forces linear mip filtering because `lod` is undefined for point sampling and its
   * result may be biased later, and the XNA assembler rejects MipFilter overrides for
   * getCompTexLOD.
   *
   * @param instr Parsed texture fetch instruction.
   */
  void ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(const ParsedAluInstruction& instr,
                             u8 memexport_eM_potentially_written_before) override;

 private:
  /** Changing these requires updating WriteInputSignature and WriteOutputSignature. */
  static constexpr u32 kInRegisterVSVertexIndex = 0;
  static constexpr u32 kInRegisterDSControlPointIndex = 0;

  /**
   * Marks a system constant as used. Together with GetSystemConstantSrc it builds a source
   * unconditionally, otherwise use LoadSystemConstant.
   *
   * @param index System constant to mark.
   */
  void MarkSystemConstantUsed(SystemConstants::Index index) {
    system_constants_used_ |= static_cast<u64>(1) << u32(index);
  }
  /**
   * Builds a constant buffer source for a system constant.
   *
   * Swizzle components are relative to the first component of the vector at the offset, so XXXX
   * selects a scalar and moves to YYYY when it sits at +4 in its 16 byte vector. Out of bounds
   * components are allowed and dropped later.
   *
   * @param offset offsetof(SystemConstants, field).
   * @param swizzle Swizzle relative to the first component at the offset.
   * @return Constant buffer source for the requested components.
   */
  dxbc::Src GetSystemConstantSrc(size_t offset, u32 swizzle) const {
    u32 first_component = static_cast<u32>((offset >> 2) & 3);
    return dxbc::Src::CB(
        cbuffer_index_system_constants_, std::to_underlying(CbufferRegister::kSystemConstants),
        static_cast<u32>(offset >> 4),
        std::min((swizzle & 3) + first_component, static_cast<u32>(3)) |
            std::min(((swizzle >> 2) & 3) + first_component, static_cast<u32>(3)) << 2 |
            std::min(((swizzle >> 4) & 3) + first_component, static_cast<u32>(3)) << 4 |
            std::min(((swizzle >> 6) & 3) + first_component, static_cast<u32>(3)) << 6);
  }
  dxbc::Src LoadSystemConstant(SystemConstants::Index index, size_t offset, u32 swizzle) {
    MarkSystemConstantUsed(index);
    return GetSystemConstantSrc(offset, swizzle);
  }
  dxbc::Src LoadFlagsSystemConstant() {
    return LoadSystemConstant(SystemConstants::Index::kFlags, offsetof(SystemConstants, flags),
                              dxbc::Src::kXXXX);
  }

  Modification GetDXBCShaderModification() const {
    return Modification(current_translation().modification());
  }

  bool IsDXBCVertexShader() const {
    return is_vertex_shader() && !Shader::IsHostVertexShaderTypeDomain(
                                     GetDXBCShaderModification().vertex.host_vertex_shader_type);
  }
  bool IsDXBCDomainShader() const {
    return is_vertex_shader() && Shader::IsHostVertexShaderTypeDomain(
                                     GetDXBCShaderModification().vertex.host_vertex_shader_type);
  }

  bool IsForceEarlyDepthStencilGlobalFlagEnabled() const {
    return is_pixel_shader() &&
           GetDXBCShaderModification().pixel.depth_stencil_mode ==
               Modification::DepthStencilMode::kEarlyHint &&
           !edram_rov_used_ && current_shader().implicit_early_z_write_allowed();
  }

  u32 GetModificationInterpolatorMask() const {
    Modification modification = GetDXBCShaderModification();
    return is_vertex_shader() ? modification.vertex.interpolator_mask
                              : modification.pixel.interpolator_mask;
  }

  /**
   * Reports whether control flow uses switch-case rather than if (pc >= label).
   *
   * @return True when control flow uses switch-case.
   */
  bool UseSwitchForControlFlow() const;

  /**
   * Allocates consecutive internal r# registers.
   *
   * @param zero_mask Mask of components to initialize to zero.
   * @param count Number of registers to allocate.
   * @return Index of the first register.
   */
  u32 PushSystemTemp(u32 zero_mask = 0, u32 count = 1);
  /**
   * Frees the last allocated internal r# registers for reuse.
   *
   * @param count Number of registers to free.
   */
  void PopSystemTemp(u32 count = 1);

  /**
   * Exports eM# to memory. Modifies eA and eM# values, so call only before starting a new export or
   * ending or deactivating the invocation.
   *
   * @param export_eM Mask of eM# registers to export.
   */
  void ExportToMemory(u8 export_eM);

  bool IsSampleRate() const {
    REX_ASSERT(is_pixel_shader());
    return DSV_IsWritingFloat24Depth() && !current_shader().writes_depth();
  }
  bool IsDepthStencilSystemTempUsed() const {
    if (edram_rov_used_) {
      return true;
    }
    if (current_shader().writes_depth()) {
      return true;
    }
    return false;
  }
  /**
   * Reports whether the non ROV pixel shader converts depth to 20e4.
   *
   * @return True when the non ROV pixel shader converts depth to 20e4.
   */
  bool DSV_IsWritingFloat24Depth() const {
    if (edram_rov_used_) {
      return false;
    }
    Modification::DepthStencilMode depth_stencil_mode =
        GetDXBCShaderModification().pixel.depth_stencil_mode;
    return depth_stencil_mode == Modification::DepthStencilMode::kFloat24Truncating ||
           depth_stencil_mode == Modification::DepthStencilMode::kFloat24Rounding;
  }
  /**
   * Reports whether it is possible and worthwhile to skip running the shader for 2x2 quads.
   *
   * @return True when skipping the shader for 2x2 quads is possible and worthwhile.
   */
  bool ROV_IsDepthStencilEarly() const {
    REX_ASSERT(edram_rov_used_);
    return !is_depth_only_pixel_shader_ && !current_shader().writes_depth() &&
           !current_shader().memexport_eM_written();
  }
  /**
   * Converts the pre clamped depth to 24 bit in bits 0 to 23 with 24 to 31 zero, leaving no room
   * for stencil since the result is used in comparisons. Uses the format from the system constants.
   *
   * Source and destination may be the same, the temporary must differ from both.
   *
   * @param d24_temp Temporary register receiving the 24 bit depth.
   * @param d24_temp_component Component of d24_temp to write.
   * @param d32_temp Temporary register holding the pre clamped depth.
   * @param d32_temp_component Component of d32_temp to read.
   * @param temp_temp Scratch temporary register.
   * @param temp_temp_component Component of temp_temp to use.
   */
  void ROV_DepthTo24Bit(u32 d24_temp, u32 d24_temp_component, u32 d32_temp, u32 d32_temp_component,
                        u32 temp_temp, u32 temp_temp_component);
  /**
   * Performs the depth and stencil test, writing depending on whether it is late or safe to do
   * early.
   *
   * Updates system_temp_rov_params_ result and coverage if allowed and safe, and
   * system_temp_depth_stencil_. Returns from the shader when early, the coverage is empty for all
   * pixels of the 2x2 quad and it is safe not to await kills, alpha test or alpha to coverage.
   */
  void ROV_DepthStencilTest();
  /**
   * Unpacks a 32bpp or 64bpp color from packed_temp.packed_temp_components to color_temp using 2
   * temporary VGPRs.
   *
   * @param rt_index Render target index.
   * @param packed_temp Temporary register holding the packed color.
   * @param packed_temp_components Component of packed_temp where the packed color starts.
   * @param color_temp Temporary register receiving the unpacked color.
   * @param temp1 First scratch VGPR.
   * @param temp1_component Component of temp1 to use.
   * @param temp2 Second scratch VGPR.
   * @param temp2_component Component of temp2 to use.
   */
  void ROV_UnpackColor(u32 rt_index, u32 packed_temp, u32 packed_temp_components, u32 color_temp,
                       u32 temp1, u32 temp1_component, u32 temp2, u32 temp2_component);
  /**
   * Packs a float32x4 color from color_temp to a 32bpp or 64bpp value in
   * packed_temp.packed_temp_components using 2 temporary VGPRs.
   *
   * color_temp and packed_temp may be the same when packed_temp_components is 0. A 32bpp format
   * writes the high part to break the register dependency.
   *
   * @param rt_index Render target index.
   * @param color_temp Temporary register holding the float32x4 color.
   * @param packed_temp Temporary register receiving the packed color.
   * @param packed_temp_components Component of packed_temp where the packed color starts.
   * @param temp1 First scratch VGPR.
   * @param temp1_component Component of temp1 to use.
   * @param temp2 Second scratch VGPR.
   * @param temp2_component Component of temp2 to use.
   */
  void ROV_PackPreClampedColor(u32 rt_index, u32 color_temp, u32 packed_temp,
                               u32 packed_temp_components, u32 temp1, u32 temp1_component,
                               u32 temp2, u32 temp2_component);
  /**
   * Emits `case` labels for color blend factors, generating the factor from src_temp.rgb and
   * dst_temp.rgb into factor_temp.rgb.
   *
   * factor_temp may be src_temp or dst_temp.
   *
   * @param src_temp Temporary register holding the source color.
   * @param dst_temp Temporary register holding the destination color.
   * @param factor_temp Temporary register receiving the factor.
   */
  void ROV_HandleColorBlendFactorCases(u32 src_temp, u32 dst_temp, u32 factor_temp);
  /**
   * Emits `case` labels for alpha blend factors, generating the factor from src_temp.a and
   * dst_temp.a into factor_temp.factor_component.
   *
   * factor_temp may be src_temp or dst_temp.
   *
   * @param src_temp Temporary register holding the source alpha.
   * @param dst_temp Temporary register holding the destination alpha.
   * @param factor_temp Temporary register receiving the factor.
   * @param factor_component Component of factor_temp to write.
   */
  void ROV_HandleAlphaBlendFactorCases(u32 src_temp, u32 dst_temp, u32 factor_temp,
                                       u32 factor_component);

  /**
   * Applies the index offset to vertex or tessellation patch indices, clamps them to the minimum
   * and maximum index values and converts them to float.
   *
   * The destination may be the same as the source.
   *
   * @param dest_temp Temporary register receiving the converted indices.
   * @param dest_temp_components Components of dest_temp to write.
   * @param src Source holding the raw indices.
   */
  void RemapAndConvertVertexIndices(u32 dest_temp, u32 dest_temp_components, const dxbc::Src& src);
  void StartVertexShader_LoadVertexIndex();
  void StartVertexOrDomainShader();
  /**
   * Loads the ROV sample coverage and EDRAM addressing parameters.
   *
   * 4x MSAA coverage maps the top right sample to 2 and the bottom left to 1, the opposite of
   * Direct3D 12, because Xenos 2x MSAA doubles the storage height and 4x MSAA doubles the width.
   */
  void StartPixelShader_LoadROVParameters();
  /**
   * Sets up the pixel shader inputs, PsParamGen and the memexport conditions.
   *
   * PsParamGen X holds the floored pixel X with the back face flag in its sign bit, and Y the
   * floored pixel Y with the point flag in its sign bit. For points ZW hold the saturated point
   * sprite coordinates, otherwise the sign bit of Z flags lines and W is unused. The position is
   * made absolute so host quads at odd locations keep correct derivatives on the left and top
   * edges.
   */
  void StartPixelShader();

  void CompleteVertexOrDomainShader();
  /**
   * Tests 1 sample against alpha to mask.
   *
   * For RTV, adds the sample to coverage_temp.coverage_temp_component when it passes, or overwrites
   * the output when initialize is true for the first tested sample. For ROV, masks the sample out
   * of coverage_temp.coverage_temp_component when it fails.
   *
   * threshold_offset and temp.temp_component may be the same.
   *
   * @param initialize Whether this is the first tested sample.
   * @param sample_index Index of the sample to test.
   * @param threshold_base Constant part of the alpha threshold.
   * @param threshold_offset Variable part of the alpha threshold.
   * @param threshold_offset_scale Scale applied to threshold_offset.
   * @param coverage_temp Temporary register holding the coverage.
   * @param coverage_temp_component Component of coverage_temp to update.
   * @param temp Scratch temporary register.
   * @param temp_component Component of temp to use.
   */
  void CompletePixelShader_AlphaToMaskSample(bool initialize, u32 sample_index, f32 threshold_base,
                                             dxbc::Src threshold_offset, f32 threshold_offset_scale,
                                             u32 coverage_temp, u32 coverage_temp_component,
                                             u32 temp, u32 temp_component);
  /**
   * Performs alpha to coverage. For RTV writes oMask, for ROV updates the coverage bits of
   * system_temp_rov_params_.x. Done manually for RTV to keep the guest dithering pattern and
   * because alpha can be exponent biased.
   */
  void CompletePixelShader_AlphaToMask();
  void CompletePixelShader_WriteToRTVs();
  void CompletePixelShader_DSV_DepthTo24Bit();
  void CompletePixelShader_WriteToROV();
  void CompletePixelShader();

  void CompleteShaderCode();

  /** Writes the original instruction disassembly from instruction_disassembly_buffer_ as shader
   * messages when enabled. */
  void EmitInstructionDisassembly();

  /**
   * Converts a translator source operand to a DXBC operand, or a zero literal if it is not
   * referenced.
   *
   * May allocate a temporary and emit instructions when the operand cannot be used as is, such as
   * an indexable GPR. In that case temp_pushed_out is set and the caller must PopSystemTemp when
   * done.
   *
   * @param operand Translator source operand to convert.
   * @param needed_components Mask of components the caller reads.
   * @param temp_pushed_out Set when a system temporary was allocated.
   * @return DXBC source for the operand.
   */
  dxbc::Src LoadOperand(const InstructionOperand& operand, u32 needed_components,
                        bool& temp_pushed_out);
  /**
   * Writes src, which must be usable as a vector `mov` source including to x#, to an instruction
   * result target.
   *
   * @param result Instruction result target.
   * @param src Value to write.
   * @param can_store_memexport_address Allows only MADs with a stream constant to write eA.
   */
  void StoreResult(const InstructionResult& result, const dxbc::Src& src,
                   bool can_store_memexport_address = false);

  /**
   * Conditionals nest as pc checks for labels, then the exec predicate or bool constant check, then
   * instruction level predicate checks.
   *
   * Execs with the same condition are merged where possible, except across labels and after a
   * `setp` in a predicated exec. Instruction level predicate checks are merged too, and an
   * instruction with the same condition as its exec gets no `if` until a `setp`. Instructions with
   * derivatives, such as texture fetches with computed LOD, run the derivative part unpredicated
   * and predicate only the result store.
   */

  /**
   * Updates the flow control condition at exec start and in jumps, closing previous conditionals if
   * needed, then emits the pending disassembly.
   *
   * An unchanged condition leaves the instruction level predicate `if` open, which callers such as
   * jumps must check separately.
   *
   * @param type Exec instruction type.
   * @param bool_constant_index Index of the bool constant for conditional execs.
   * @param condition Value the condition must have for the exec to run.
   */
  void UpdateExecConditionalsAndEmitDisassembly(ParsedExecInstruction::Type type,
                                                u32 bool_constant_index, bool condition);
  /** Closes `if`s opened by execs and their instructions, but not by labels. */
  void CloseExecConditionals();
  /**
   * Opens or reopens the predicate check for a non control flow instruction, emitting the pending
   * disassembly between closing the previous conditional and opening the new one. Runs before
   * each such instruction is processed.
   *
   * @param predicated Whether the instruction is predicated.
   * @param condition Predicate value required for the instruction to run.
   */
  void UpdateInstructionPredicationAndEmitDisassembly(bool predicated, bool condition);
  /** Closes the instruction level predicate `if` if open. */
  void CloseInstructionPredication();
  void JumpToLabel(u32 address);

  u32 FindOrAddTextureBinding(u32 fetch_constant, xenos::FetchOpDimension dimension,
                              bool is_signed);
  u32 FindOrAddSamplerBinding(u32 fetch_constant, xenos::TextureFilter mag_filter,
                              xenos::TextureFilter min_filter, xenos::TextureFilter mip_filter,
                              xenos::AnisoFilter aniso_filter);
  /**
   * Returns the number of texture SRV and sampler offsets passed to the shader through a constant
   * buffer.
   *
   * @return Number of bindless texture and sampler offsets.
   */
  u32 GetBindlessResourceCount() const {
    return static_cast<u32>(texture_bindings_.size() + sampler_bindings_.size());
  }
  /**
   * Marks fetch constants used and returns the source for words 0 and 1 (pair 0), 2 and 3 (pair 1)
   * or 4 and 5 (pair 2) of a texture fetch constant.
   *
   * @param fetch_constant_index Index of the texture fetch constant.
   * @param pair_index Index of the word pair within the fetch constant.
   * @return Constant buffer source for the word pair.
   */
  dxbc::Src RequestTextureFetchConstantWordPair(u32 fetch_constant_index, u32 pair_index) {
    if (cbuffer_index_fetch_constants_ == kBindingIndexUnallocated) {
      cbuffer_index_fetch_constants_ = cbuffer_count_++;
    }
    u32 total_pair_index = fetch_constant_index * 3 + pair_index;
    return dxbc::Src::CB(cbuffer_index_fetch_constants_,
                         std::to_underlying(CbufferRegister::kFetchConstants),
                         total_pair_index >> 1, (total_pair_index & 1) ? 0b10101110 : 0b00000100);
  }
  dxbc::Src RequestTextureFetchConstantWord(u32 fetch_constant_index, u32 word_index) {
    return RequestTextureFetchConstantWordPair(fetch_constant_index, word_index >> 1)
        .SelectFromSwizzled(word_index & 1);
  }

  void KillPixel(bool condition, const dxbc::Src& condition_src,
                 u8 memexport_eM_potentially_written_before);

  void ProcessVectorAluOperation(const ParsedAluInstruction& instr,
                                 u8 memexport_eM_potentially_written_before, u32& result_swizzle,
                                 bool& predicate_written);
  void ProcessScalarAluOperation(const ParsedAluInstruction& instr,
                                 u8 memexport_eM_potentially_written_before,
                                 bool& predicate_written);

  void WriteResourceDefinition();
  void WriteInputSignature();
  void WritePatchConstantSignature();
  void WriteOutputSignature();
  void WriteShaderCode();

  std::vector<u32> shader_code_;
  std::vector<u32> shader_object_;

  dxbc::ShaderFeatureInfo shader_feature_info_;
  dxbc::Statistics statistics_;

  /** Must be declared after shader_code_ and statistics_ for initialization order. */
  dxbc::Assembler a_;
  /** Must be declared after shader_object_ and statistics_ for initialization order. */
  dxbc::Assembler ao_;

  StringBuffer instruction_disassembly_buffer_;

  bool emit_source_map_;

  gpu::GraphicsProvider::GPUVendorID vendor_id_;

  bool bindless_resources_used_;

  bool edram_rov_used_;

  bool gamma_render_target_as_unorm8_;

  bool msaa_2x_supported_;

  u32 draw_resolution_scale_x_;
  u32 draw_resolution_scale_y_;

  bool is_depth_only_pixel_shader_ = false;

  enum class ShaderRdefTypeIndex : u8 {
    kFloat,
    kFloat2,
    kFloat3,
    kFloat4,
    kUint,
    kUint2,
    kUint4,
    kFloat4Array4,
    kFloat4Array6,
    kFloat4ConstantArray,
    kUint4Array2,
    kUint4Array8,
    kUint4Array48,
    kUint4DescriptorIndexArray,

    kCount,
    kUnknown = kCount
  };

  struct ShaderRdefType {
    /** Ignored for arrays. */
    const char* name;
    dxbc::RdefVariableClass variable_class;
    dxbc::RdefVariableType variable_type;
    u16 row_count;
    u16 column_count;
    u16 element_count;
    ShaderRdefTypeIndex array_element_type;
  };
  static const ShaderRdefType rdef_types_[std::to_underlying(ShaderRdefTypeIndex::kCount)];

  static constexpr u32 kBindingIndexUnallocated = UINT32_MAX;

  u32 cbuffer_count_;
  u32 cbuffer_index_system_constants_;
  u32 cbuffer_index_float_constants_;
  u32 cbuffer_index_bool_loop_constants_;
  u32 cbuffer_index_fetch_constants_;
  u32 cbuffer_index_descriptor_indices_;

  struct SystemConstantRdef {
    const char* name;
    ShaderRdefTypeIndex type;
    u32 size;
    u32 padding_after = 0;
  };
  static const SystemConstantRdef
      system_constant_rdef_[std::to_underlying(SystemConstants::Index::kCount)];
  /** Mask of system constants (1 << SystemConstants::Index) used, so the rest are marked unused in
   * RDEF. */
  u64 system_constants_used_;

  u32 out_reg_vs_interpolators_;
  u32 out_reg_vs_position_;
  /** Clip and cull distances are tightly packed with up to 6 SV_ClipDistance or SV_CullDistance
   * depending on user_clip_plane_cull, then 1 SV_CullDistance when vertex_kill_and is used. */
  u32 out_reg_vs_clip_cull_distances_;
  u32 out_reg_vs_point_size_;
  u32 in_reg_ps_interpolators_;
  u32 in_reg_ps_point_coordinates_;
  u32 in_reg_ps_position_;
  /** Nointerpolation inputs. SV_IsFrontFace (X) is for non point PsParamGen, SV_SampleIndex (Y) is
   * for memexport when sample rate shading is needed for depth conversion. */
  u32 in_reg_ps_front_face_sample_index_;

  u32 in_domain_location_used_;
  bool in_control_point_index_used_;
  u32 in_position_used_;
  bool in_front_face_used_;

  u32 system_temp_count_current_;
  u32 system_temp_count_max_;

  u32 system_temp_position_;
  u32 system_temp_point_size_edge_flag_kill_vertex_;
  /**
   * ROV only, 4 persistent VGPRs when writing color targets and 2 otherwise.
   *
   * - X holds bit masks. Bits 0 to 3 are the per sample coverage at the current point of execution,
   * affected by SV_Coverage, early or late depth and stencil (failing bits are always reset) and
   * alpha to coverage. Bits 4 to 7 are the deferred depth write mask for samples whose early depth
   * and stencil result differs but cannot be written before the shader runs because kills, alpha
   * test or alpha to coverage may discard the sample. Early rejection of the pixel is possible when
   * bits 0 to 7 are all zero. Bits 8 to 11 report whether color buffers were written on the taken
   * path, and unwritten ones are not exported as in Direct3D 9.
   * - Y is the absolute resolution scaled EDRAM offset for depth and stencil in dwords before and
   * during depth testing, and the current color sample address during color writing.
   * - Z is the base relative resolution scaled EDRAM offset for 32bpp color in dwords.
   * - W is the base relative resolution scaled EDRAM offset for 64bpp color in dwords.
   */
  u32 system_temp_rov_params_;
  /**
   * When writing oDepth, X holds the shader depth, which for float24 host render targets is
   * remapped from 0...1 to 0...0.5 and converted to float24 precision, and for ROV is written at
   * the end of the shader.
   *
   * Without oDepth and with ROV and ROV_IsDepthStencilEarly it holds the new per sample depth and
   * stencil from the early test. Without ROV_IsDepthStencilEarly, .xy hold the Z gradients taken
   * before any return.
   */
  u32 system_temp_depth_stencil_;
  /** Up to 4 color outputs, readable for alpha test, alpha to coverage, exponent bias, gamma and
   * ROV writing. */
  u32 system_temps_color_[4];

  /** Allocated only when the shader writes any eM#. X is whether memexport is enabled for the
   * invocation, Y is the mask of eM# elements written since the last memory write. */
  u32 system_temp_memexport_enabled_and_eM_written_;
  u32 system_temp_memexport_address_;
  u32 system_temps_memexport_data_[xenos::ucode::kMaxMemExportElementCount];

  u32 system_temp_result_;
  u32 system_temp_ps_pc_p0_a0_;
  /** Loop index stack with the active loop in .x, shifted right to .yzw on push. */
  u32 system_temp_aL_;
  /** Loop counter stack with the active loop in .x, holding the remaining iteration count. */
  u32 system_temp_loop_count_;
  u32 system_temp_grad_h_lod_;
  /** The .w component holds `base + index * stride` in bytes from the last vfetch_full for
   * vfetch_mini. */
  u32 system_temp_grad_v_vfetch_address_;

  /** Bool constant index of the processed exec condition, or kCfExecBoolConstantNone when
   * unchecked. */
  u32 cf_exec_bool_constant_;
  static constexpr u32 kCfExecBoolConstantNone = UINT32_MAX;
  bool cf_exec_bool_constant_condition_;
  bool cf_exec_predicated_;
  bool cf_exec_predicate_condition_;
  bool cf_instruction_predicate_if_open_;
  bool cf_instruction_predicate_condition_;
  /** Set by a `setp` in the current exec, after which the instruction level predicate can differ
   * from the exec level one and execs with the same condition cannot be merged. */
  bool cf_exec_predicate_written_;

  u32 srv_count_;
  u32 srv_index_shared_memory_;
  u32 srv_index_bindless_textures_2d_;
  u32 srv_index_bindless_textures_3d_;
  u32 srv_index_bindless_textures_cube_;

  /** The first binding is at t[SRVMainRegister::kBindfulTexturesStart] of SRVSpace::kMain. */
  std::vector<TextureBinding> texture_bindings_;
  std::unordered_map<u32, u32> texture_bindings_for_bindful_srv_indices_;

  u32 uav_count_;
  u32 uav_index_shared_memory_;
  u32 uav_index_edram_;

  std::vector<SamplerBinding> sampler_bindings_;
};

}  // namespace rex::gpu
