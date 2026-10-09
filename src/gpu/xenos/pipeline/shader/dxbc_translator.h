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
class DxbcShaderTranslator : public ShaderTranslator {
 public:
  DxbcShaderTranslator(gpu::GraphicsProvider::GpuVendorID vendor_id, bool bindless_resources_used,
                       bool edram_rov_used, bool gamma_render_target_as_unorm8 = false,
                       bool msaa_2x_supported = true, uint32_t draw_resolution_scale_x = 1,
                       uint32_t draw_resolution_scale_y = 1, bool force_emit_source_map = false);
  ~DxbcShaderTranslator() override;

  union Modification {
    /** Layout version in 0xYYYYMMDD form, increased on incompatible changes to invalidate pipeline
     * storage. */
    static constexpr uint32_t kVersion = 0x20260226;

    enum class DepthStencilMode : uint32_t {
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

    uint64_t value;
    struct VertexShaderModification {
      uint32_t interpolator_mask : xenos::kMaxInterpolators;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t vertex_kill_and : 1;
      uint32_t output_point_size : 1;
      /** Value from SQ_PROGRAM_CNTL. */
      uint32_t dynamic_addressable_register_count : 8;
      /** Value of PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives. */
      uint32_t point_ps_ucp_mode : 2;
      Shader::HostVertexShaderType host_vertex_shader_type : Shader::kHostVertexShaderTypeBitCount;
    } vertex;
    struct PixelShaderModification {
      uint32_t interpolator_mask : xenos::kMaxInterpolators;
      uint32_t interpolators_centroid : xenos::kMaxInterpolators;
      uint32_t param_gen_enable : 1;
      uint32_t param_gen_interpolator : 4;
      /** Must be set for point primitives and clear otherwise when param_gen_enable is set. Enables
       * the point sprite coordinates input and changes the PsParamGen flag bits. */
      uint32_t param_gen_point : 1;
      uint32_t dynamic_addressable_register_count : 8;
      /** Used without ROV only. */
      DepthStencilMode depth_stencil_mode : 2;
    } pixel;

    explicit Modification(uint64_t modification_value = 0) : value(modification_value) {
      REX_STATIC_ASSERT_SIZE(*this, sizeof(value));
    }

    uint32_t GetVertexClipDistanceCount() const {
      return vertex.user_clip_plane_cull ? 0 : vertex.user_clip_plane_count;
    }
    uint32_t GetVertexCullDistanceCount() const {
      return (vertex.user_clip_plane_cull ? vertex.user_clip_plane_count : 0) +
             vertex.vertex_kill_and;
    }
  };

  /** Constant buffer registers in space 0. */
  enum class CbufferRegister : uint32_t {
    kSystemConstants,
    kFloatConstants,
    kBoolLoopConstants,
    kFetchConstants,
    kDescriptorIndices,
  };

  /** Some flags are referenced in xenos_draw.hlsli and must stay in sync with it. */
  enum : uint32_t {
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
    uint32_t flags;
    union {
      struct {
        float tessellation_factor_range_min;
        float tessellation_factor_range_max;
      };
      float tessellation_factor_range[2];
    };
    uint32_t line_loop_closing_index;

    xenos::Endian vertex_index_endian;
    uint32_t vertex_index_offset;
    union {
      struct {
        uint32_t vertex_index_min;
        uint32_t vertex_index_max;
      };
      uint32_t vertex_index_min_max[2];
    };

    float user_clip_planes[6][4];

    float ndc_scale[3];
    float point_vertex_diameter_min;

    float ndc_offset[3];
    float point_vertex_diameter_max;

    float point_constant_diameter[2];
    /** Converts diameter in guest screen coordinates to radius in NDC for the host viewport. */
    float point_screen_diameter_to_ndc_radius[2];

    /** Post swizzle TextureSign values, 1 byte per needed component of each of the 32 texture fetch
     * constants. */
    uint32_t texture_swizzled_signs[8];

    uint32_t textures_resolution_scaled;
    /** Log2 of the X and Y sample size, used for alpha to mask and for EDRAM addressing with ROV
     * MSAA. */
    uint32_t sample_count_log2[2];
    float alpha_test_reference;

    /** 0 when alpha to mask is disabled. When enabled, bits 0 to 7 are sample offsets and bit 8 is
     * set. */
    uint32_t alpha_to_mask;
    uint32_t edram_32bpp_tile_pitch_dwords_scaled;
    uint32_t edram_depth_base_dwords_scaled;
    uint32_t padding_edram_depth_base_dwords_scaled;

    float color_exp_bias[4];

    union {
      struct {
        float edram_poly_offset_front_scale;
        float edram_poly_offset_front_offset;
      };
      float edram_poly_offset_front[2];
    };
    union {
      struct {
        float edram_poly_offset_back_scale;
        float edram_poly_offset_back_offset;
      };
      float edram_poly_offset_back[2];
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
        uint32_t edram_stencil_front_reference;
        uint32_t edram_stencil_front_read_mask;
        uint32_t edram_stencil_front_write_mask;
        uint32_t edram_stencil_front_func_ops;

        uint32_t edram_stencil_back_reference;
        uint32_t edram_stencil_back_read_mask;
        uint32_t edram_stencil_back_write_mask;
        uint32_t edram_stencil_back_func_ops;
      };
      struct {
        uint32_t edram_stencil_front[4];
        uint32_t edram_stencil_back[4];
      };
      uint32_t edram_stencil[2][4];
    };

    uint32_t edram_rt_base_dwords_scaled[4];

    /** Render target format combined with RenderTargetCache::kPSIColorFormatFlag values from
     * RenderTargetCache::AddPSIColorFormatFlags. */
    uint32_t edram_rt_format_flags[4];

    /** Color clamp values applied before blending or storing, ordered low color, low alpha, high
     * color, high alpha. */
    float edram_rt_clamp[4][4];

    /** Mask applied to the old packed render target data and, inverted, to the new packed data
     * before storing. When 0 and not blending, reading the old data from EDRAM is not required. */
    uint32_t edram_rt_keep_mask[4][2];

    /** RB_BLENDCONTROL factors and operations masked with 0x1FFF1FFF. A value of 0x00010001 (1 *
     * src + 0 * dst) disables blending for the render target. */
    uint32_t edram_rt_blend_factors_ops[4];

    float edram_blend_constant[4];

   private:
    friend class DxbcShaderTranslator;

    enum class Index : uint32_t {
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
  enum class SRVSpace : uint32_t {
    /** Uses the SRVMainRegister t# layout. */
    kMain,
    kBindlessTextures2DArray,
    kBindlessTextures3D,
    kBindlessTexturesCube,
  };

  /** Shader resource view registers in SRVSpace::kMain. */
  enum class SRVMainRegister : uint32_t {
    kSharedMemory,
    kBindfulTexturesStart,
  };

  /** At most 192 textures, from 32 fetch constants times 2D array, 3D or cube dimensions and signed
   * or unsigned. */
  static constexpr uint32_t kMaxTextureBindingIndexBits = 8;
  static constexpr uint32_t kMaxTextureBindings = (1 << kMaxTextureBindingIndexBits) - 1;
  struct TextureBinding {
    uint32_t bindful_srv_index;
    uint32_t bindful_srv_rdef_name_ptr;
    uint32_t bindless_descriptor_index;
    uint32_t fetch_constant;
    xenos::FetchOpDimension dimension;
    bool is_signed;
    std::string bindful_name;
  };

  /**
   * Limit of sampler bindings so the count packs into 32 bit map keys such as root signature keys.
   */
  static constexpr uint32_t kMaxSamplerBindingIndexBits = 7;
  static constexpr uint32_t kMaxSamplerBindings = (1 << kMaxSamplerBindingIndexBits) - 1;
  struct SamplerBinding {
    uint32_t bindless_descriptor_index;
    uint32_t fetch_constant;
    xenos::TextureFilter mag_filter;
    xenos::TextureFilter min_filter;
    xenos::TextureFilter mip_filter;
    xenos::AnisoFilter aniso_filter;
    std::string bindful_name;
  };

  /** Unordered access view registers in space 0. */
  enum class UAVRegister : uint32_t {
    kSharedMemory,
    kEDRAM,
  };

  uint64_t GetDefaultVertexShaderModification(
      uint32_t dynamic_addressable_register_count,
      Shader::HostVertexShaderType host_vertex_shader_type =
          Shader::HostVertexShaderType::kVertex) const override;
  uint64_t GetDefaultPixelShaderModification(
      uint32_t dynamic_addressable_register_count) const override;

  /**
   * Creates a pixel shader without color outputs.
   *
   * Resets the translator state.
   *
   * @return The translated shader binary.
   */
  std::vector<uint8_t> CreateDepthOnlyPixelShader();

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
  static void PreClampedFloat32To7e3(dxbc::Assembler& a, uint32_t f10_temp,
                                     uint32_t f10_temp_component, uint32_t f32_temp,
                                     uint32_t f32_temp_component, uint32_t temp_temp,
                                     uint32_t temp_temp_component);
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
  static void UnclampedFloat32To7e3(dxbc::Assembler& a, uint32_t f10_temp,
                                    uint32_t f10_temp_component, uint32_t f32_temp,
                                    uint32_t f32_temp_component, uint32_t temp_temp,
                                    uint32_t temp_temp_component);
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
  static void Float7e3To32(dxbc::Assembler& a, const dxbc::Dest& f32, uint32_t f10_temp,
                           uint32_t f10_temp_component, uint32_t f10_shift, uint32_t temp1_temp,
                           uint32_t temp1_temp_component, uint32_t temp2_temp,
                           uint32_t temp2_temp_component);
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
  static void PreClampedDepthTo20e4(dxbc::Assembler& a, uint32_t f24_temp,
                                    uint32_t f24_temp_component, uint32_t f32_temp,
                                    uint32_t f32_temp_component, uint32_t temp_temp,
                                    uint32_t temp_temp_component, bool round_to_nearest_even,
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
  static void Depth20e4To32(dxbc::Assembler& a, const dxbc::Dest& f32, uint32_t f24_temp,
                            uint32_t f24_temp_component, uint32_t f24_shift, uint32_t temp1_temp,
                            uint32_t temp1_temp_component, uint32_t temp2_temp,
                            uint32_t temp2_temp_component, bool remap_to_0_to_0_5);

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
  static void PWLGammaToLinear(dxbc::Assembler& a, uint32_t target_temp,
                               uint32_t target_temp_component, uint32_t source_temp,
                               uint32_t source_temp_component, bool source_pre_saturated,
                               uint32_t temp1, uint32_t temp1_component, uint32_t temp2,
                               uint32_t temp2_component);
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
  static void PreSaturatedLinearToPWLGamma(dxbc::Assembler& a, uint32_t target_temp,
                                           uint32_t target_temp_component, uint32_t source_temp,
                                           uint32_t source_temp_component, uint32_t temp_or_target,
                                           uint32_t temp_or_target_component,
                                           uint32_t temp_non_target,
                                           uint32_t temp_non_target_component);

 protected:
  void Reset() override;

  uint32_t GetModificationRegisterCount() const override;

  void StartTranslation() override;
  std::vector<uint8_t> CompleteTranslation() override;
  void PostTranslation() override;

  void ProcessLabel(uint32_t cf_index) override;

  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr, uint8_t export_eM) override;

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
                             uint8_t memexport_eM_potentially_written_before) override;

 private:
  /** Changing these requires updating WriteInputSignature and WriteOutputSignature. */
  static constexpr uint32_t kInRegisterVSVertexIndex = 0;
  static constexpr uint32_t kInRegisterDSControlPointIndex = 0;

  /**
   * Marks a system constant as used. Together with GetSystemConstantSrc it builds a source
   * unconditionally, otherwise use LoadSystemConstant.
   *
   * @param index System constant to mark.
   */
  void MarkSystemConstantUsed(SystemConstants::Index index) {
    system_constants_used_ |= static_cast<uint64_t>(1) << uint32_t(index);
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
  dxbc::Src GetSystemConstantSrc(size_t offset, uint32_t swizzle) const {
    uint32_t first_component = static_cast<uint32_t>((offset >> 2) & 3);
    return dxbc::Src::CB(
        cbuffer_index_system_constants_, std::to_underlying(CbufferRegister::kSystemConstants),
        static_cast<uint32_t>(offset >> 4),
        std::min((swizzle & 3) + first_component, static_cast<uint32_t>(3)) |
            std::min(((swizzle >> 2) & 3) + first_component, static_cast<uint32_t>(3)) << 2 |
            std::min(((swizzle >> 4) & 3) + first_component, static_cast<uint32_t>(3)) << 4 |
            std::min(((swizzle >> 6) & 3) + first_component, static_cast<uint32_t>(3)) << 6);
  }
  dxbc::Src LoadSystemConstant(SystemConstants::Index index, size_t offset, uint32_t swizzle) {
    MarkSystemConstantUsed(index);
    return GetSystemConstantSrc(offset, swizzle);
  }
  dxbc::Src LoadFlagsSystemConstant() {
    return LoadSystemConstant(SystemConstants::Index::kFlags, offsetof(SystemConstants, flags),
                              dxbc::Src::kXXXX);
  }

  Modification GetDxbcShaderModification() const {
    return Modification(current_translation().modification());
  }

  bool IsDxbcVertexShader() const {
    return is_vertex_shader() && !Shader::IsHostVertexShaderTypeDomain(
                                     GetDxbcShaderModification().vertex.host_vertex_shader_type);
  }
  bool IsDxbcDomainShader() const {
    return is_vertex_shader() && Shader::IsHostVertexShaderTypeDomain(
                                     GetDxbcShaderModification().vertex.host_vertex_shader_type);
  }

  bool IsForceEarlyDepthStencilGlobalFlagEnabled() const {
    return is_pixel_shader() &&
           GetDxbcShaderModification().pixel.depth_stencil_mode ==
               Modification::DepthStencilMode::kEarlyHint &&
           !edram_rov_used_ && current_shader().implicit_early_z_write_allowed();
  }

  uint32_t GetModificationInterpolatorMask() const {
    Modification modification = GetDxbcShaderModification();
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
  uint32_t PushSystemTemp(uint32_t zero_mask = 0, uint32_t count = 1);
  /**
   * Frees the last allocated internal r# registers for reuse.
   *
   * @param count Number of registers to free.
   */
  void PopSystemTemp(uint32_t count = 1);

  /**
   * Exports eM# to memory. Modifies eA and eM# values, so call only before starting a new export or
   * ending or deactivating the invocation.
   *
   * @param export_eM Mask of eM# registers to export.
   */
  void ExportToMemory(uint8_t export_eM);

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
        GetDxbcShaderModification().pixel.depth_stencil_mode;
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
  void ROV_DepthTo24Bit(uint32_t d24_temp, uint32_t d24_temp_component, uint32_t d32_temp,
                        uint32_t d32_temp_component, uint32_t temp_temp,
                        uint32_t temp_temp_component);
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
  void ROV_UnpackColor(uint32_t rt_index, uint32_t packed_temp, uint32_t packed_temp_components,
                       uint32_t color_temp, uint32_t temp1, uint32_t temp1_component,
                       uint32_t temp2, uint32_t temp2_component);
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
  void ROV_PackPreClampedColor(uint32_t rt_index, uint32_t color_temp, uint32_t packed_temp,
                               uint32_t packed_temp_components, uint32_t temp1,
                               uint32_t temp1_component, uint32_t temp2, uint32_t temp2_component);
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
  void ROV_HandleColorBlendFactorCases(uint32_t src_temp, uint32_t dst_temp, uint32_t factor_temp);
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
  void ROV_HandleAlphaBlendFactorCases(uint32_t src_temp, uint32_t dst_temp, uint32_t factor_temp,
                                       uint32_t factor_component);

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
  void RemapAndConvertVertexIndices(uint32_t dest_temp, uint32_t dest_temp_components,
                                    const dxbc::Src& src);
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
  void CompletePixelShader_AlphaToMaskSample(bool initialize, uint32_t sample_index,
                                             float threshold_base, dxbc::Src threshold_offset,
                                             float threshold_offset_scale, uint32_t coverage_temp,
                                             uint32_t coverage_temp_component, uint32_t temp,
                                             uint32_t temp_component);
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
  dxbc::Src LoadOperand(const InstructionOperand& operand, uint32_t needed_components,
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
                                                uint32_t bool_constant_index, bool condition);
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
  void JumpToLabel(uint32_t address);

  uint32_t FindOrAddTextureBinding(uint32_t fetch_constant, xenos::FetchOpDimension dimension,
                                   bool is_signed);
  uint32_t FindOrAddSamplerBinding(uint32_t fetch_constant, xenos::TextureFilter mag_filter,
                                   xenos::TextureFilter min_filter, xenos::TextureFilter mip_filter,
                                   xenos::AnisoFilter aniso_filter);
  /**
   * Returns the number of texture SRV and sampler offsets passed to the shader through a constant
   * buffer.
   *
   * @return Number of bindless texture and sampler offsets.
   */
  uint32_t GetBindlessResourceCount() const {
    return static_cast<uint32_t>(texture_bindings_.size() + sampler_bindings_.size());
  }
  /**
   * Marks fetch constants used and returns the source for words 0 and 1 (pair 0), 2 and 3 (pair 1)
   * or 4 and 5 (pair 2) of a texture fetch constant.
   *
   * @param fetch_constant_index Index of the texture fetch constant.
   * @param pair_index Index of the word pair within the fetch constant.
   * @return Constant buffer source for the word pair.
   */
  dxbc::Src RequestTextureFetchConstantWordPair(uint32_t fetch_constant_index,
                                                uint32_t pair_index) {
    if (cbuffer_index_fetch_constants_ == kBindingIndexUnallocated) {
      cbuffer_index_fetch_constants_ = cbuffer_count_++;
    }
    uint32_t total_pair_index = fetch_constant_index * 3 + pair_index;
    return dxbc::Src::CB(cbuffer_index_fetch_constants_,
                         std::to_underlying(CbufferRegister::kFetchConstants),
                         total_pair_index >> 1, (total_pair_index & 1) ? 0b10101110 : 0b00000100);
  }
  dxbc::Src RequestTextureFetchConstantWord(uint32_t fetch_constant_index, uint32_t word_index) {
    return RequestTextureFetchConstantWordPair(fetch_constant_index, word_index >> 1)
        .SelectFromSwizzled(word_index & 1);
  }

  void KillPixel(bool condition, const dxbc::Src& condition_src,
                 uint8_t memexport_eM_potentially_written_before);

  void ProcessVectorAluOperation(const ParsedAluInstruction& instr,
                                 uint8_t memexport_eM_potentially_written_before,
                                 uint32_t& result_swizzle, bool& predicate_written);
  void ProcessScalarAluOperation(const ParsedAluInstruction& instr,
                                 uint8_t memexport_eM_potentially_written_before,
                                 bool& predicate_written);

  void WriteResourceDefinition();
  void WriteInputSignature();
  void WritePatchConstantSignature();
  void WriteOutputSignature();
  void WriteShaderCode();

  std::vector<uint32_t> shader_code_;
  std::vector<uint32_t> shader_object_;

  dxbc::ShaderFeatureInfo shader_feature_info_;
  dxbc::Statistics statistics_;

  /** Must be declared after shader_code_ and statistics_ for initialization order. */
  dxbc::Assembler a_;
  /** Must be declared after shader_object_ and statistics_ for initialization order. */
  dxbc::Assembler ao_;

  StringBuffer instruction_disassembly_buffer_;

  bool emit_source_map_;

  gpu::GraphicsProvider::GpuVendorID vendor_id_;

  bool bindless_resources_used_;

  bool edram_rov_used_;

  bool gamma_render_target_as_unorm8_;

  bool msaa_2x_supported_;

  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  bool is_depth_only_pixel_shader_ = false;

  enum class ShaderRdefTypeIndex : uint32_t {
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
    uint16_t row_count;
    uint16_t column_count;
    uint16_t element_count;
    ShaderRdefTypeIndex array_element_type;
  };
  static const ShaderRdefType rdef_types_[std::to_underlying(ShaderRdefTypeIndex::kCount)];

  static constexpr uint32_t kBindingIndexUnallocated = UINT32_MAX;

  uint32_t cbuffer_count_;
  uint32_t cbuffer_index_system_constants_;
  uint32_t cbuffer_index_float_constants_;
  uint32_t cbuffer_index_bool_loop_constants_;
  uint32_t cbuffer_index_fetch_constants_;
  uint32_t cbuffer_index_descriptor_indices_;

  struct SystemConstantRdef {
    const char* name;
    ShaderRdefTypeIndex type;
    uint32_t size;
    uint32_t padding_after = 0;
  };
  static const SystemConstantRdef
      system_constant_rdef_[std::to_underlying(SystemConstants::Index::kCount)];
  /** Mask of system constants (1 << SystemConstants::Index) used, so the rest are marked unused in
   * RDEF. */
  uint64_t system_constants_used_;

  uint32_t out_reg_vs_interpolators_;
  uint32_t out_reg_vs_position_;
  /** Clip and cull distances are tightly packed with up to 6 SV_ClipDistance or SV_CullDistance
   * depending on user_clip_plane_cull, then 1 SV_CullDistance when vertex_kill_and is used. */
  uint32_t out_reg_vs_clip_cull_distances_;
  uint32_t out_reg_vs_point_size_;
  uint32_t in_reg_ps_interpolators_;
  uint32_t in_reg_ps_point_coordinates_;
  uint32_t in_reg_ps_position_;
  /** Nointerpolation inputs. SV_IsFrontFace (X) is for non point PsParamGen, SV_SampleIndex (Y) is
   * for memexport when sample rate shading is needed for depth conversion. */
  uint32_t in_reg_ps_front_face_sample_index_;

  uint32_t in_domain_location_used_;
  bool in_control_point_index_used_;
  uint32_t in_position_used_;
  bool in_front_face_used_;

  uint32_t system_temp_count_current_;
  uint32_t system_temp_count_max_;

  uint32_t system_temp_position_;
  uint32_t system_temp_point_size_edge_flag_kill_vertex_;
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
  uint32_t system_temp_rov_params_;
  /**
   * When writing oDepth, X holds the shader depth, which for float24 host render targets is
   * remapped from 0...1 to 0...0.5 and converted to float24 precision, and for ROV is written at
   * the end of the shader.
   *
   * Without oDepth and with ROV and ROV_IsDepthStencilEarly it holds the new per sample depth and
   * stencil from the early test. Without ROV_IsDepthStencilEarly, .xy hold the Z gradients taken
   * before any return.
   */
  uint32_t system_temp_depth_stencil_;
  /** Up to 4 color outputs, readable for alpha test, alpha to coverage, exponent bias, gamma and
   * ROV writing. */
  uint32_t system_temps_color_[4];

  /** Allocated only when the shader writes any eM#. X is whether memexport is enabled for the
   * invocation, Y is the mask of eM# elements written since the last memory write. */
  uint32_t system_temp_memexport_enabled_and_eM_written_;
  uint32_t system_temp_memexport_address_;
  uint32_t system_temps_memexport_data_[xenos::ucode::kMaxMemExportElementCount];

  uint32_t system_temp_result_;
  uint32_t system_temp_ps_pc_p0_a0_;
  /** Loop index stack with the active loop in .x, shifted right to .yzw on push. */
  uint32_t system_temp_aL_;
  /** Loop counter stack with the active loop in .x, holding the remaining iteration count. */
  uint32_t system_temp_loop_count_;
  uint32_t system_temp_grad_h_lod_;
  /** The .w component holds `base + index * stride` in bytes from the last vfetch_full for
   * vfetch_mini. */
  uint32_t system_temp_grad_v_vfetch_address_;

  /** Bool constant index of the processed exec condition, or kCfExecBoolConstantNone when
   * unchecked. */
  uint32_t cf_exec_bool_constant_;
  static constexpr uint32_t kCfExecBoolConstantNone = UINT32_MAX;
  bool cf_exec_bool_constant_condition_;
  bool cf_exec_predicated_;
  bool cf_exec_predicate_condition_;
  bool cf_instruction_predicate_if_open_;
  bool cf_instruction_predicate_condition_;
  /** Set by a `setp` in the current exec, after which the instruction level predicate can differ
   * from the exec level one and execs with the same condition cannot be merged. */
  bool cf_exec_predicate_written_;

  uint32_t srv_count_;
  uint32_t srv_index_shared_memory_;
  uint32_t srv_index_bindless_textures_2d_;
  uint32_t srv_index_bindless_textures_3d_;
  uint32_t srv_index_bindless_textures_cube_;

  /** The first binding is at t[SRVMainRegister::kBindfulTexturesStart] of SRVSpace::kMain. */
  std::vector<TextureBinding> texture_bindings_;
  std::unordered_map<uint32_t, uint32_t> texture_bindings_for_bindful_srv_indices_;

  uint32_t uav_count_;
  uint32_t uav_index_shared_memory_;
  uint32_t uav_index_edram_;

  std::vector<SamplerBinding> sampler_bindings_;
};

}  // namespace rex::gpu
