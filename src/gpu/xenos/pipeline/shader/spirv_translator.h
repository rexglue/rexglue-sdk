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

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu/xenos/pipeline/shader/spirv_builder.h"
#include "gpu/xenos/pipeline/shader/translator.h"
#include <rex/xenos/xenos.h>
#include "gpu/xenos/host/vulkan/device.h"

namespace rex::gpu {

class SpirvShaderTranslator : public ShaderTranslator {
 public:
  union Modification {
    /** Pipeline storage version, incremented when the layout changes incompatibly. */
    static constexpr uint32_t kVersion = 12;

    enum class DepthStencilMode : uint32_t {
      kNoModifiers,
      /**
       * Early fragment tests, enabled when alpha test and alpha to coverage are disabled.
       * Ignored when the shader blocks early Z writes.
       */
      kEarlyHint,
      /**
       * Remaps the guest [0, 1] depth range to the host [0, 0.5] range, as in DXBC float24
       * conversion.
       */
      kFloat24Truncating,
      kFloat24Rounding,
    };

    struct {
      uint32_t interpolator_mask : xenos::kMaxInterpolators;
      /**
       * Outputs the point coordinates for kPointListAsTriangleStrip and the point size for other
       * host vertex shader types.
       */
      uint32_t output_point_parameters : 1;
      /** From SQ_PROGRAM_CNTL. */
      uint32_t dynamic_addressable_register_count : 8;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t vertex_kill_and : 1;
      /** PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives. */
      uint32_t point_ps_ucp_mode : 2;
      Shader::HostVertexShaderType host_vertex_shader_type : Shader::kHostVertexShaderTypeBitCount;
      /** Domain host vertex shader types only. */
      uint32_t tessellation_mode : 2;
    } vertex;
    struct PixelShaderModification {
      uint32_t interpolator_mask : xenos::kMaxInterpolators;
      uint32_t interpolators_centroid : xenos::kMaxInterpolators;
      /** From SQ_PROGRAM_CNTL. */
      uint32_t dynamic_addressable_register_count : 8;
      uint32_t param_gen_enable : 1;
      uint32_t param_gen_interpolator : 4;
      /** Set for point primitives and clear for others when param_gen_enable is set. */
      uint32_t param_gen_point : 1;
      DepthStencilMode depth_stencil_mode : 3;
    } pixel;
    uint64_t value = 0;

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

  enum : uint32_t {
    kSysFlag_VertexIndexLoad_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexReset_Shift,
    /** Memexport compute fallback only. */
    kSysFlag_ComputeMemExportPatchIndexFromInvocation_Shift,
    /** Memexport compute fallback only. */
    kSysFlag_ComputeMemExportPatchIndexInRegister1_Shift,
    /** Memexport compute fallback only. */
    kSysFlag_ComputeMemExportTriangleCPIndexed_Shift,
    /** Memexport compute fallback only. */
    kSysFlag_ComputeMemExportQuadCPIndexed_Shift,
    kSysFlag_XYDividedByW_Shift,
    kSysFlag_ZDividedByW_Shift,
    kSysFlag_WNotReciprocal_Shift,
    kSysFlag_PrimitivePolygonal_Shift,
    kSysFlag_PrimitiveLine_Shift,
    kSysFlag_MsaaSamples_Shift,
    kSysFlag_DepthFloat24_Shift = kSysFlag_MsaaSamples_Shift + xenos::kMSAASamplesBits,
    kSysFlag_AlphaPassIfLess_Shift,
    kSysFlag_AlphaPassIfEqual_Shift,
    kSysFlag_AlphaPassIfGreater_Shift,
    kSysFlag_ConvertColor0ToGamma_Shift,
    kSysFlag_ConvertColor1ToGamma_Shift,
    kSysFlag_ConvertColor2ToGamma_Shift,
    kSysFlag_ConvertColor3ToGamma_Shift,

    kSysFlag_FSIDepthStencil_Shift,
    kSysFlag_FSIDepthPassIfLess_Shift,
    kSysFlag_FSIDepthPassIfEqual_Shift,
    kSysFlag_FSIDepthPassIfGreater_Shift,
    /** 1 writes the new depth when the depth test passes, 0 keeps the old depth. */
    kSysFlag_FSIDepthWrite_Shift,
    kSysFlag_FSIStencilTest_Shift,
    /**
     * Writes a stencil value that differs from the depth buffer after a failed depth and stencil
     * test, skipping the rest of the shader. Ignored by shaders with kill instructions or depth
     * output.
     */
    kSysFlag_FSIDepthStencilEarlyWrite_Shift,

    kSysFlag_Count,

    /**
     * kVertex only, when fullDrawIndexUint32 is unsupported. Fetches 32-bit indices manually,
     * 16-bit indices always come from the Vulkan index buffer.
     */
    kSysFlag_VertexIndexLoad = 1u << kSysFlag_VertexIndexLoad_Shift,
    /**
     * kMemExportCompute, kPointListAsTriangleStrip and kRectangleListAsTriangleStrip. Loads the
     * vertex index from the index buffer and flags whether it is 32-bit. Separate from
     * kSysFlag_VertexIndexLoad.
     */
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad =
        1u << kSysFlag_ComputeOrPrimitiveVertexIndexLoad_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit =
        1u << kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexReset =
        1u << kSysFlag_ComputeOrPrimitiveVertexIndexReset_Shift,
    kSysFlag_ComputeMemExportPatchIndexFromInvocation =
        1u << kSysFlag_ComputeMemExportPatchIndexFromInvocation_Shift,
    kSysFlag_ComputeMemExportPatchIndexInRegister1 =
        1u << kSysFlag_ComputeMemExportPatchIndexInRegister1_Shift,
    kSysFlag_ComputeMemExportTriangleCPIndexed =
        1u << kSysFlag_ComputeMemExportTriangleCPIndexed_Shift,
    kSysFlag_ComputeMemExportQuadCPIndexed = 1u << kSysFlag_ComputeMemExportQuadCPIndexed_Shift,
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
    kSysFlag_FSIDepthStencil = 1u << kSysFlag_FSIDepthStencil_Shift,
    kSysFlag_FSIDepthPassIfLess = 1u << kSysFlag_FSIDepthPassIfLess_Shift,
    kSysFlag_FSIDepthPassIfEqual = 1u << kSysFlag_FSIDepthPassIfEqual_Shift,
    kSysFlag_FSIDepthPassIfGreater = 1u << kSysFlag_FSIDepthPassIfGreater_Shift,
    kSysFlag_FSIDepthWrite = 1u << kSysFlag_FSIDepthWrite_Shift,
    kSysFlag_FSIStencilTest = 1u << kSysFlag_FSIStencilTest_Shift,
    kSysFlag_FSIDepthStencilEarlyWrite = 1u << kSysFlag_FSIDepthStencilEarlyWrite_Shift,
  };
  static_assert(kSysFlag_Count <= 32, "Too many flags in the system constants");

  /** Dispatch size for the Vulkan vertex memexport compute fallback. */
  static constexpr uint32_t kMemExportComputeDispatchX = 65535;
  static constexpr uint32_t kMemExportComputeDispatchY = 65535;

  /**
   * System constants in the std140 layout, vec2 aligned to 8 bytes and vec3 and vec4 to 16.
   * SystemConstantIndex and the BeginTranslation members must match.
   */
  struct SystemConstants {
    uint32_t flags;
    uint32_t vertex_index_load_address;
    xenos::Endian vertex_index_endian;
    /** For non indexed line loop draws, the host vertex index that maps to guest vertex 0. */
    uint32_t line_loop_closing_index;
    int32_t vertex_base_index;
    uint32_t vertex_index_reset;
    uint32_t compute_memexport_vertex_count;
    float user_clip_planes[6][4];

    float ndc_scale[3];
    float point_vertex_diameter_min;

    float ndc_offset[3];
    float point_vertex_diameter_max;

    float point_constant_diameter[2];
    /** Diameter in guest screen coordinates to radius in NDC for the host viewport. */
    float point_screen_diameter_to_ndc_radius[2];
    /** One bit per texture fetch constant that references a draw resolution scaled texture. */
    uint32_t textures_resolution_scaled;

    /**
     * Post swizzle TextureSign per byte for each needed component of the 32 used texture
     * fetch constants.
     */
    uint32_t texture_swizzled_signs[8];

    /**
     * Component swizzle applied in the shader when imageViewFormatSwizzle is unsupported,
     * combining guest and host swizzles. 2 texture fetch constants per uint32_t in bits 0:11
     * and 12:23.
     */
    uint32_t texture_swizzles[16];

    float alpha_test_reference;
    /**
     * Bits 0:7 are the alpha to coverage dither offsets from RB_COLORCONTROL bits 24:31, bit 8
     * enables alpha to coverage.
     */
    uint32_t alpha_to_mask;
    uint32_t edram_32bpp_tile_pitch_dwords_scaled;
    uint32_t edram_depth_base_dwords_scaled;

    float color_exp_bias[4];

    float edram_poly_offset_front_scale;
    float edram_poly_offset_back_scale;
    float edram_poly_offset_front_offset;
    float edram_poly_offset_back_offset;

    union {
      struct {
        uint32_t edram_stencil_front_reference_masks;
        uint32_t edram_stencil_front_func_ops;

        uint32_t edram_stencil_back_reference_masks;
        uint32_t edram_stencil_back_func_ops;
      };
      struct {
        uint32_t edram_stencil_front[2];
        uint32_t edram_stencil_back[2];
      };
    };

    uint32_t edram_rt_base_dwords_scaled[4];

    /** RT format combined with RenderTargetCache::kPSIColorFormatFlag values. */
    uint32_t edram_rt_format_flags[4];

    /**
     * RB_BLENDCONTROL factors and operations masked with 0x1FFF1FFF. 0x00010001
     * (1 * src + 0 * dst) disables blending for the render target.
     */
    uint32_t edram_rt_blend_factors_ops[4];

    /**
     * Mask applied to the old packed RT data and, inverted, to the new packed data before
     * storing. Zero without blending means the old EDRAM data need not be read.
     */
    uint32_t edram_rt_keep_mask[4][2];

    /**
     * Color clamp bounds before blending or storing, ordered low color, low alpha, high color,
     * high alpha.
     */
    float edram_rt_clamp[4][4];

    float edram_blend_constant[4];

    uint32_t vertex_index_min;
    uint32_t vertex_index_max;
    float tessellation_factor_range_min;
    float tessellation_factor_range_max;
  };

  enum ConstantBuffer : uint32_t {
    kConstantBufferSystem,
    kConstantBufferFloatVertex,
    kConstantBufferFloatPixel,
    kConstantBufferBoolLoop,
    kConstantBufferFetch,

    kConstantBufferCount,
  };

  /**
   * maxPerStageDescriptorStorageBuffers has a minimum of 4 and maxStorageBufferRange a minimum
   * of 128 MB, so SSBOs hold only shared memory and all other storage resources are images or
   * texel buffers. Sets with an immutable layout come first, in reverse frequency of change,
   * followed by sets that vary per pipeline state.
   */
  enum DescriptorSet : uint32_t {

    kDescriptorSetSharedMemoryAndEdram,
    kDescriptorSetConstants,

    kDescriptorSetMutableLayoutsStart,

    kDescriptorSetTexturesVertex = kDescriptorSetMutableLayoutsStart,
    kDescriptorSetTexturesPixel,

    kDescriptorSetCount,
  };
  static_assert(kDescriptorSetCount <= 4,
                "The number of descriptor sets used by translated shaders must be within "
                "the minimum Vulkan maxBoundDescriptorSets requirement of 4, which is "
                "the limit on most GPUs used in Android devices - Arm Mali, Imagination "
                "PowerVR, Qualcomm Adreno 6xx and older, as well as on old PC Nvidia "
                "drivers");

  /** SPIR-V generator magic tool ID of the ReXGlue Emulator Microcode Translator. */
  static constexpr uint32_t kSpirvMagicToolId = 26;

  struct Features {
    explicit Features(const gpu::vulkan::VulkanDevice* vulkan_device);
    explicit Features(bool all = false);

    unsigned int spirv_version;

    uint32_t max_storage_buffer_range;

    bool full_draw_index_uint32;

    bool vertex_pipeline_stores_and_atomics;
    bool fragment_stores_and_atomics;

    bool clip_distance;
    bool cull_distance;

    bool image_view_format_swizzle;

    bool signed_zero_inf_nan_preserve_float32;
    bool denorm_flush_to_zero_float32;
    bool rounding_mode_rte_float32;

    bool fragment_shader_sample_interlock;

    bool demote_to_helper_invocation;
    bool sample_rate_shading;
  };

  SpirvShaderTranslator(const Features& features, bool native_2x_msaa_with_attachments,
                        bool native_2x_msaa_no_attachments, bool edram_fragment_shader_interlock,
                        uint32_t draw_resolution_scale_x = 1, uint32_t draw_resolution_scale_y = 1)
      : features_(features),
        native_2x_msaa_with_attachments_(native_2x_msaa_with_attachments),
        native_2x_msaa_no_attachments_(native_2x_msaa_no_attachments),
        edram_fragment_shader_interlock_(edram_fragment_shader_interlock),
        draw_resolution_scale_x_(draw_resolution_scale_x ? draw_resolution_scale_x : 1),
        draw_resolution_scale_y_(draw_resolution_scale_y ? draw_resolution_scale_y : 1) {}

  uint64_t GetDefaultVertexShaderModification(
      uint32_t dynamic_addressable_register_count,
      Shader::HostVertexShaderType host_vertex_shader_type =
          Shader::HostVertexShaderType::kVertex) const override;
  uint64_t GetDefaultPixelShaderModification(
      uint32_t dynamic_addressable_register_count) const override;

  static constexpr uint32_t GetSharedMemoryStorageBufferCountLog2(
      uint32_t max_storage_buffer_range) {
    if (max_storage_buffer_range >= 512 * 1024 * 1024) {
      return 0;
    }
    if (max_storage_buffer_range >= 256 * 1024 * 1024) {
      return 1;
    }
    return 2;
  }
  uint32_t GetSharedMemoryStorageBufferCountLog2() const {
    return GetSharedMemoryStorageBufferCountLog2(features_.max_storage_buffer_range);
  }

  /**
   * Creates a fragment shader without color outputs and resets the translator state.
   *
   * @param depth_stencil_mode Depth and stencil output mode.
   * @return The SPIR-V words of the shader.
   */
  std::vector<uint8_t> CreateDepthOnlyFragmentShader(
      Modification::DepthStencilMode depth_stencil_mode =
          Modification::DepthStencilMode::kNoModifiers);

  /**
   * Converts a color value clamped to [0, 31.875] to 7e3 float with zeros in bits 10:31,
   * rounding to nearest even.
   *
   * @param builder Builder emitting the instructions.
   * @param f32_scalar Clamped 32 bit float scalar.
   * @param ext_inst_glsl_std_450 Id of the imported GLSL.std.450 instruction set.
   * @return Id of the 7e3 value as a uint.
   */
  static spv::Id PreClampedFloat32To7e3(SpirvBuilder& builder, spv::Id f32_scalar,
                                        spv::Id ext_inst_glsl_std_450);
  /**
   * As PreClampedFloat32To7e3, but clamps the input to [0, 31.875].
   *
   * @param builder Builder emitting the instructions.
   * @param f32_scalar 32 bit float scalar.
   * @param ext_inst_glsl_std_450 Id of the imported GLSL.std.450 instruction set.
   * @return Id of the 7e3 value as a uint.
   */
  static spv::Id UnclampedFloat32To7e3(SpirvBuilder& builder, spv::Id f32_scalar,
                                       spv::Id ext_inst_glsl_std_450);
  /**
   * Converts the 7e3 number in bits [f10_shift, f10_shift + 10) to a 32 bit float.
   *
   * @param builder Builder emitting the instructions.
   * @param f10_uint_scalar Uint scalar holding the 7e3 number.
   * @param f10_shift Bit offset of the 7e3 number.
   * @param result_as_uint True to return the float bits as a uint.
   * @param ext_inst_glsl_std_450 Id of the imported GLSL.std.450 instruction set.
   * @return Id of the converted value.
   */
  static spv::Id Float7e3To32(SpirvBuilder& builder, spv::Id f10_uint_scalar, uint32_t f10_shift,
                              bool result_as_uint, spv::Id ext_inst_glsl_std_450);
  /**
   * Converts a depth value clamped to [0, 2) to 20e4 float with zeros in bits 24:31, rounding to
   * nearest even or towards zero.
   *
   * @param builder Builder emitting the instructions.
   * @param f32_scalar Clamped 32 bit float scalar.
   * @param round_to_nearest_even True to round to nearest even, false to round towards zero.
   * @param remap_from_0_to_0_5 True when the input range 0...1 is pre remapped to 0...0.5.
   * @param ext_inst_glsl_std_450 Id of the imported GLSL.std.450 instruction set.
   * @return Id of the 20e4 value as a uint.
   */
  static spv::Id PreClampedDepthTo20e4(SpirvBuilder& builder, spv::Id f32_scalar,
                                       bool round_to_nearest_even, bool remap_from_0_to_0_5,
                                       spv::Id ext_inst_glsl_std_450);
  /**
   * Converts the 20e4 number in bits [f24_shift, f24_shift + 10) to a 32 bit float.
   *
   * @param builder Builder emitting the instructions.
   * @param f24_uint_scalar Uint scalar holding the 20e4 number.
   * @param f24_shift Bit offset of the 20e4 number.
   * @param remap_to_0_to_0_5 True to remap the result range 0...1 to 0...0.5.
   * @param result_as_uint True to return the float bits as a uint.
   * @param ext_inst_glsl_std_450 Id of the imported GLSL.std.450 instruction set.
   * @return Id of the converted value.
   */
  static spv::Id Depth20e4To32(SpirvBuilder& builder, spv::Id f24_uint_scalar, uint32_t f24_shift,
                               bool remap_to_0_to_0_5, bool result_as_uint,
                               spv::Id ext_inst_glsl_std_450);

 protected:
  void Reset() override;

  uint32_t GetModificationRegisterCount() const override;

  void StartTranslation() override;

  /**
   * The rectangle list fallback without geometry shaders runs the guest vertex shader 3 times, once
   * per source vertex, then chooses or synthesizes the output for the current host strip vertex.
   * Denormals flush to zero like the hardware, including for the Shader Model 3 multiplication
   * emulation. Signed zero is preserved as it yields VFACE from ps_param_gen, and infinity as some
   * instructions treat it specially, such as logarithm, reciprocal and muls_prev2.
   *
   * @return The SPIR-V words of the shader.
   */
  std::vector<uint8_t> CompleteTranslation() override;

  void PostTranslation() override;

  void ProcessLabel(uint32_t cf_index) override;

  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  /**
   * The loop constant holds the count in bits 0:7 and the initial aL in bits 8:15, both unsigned.
   * Pushing moves the count stack XYZ to YZW and sets X, and the loop is skipped when the count is
   * 0.
   *
   * @param instr Parsed loop start instruction.
   */
  void ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) override;
  /**
   * The signed value added to aL is in bits 16:23 of the loop constant. A predicated break is a
   * break if (loop_count == 0 || [!]p0). Breaking pops the count and relative address stacks by
   * moving YZW to XYZ and setting W to 0.
   *
   * @param instr Parsed loop end instruction.
   */
  void ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr, uint8_t export_eM) override;

  /**
   * Vfetch_full computes `base + index * stride` in dwords, kept for the following vfetch_mini.
   * The base is bits 2:31 of fetch constant word 0 and the endianness is bits 0:1 of word 1. The
   * index is converted to an integer as floor(index + 0.5), since rounding to nearest even makes no
   * sense for addressing. Words outside the buffer are not bound checked, and the extended range
   * Xenos float16 is converted as GLSL float16. Signed normalized values map both -(2^(n-1)) and
   * -(2^(n-1)-1) to -1. Signed normalization of 1 bit components is unsupported as no format has
   * them.
   *
   * @param instr Parsed vertex fetch instruction.
   */
  void ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& instr) override;
  /**
   * Fetch constant word 2 holds the size stored minus 1, word 3 the exponent adjustment, word 4
   * the LOD at bits 12:21, the result exponent bias at bits 13:18 and the stacked texture filtering
   * modes, and word 5 the dimensionality at bits 9:10. 3D and stacked textures use different
   * bindings. The LOD sources accumulate in the D3D11.3 order of specified LOD, sampler LOD bias
   * and instruction LOD bias. Offsets have a granularity of 0.5, so they cannot be passed as
   * ConstOffset, and are applied at LOD 0. A rounding epsilon of 1.5/4 of the fixed point texture
   * coordinate ULP with 8 bit subtexel precision resolves ambiguity when point sampling between
   * texels. Cube SC and TC are moved from 1...2 to -1...1 and the face index is split into axis
   * and sign, remapped opposite to the ALU cube instruction. Stacked textures floor the array layer
   * since addressing is similar to 3D textures, and the LOD within layers does not affect filtering
   * between layers. Signedness is per guest component while a host format component may repeat, so
   * it is applied during swizzling for unsigned versus signed and after it for biased and gamma.
   *
   * @param instr Parsed texture fetch instruction.
   */
  void ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& instr) override;
  /**
   * Floating point addition, subtraction, negation, multiplication, division and modulo carry
   * the NoContraction decoration (glslang does not apply it to OpDot) so calculations optimize
   * predictably and identically across shaders for multipass rendering, together with the
   * Invariant decoration on outputs.
   *
   * @param instr Parsed ALU instruction.
   * @param memexport_eM_potentially_written_before Bit mask of eM# elements possibly written.
   */
  void ProcessAluInstruction(const ParsedAluInstruction& instr,
                             uint8_t memexport_eM_potentially_written_before) override;

 private:
  struct TextureBinding {
    uint32_t fetch_constant;
    xenos::FetchOpDimension dimension;
    bool is_signed;

    spv::Id variable;
  };

  struct SamplerBinding {
    uint32_t fetch_constant;
    xenos::TextureFilter mag_filter;
    xenos::TextureFilter min_filter;
    xenos::TextureFilter mip_filter;
    xenos::AnisoFilter aniso_filter;

    spv::Id variable;
  };

  spv::Id SpirvSmearScalarResultOrConstant(spv::Id scalar, spv::Id vector_type);

  Modification GetSpirvShaderModification() const {
    return Modification(current_translation().modification());
  }

  bool IsSpirvVertexShader() const {
    return is_vertex_shader() && !Shader::IsHostVertexShaderTypeDomain(
                                     GetSpirvShaderModification().vertex.host_vertex_shader_type);
  }
  bool IsSpirvTessEvalShader() const {
    return is_vertex_shader() && Shader::IsHostVertexShaderTypeDomain(
                                     GetSpirvShaderModification().vertex.host_vertex_shader_type);
  }
  bool IsSpirvComputeShader() const {
    return is_vertex_shader() && GetSpirvShaderModification().vertex.host_vertex_shader_type ==
                                     Shader::HostVertexShaderType::kMemExportCompute;
  }
  bool IsSpirvRectangleListVertexLoopEnabled() const {
    return IsSpirvVertexShader() && GetSpirvShaderModification().vertex.host_vertex_shader_type ==
                                        Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  }

  bool IsExecutionModeEarlyFragmentTests() const {
    return is_pixel_shader() &&
           GetSpirvShaderModification().pixel.depth_stencil_mode ==
               Modification::DepthStencilMode::kEarlyHint &&
           !edram_fragment_shader_interlock_ && current_shader().implicit_early_z_write_allowed();
  }
  bool IsWritingFloat24Depth() const {
    REX_ASSERT(is_pixel_shader());
    if (edram_fragment_shader_interlock_) {
      return false;
    }
    Modification::DepthStencilMode depth_stencil_mode =
        GetSpirvShaderModification().pixel.depth_stencil_mode;
    return depth_stencil_mode == Modification::DepthStencilMode::kFloat24Truncating ||
           depth_stencil_mode == Modification::DepthStencilMode::kFloat24Rounding;
  }
  bool IsSampleRate() const {
    REX_ASSERT(is_pixel_shader());
    return IsWritingFloat24Depth() && !current_shader().writes_depth();
  }

  uint32_t GetModificationInterpolatorMask() const {
    Modification modification = GetSpirvShaderModification();
    return is_vertex_shader() ? modification.vertex.interpolator_mask
                              : modification.pixel.interpolator_mask;
  }

  /** @return UINT32_MAX when PsParamGen need not be written. */
  uint32_t GetPsParamGenInterpolator() const;

  /**
   * Starts a new block when the last instruction, such as OpBranch, terminated the current one.
   * Operations that must be in a block are emitted after it.
   */
  void EnsureBuildPointAvailable();

  /**
   * For the memexport fallback each invocation corresponds to one guest vertex processing index.
   * The point size output is a separate variable rather than gl_PointSize, which would depend on
   * the shaderTessellationAndGeometryPointSize feature and must be written greater than zero.
   */
  void StartVertexOrTessEvalShaderBeforeMain();
  /**
   * Initializes the point size negative so point sprite expansion uses the default size unless the
   * vertex shader overrides it, and the kill bits 0:30 to zero. General purpose registers are
   * zeroed as titles read registers they initialized only conditionally. Tessellation registers
   * hold r0.xyz domain location zyx with r1.xyz control point indices (triangle, control point
   * indexed), r0.xyz domain location zyx with r1.x patch index and r1.y swizzle (triangle, patch
   * indexed), r0.xy domain location xy with r0.z and r1.xyz control point indices (quad, control
   * point indexed), and r0.x patch index with r0.yz domain location xy and r1.x swizzle (quad,
   * patch indexed). Memory export is allowed from only 1 host vertex per guest vertex, host vertex
   * 0 for points and the rectangle loop mode, and every host vertex except the synthetic fourth one
   * otherwise. Rectangle lists consume 3 guest vertices per primitive and the synthetic fourth host
   * vertex is clamped to guest vertex 2. Indices get the base index added, the 24 bit index mask
   * applied and are clamped to the draw index range. Primitive reset suppresses memexport for reset
   * index invocations in the compute fallback.
   */
  void StartVertexOrTessEvalShaderInMain();
  /**
   * Vertex killing in the AND mode kills via a cull distance so the primitive is culled only if all
   * vertices request it, and in the OR mode sets W to NaN. Point sprite corners are emitted in the
   * order top left, bottom left, top right, bottom right.
   */
  void CompleteVertexOrTessEvalShaderInMain();

  void StartFragmentShaderBeforeMain();
  /**
   * Pixel killing from the translated shader does not affect control flow, unlike OpKill, as on the
   * Xenos, and keeps a single critical section exit and return across Vulkan implementations with
   * fragment shader interlock. The rest of the shader is skipped when the whole quad failed the
   * depth and stencil test with no late writes pending, detected from zero derivatives of the
   * covered flag. gl_SampleMaskIn on Vulkan may omit the coverage bits of other sample invocations,
   * unlike Direct3D SV_Coverage, so the sample friendliest to the half pixel offset is used.
   * PsParamGen holds X as pixel X .0 with back facing in the sign, Y as pixel Y .0 with point in
   * the sign, Z as point S with line in the sign and W as point T. Fragment coordinates are rounded
   * down and made absolute so derivative magnitude and LODs stay correct on the left and top edges.
   */
  void StartFragmentShaderInMain();
  /**
   * Without oDepth the guest [0, 1] depth is rebuilt from the host [0, 0.5] range by doubling
   * gl_FragCoord.z and saturating, and written depth is converted to float24 and back to host
   * [0, 0.5]. The alpha test comparison "always" passes even for NaN and "not equal" is true for
   * NaN. The exponent bias is applied after the alpha test and alpha to coverage, which need the
   * unbiased alpha. Guest MSAA samples map to Vulkan samples with 2x top and bottom as 1 and 0
   * (0 and 3 in the 4x fallback) and 4x TL, BL, TR, BR as TL, TR, BL, BR. Gamma conversion in the
   * host render target path happens before blending, while the Xenos converts after blending.
   */
  void CompleteFragmentShaderInMain();

  /**
   * Updates the current flow control condition at the start of exec and in jumps, closing the
   * previous conditionals when needed. Does not close the instruction level predicate
   * conditional when the condition is unchanged, callers such as jumps must check separately.
   *
   * @param type Conditional type of the exec instruction.
   * @param bool_constant_index Index of the bool constant for a bool conditional.
   * @param condition Condition the exec requires.
   */
  void UpdateExecConditionals(ParsedExecInstruction::Type type, uint32_t bool_constant_index,
                              bool condition);
  /**
   * Opens or reopens the predicate check conditional that a non control flow instruction is
   * emitted in.
   *
   * @param predicated True when the instruction is predicated.
   * @param condition Predicate value the instruction requires.
   */
  void UpdateInstructionPredication(bool predicated, bool condition);
  /**
   * Closes the instruction level predicate conditional so a control flow instruction can emit
   * code that respects the exec conditional without being predicated.
   */
  void CloseInstructionPredication();
  /** Closes conditionals opened by exec and the instructions within it, but not by labels. */
  void CloseExecConditionals();

  spv::Id GetStorageAddressingIndex(InstructionStorageAddressingMode addressing_mode,
                                    uint32_t storage_index, bool is_float_constant = false);
  /**
   * Loads the unswizzled operand without sign modifiers as float4.
   *
   * @param operand Operand to load.
   * @return Id of the loaded float4.
   */
  spv::Id LoadOperandStorage(const InstructionOperand& operand);
  spv::Id ApplyOperandModifiers(spv::Id operand_value, const InstructionOperand& original_operand,
                                bool invert_negate = false, bool force_absolute = false);
  /**
   * Returns the requested components with the operand swizzle applied, condensed and without
   * negation or absolute value modifiers. The storage is float4 whatever the component count.
   *
   * @param operand_storage Id of the loaded float4 operand.
   * @param original_operand Operand providing the swizzle.
   * @param components Number of components to return.
   * @return Id of the requested components.
   */
  spv::Id GetUnmodifiedOperandComponents(spv::Id operand_storage,
                                         const InstructionOperand& original_operand,
                                         uint32_t components);
  spv::Id GetOperandComponents(spv::Id operand_storage, const InstructionOperand& original_operand,
                               uint32_t components, bool invert_negate = false,
                               bool force_absolute = false) {
    return ApplyOperandModifiers(
        GetUnmodifiedOperandComponents(operand_storage, original_operand, components),
        original_operand, invert_negate, force_absolute);
  }
  /**
   * Writes the same Id to both outputs when the components are identical.
   *
   * @param operand_storage Id of the loaded float4 operand.
   * @param original_operand Operand providing the swizzle.
   * @param a_out Receives the first component.
   * @param b_out Receives the second component.
   * @param invert_negate True to invert the operand negation.
   * @param force_absolute True to apply the absolute value.
   */
  void GetOperandScalarXY(spv::Id operand_storage, const InstructionOperand& original_operand,
                          spv::Id& a_out, spv::Id& b_out, bool invert_negate = false,
                          bool force_absolute = false);
  /**
   * Returns the absolute value of the loaded operand, or the operand itself when absolute.
   *
   * @param operand_storage Id of the loaded float4 operand.
   * @param original_operand Operand providing the modifiers.
   * @return Id of the absolute value.
   */
  spv::Id GetAbsoluteOperand(spv::Id operand_storage, const InstructionOperand& original_operand);
  /**
   * Stores a value to the result. A point size is made non negative, since negative selects the
   * default size, and clamped like the R400 hardware, functionally a signed 32 bit integer with
   * -NaN and -Infinity...-0 mapping to the minimum and +NaN to the maximum.
   *
   * @param result Result destination.
   * @param value Float vector or scalar for the used components, or spv::NoResult for no store.
   * @param can_store_memexport_address Allows only a MAD with a stream constant to write eA.
   */
  void StoreResult(const InstructionResult& result, spv::Id value,
                   bool can_store_memexport_address = false);

  /**
   * Shader Model 3 multiplication yields +0 for +-0 or denormal times anything. Replaces the value
   * with +0 when the minimum of the operands is 0.
   *
   * @param value Product to replace when needed.
   * @param operand_0_abs Absolute value of the first operand, see GetAbsoluteOperand.
   * @param operand_1_abs Absolute value of the second operand.
   * @return Id of the product, or +0 when an operand is 0.
   */
  spv::Id ZeroIfAnyOperandIsZero(spv::Id value, spv::Id operand_0_abs, spv::Id operand_1_abs);
  /**
   * Conditionally discards the current fragment and changes the build point. Outstanding memory
   * exports are performed before the invocation becomes inactive and storage writes are disabled.
   *
   * @param condition Bool Id that is true when the fragment is discarded.
   * @param memexport_eM_potentially_written_before Bit mask of eM# elements possibly written.
   */
  void KillPixel(spv::Id condition, uint8_t memexport_eM_potentially_written_before);
  /**
   * Returns a float vector of rex::bit_count(result.GetUsedResultComponents()) components or a
   * single float for a reduction instruction, or spv::NoResult when there is nothing to store.
   *
   * Max and min follow Shader Model 3 NaN behavior (a op b ? a : b), unlike SPIR-V FMax and FMin
   * that are undefined for NaN and NMax and NMin that return the non NaN operand. Max4 uses NMax.
   * Cube takes operands[0] as .z_xy and returns the T
   * coordinate, S coordinate, 2 * major axis and face ID. For major axis Z, tc = -y,
   * sc = z < 0 ? -x : x and id = z < 0 ? 5 : 4. For Y, sc = x, tc = y < 0 ? -z : z and
   * id = y < 0 ? 3 : 2. For X, tc = -y, sc = x < 0 ? z : -z and id = x < 0 ? 1 : 0. The
   * setp push opcodes set p0 = src0.w == 0 && src1.w op 0 and return
   * (src0.x == 0 && src1.x op 0) ? 0 : src0.x + 1. Dst returns x = 1, y = src0.y * src1.y,
   * z = src0.z and w = src1.w. Maxa sets a0 = (int)clamp(floor(src0.w + 0.5), -256, 255).
   *
   * @param instr Parsed ALU instruction.
   * @param memexport_eM_potentially_written_before Bit mask of eM# elements possibly written.
   * @param predicate_written Set when the operation writes the predicate.
   * @return Id of the vector result.
   */
  spv::Id ProcessVectorAluOperation(const ParsedAluInstruction& instr,
                                    uint8_t memexport_eM_potentially_written_before,
                                    bool& predicate_written);
  /**
   * Returns the float to write to the previous scalar register and the destination, or
   * spv::NoResult when it is ps itself in the retain_prev case.
   *
   * Max and min use the Shader Model 3 NaN behavior as in the vector operations. Maxas sets
   * a0 = (int)clamp(floor(src0.a + 0.5), -256, 255) and maxasf uses floor(src0.a). Muls_prev2
   * multiplies src0.a by ps only when ps is finite and not -FLT_MAX, src0.b is finite and
   * src0.b > 0, otherwise it returns -FLT_MAX.
   *
   * @param instr Parsed ALU instruction.
   * @param memexport_eM_potentially_written_before Bit mask of eM# elements possibly written.
   * @param predicate_written Set when the operation writes the predicate.
   * @return Id of the scalar result.
   */
  spv::Id ProcessScalarAluOperation(const ParsedAluInstruction& instr,
                                    uint8_t memexport_eM_potentially_written_before,
                                    bool& predicate_written);

  spv::Id EndianSwap32Uint(spv::Id value, spv::Id endian);
  spv::Id EndianSwap128Uint4(spv::Id value, spv::Id endian);

  /**
   * Loads a uint32 from shared memory, zero when out of bounds. With multiple bindings the binding
   * is chosen without indexing the array by a variable, as that needs non uniform storage buffer
   * indexing. The binding split is by log2(512 MB) = 29 in bytes, addressing in dwords.
   *
   * @param address_dwords_int Signed int address in dwords.
   * @return Id of the loaded uint32.
   */
  spv::Id LoadUint32FromSharedMemory(spv::Id address_dwords_int);
  /**
   * Stores a uint32 to shared memory.
   *
   * @param value Uint32 to store.
   * @param address_dwords_int Signed int address in dwords.
   * @param replace_mask Bits to replace via OpAtomicAnd and OpAtomicOr, other bits of value are
   * ignored.
   */
  void StoreUint32ToSharedMemory(spv::Id value, spv::Id address_dwords_int,
                                 spv::Id replace_mask = spv::NoResult);

  bool IsMemoryExportSupported() const {
    if (is_pixel_shader()) {
      return features_.fragment_stores_and_atomics;
    }
    return features_.vertex_pipeline_stores_and_atomics || IsSpirvComputeShader();
  }

  bool IsMemoryExportUsed() const {
    return current_shader().memexport_eM_written() && IsMemoryExportSupported();
  }

  /**
   * Emits the memory export writes for the written eM# elements. Writes only when the address in
   * eA has sign and exponent bits 0x1 and 0x96 (an index within the mantissa bits) and the index
   * stays inside the 23 bit limited stream. The stream base is converted from dwords to bytes by
   * a left shift of 2 that drops the upper bits. Color format packing follows the Direct3D
   * conversion rules, rounding by adding plus or minus 0.5 before truncation, and requires
   * widths without holes that add up to the texel size of 8, 16 or 32 bits and are at most 23
   * bits so max + 0.5 is exact.
   *
   * @param export_eM Bit mask of the eM# elements to export.
   */
  void ExportToMemory(uint8_t export_eM);

  /**
   * Converts piecewise linear gamma to linear. The source may be a float scalar or vector.
   *
   * @param gamma Gamma value.
   * @param gamma_pre_saturated True when the gamma value is already saturated.
   * @return Id of the linear value.
   */
  spv::Id PWLGammaToLinear(spv::Id gamma, bool gamma_pre_saturated);
  spv::Id LinearToPWLGamma(spv::Id linear, bool linear_pre_saturated);

  /**
   * 1D and 2D textures, including stacked ones, are bound as 2D arrays.
   *
   * @param fetch_constant Fetch constant index.
   * @param dimension Fetch dimension.
   * @param is_signed True for the signed texture version.
   * @return Index of the texture binding.
   */
  size_t FindOrAddTextureBinding(uint32_t fetch_constant, xenos::FetchOpDimension dimension,
                                 bool is_signed);
  /**
   * Sampler binding indices are assigned after all textures are added, as samplers follow images
   * in the descriptor set.
   *
   * @param fetch_constant Fetch constant index.
   * @param mag_filter Magnification filter.
   * @param min_filter Minification filter.
   * @param mip_filter Mip filter.
   * @param aniso_filter Anisotropic filter.
   * @return Index of the sampler binding.
   */
  size_t FindOrAddSamplerBinding(uint32_t fetch_constant, xenos::TextureFilter mag_filter,
                                 xenos::TextureFilter min_filter, xenos::TextureFilter mip_filter,
                                 xenos::AnisoFilter aniso_filter);
  /**
   * Samples with texture_parameters set up except for sampler. Optionally interpolates between an
   * existing value and the new one, the result may alias the first lerp endpoint but not across
   * signedness.
   *
   * @param texture_parameters Sampling parameters, with the sampler left unset.
   * @param image_operands_mask Image operands mask.
   * @param image_unsigned Unsigned texture Id.
   * @param image_signed Signed texture Id.
   * @param sampler Sampler Id.
   * @param is_any_unsigned Bool Id that is true when any component is unsigned.
   * @param is_any_signed Bool Id that is true when any component is signed.
   * @param result_unsigned_out Receives the unsigned sample.
   * @param result_signed_out Receives the signed sample.
   * @param lerp_factor Interpolation factor, or spv::NoResult for no interpolation.
   * @param lerp_first_unsigned First unsigned interpolation endpoint.
   * @param lerp_first_signed First signed interpolation endpoint.
   */
  void SampleTexture(spv::Builder::TextureParameters& texture_parameters,
                     spv::ImageOperandsMask image_operands_mask, spv::Id image_unsigned,
                     spv::Id image_signed, spv::Id sampler, spv::Id is_any_unsigned,
                     spv::Id is_any_signed, spv::Id& result_unsigned_out,
                     spv::Id& result_signed_out, spv::Id lerp_factor = spv::NoResult,
                     spv::Id lerp_first_unsigned = spv::NoResult,
                     spv::Id lerp_first_signed = spv::NoResult);
  /**
   * Returns the LOD queried with texture_parameters set up except for the sampler.
   *
   * @param texture_parameters Sampling parameters, with the sampler left unset.
   * @param image_unsigned Unsigned texture Id.
   * @param image_signed Signed texture Id.
   * @param sampler Sampler Id.
   * @param is_all_signed Bool Id that is true when all components are signed.
   * @return Id of the queried LOD.
   */
  spv::Id QueryTextureLod(spv::Builder::TextureParameters& texture_parameters,
                          spv::Id image_unsigned, spv::Id image_signed, spv::Id sampler,
                          spv::Id is_all_signed);

  spv::Id LoadMsaaSamplesFromFlags();
  /**
   * Reports whether skipping the translated shader for 2x2 quads is possible and worthwhile.
   *
   * @return True when the depth and stencil test can run early.
   */
  bool FSI_IsDepthStencilEarly() const {
    REX_ASSERT(edram_fragment_shader_interlock_);
    return !is_depth_only_fragment_shader_ && !current_shader().writes_depth() &&
           !current_shader().memexport_eM_written();
  }
  /**
   * On the Xenos 2x MSAA doubles the storage height and 4x MSAA doubles the storage width. Vulkan
   * 2x samples are bottom, top and 4x samples are TL, TR, BL, BR, remapped to T, B for 2x and to
   * TL, BL, TR, BR for 4x. 2x corresponds to Vulkan samples 1, 0 with native 2x MSAA and 0, 3 with
   * 2x as 4x, and 4x corresponds to 0, 2, 1, 3.
   *
   * @param msaa_samples Uint Id of the MSAA sample count.
   */
  void FSI_LoadSampleMask(spv::Id msaa_samples);
  /**
   * A 32bpp tile is 80x16 samples in 2 half tiles of 40x16 and a 64bpp tile is 40x16. Depth and
   * stencil place samples 40...79 in the first half tile and 0...39 in the second, flipped
   * compared to color. Addresses wrap modulo the EDRAM size.
   *
   * @param msaa_samples Uint Id of the MSAA sample count.
   */
  void FSI_LoadEdramOffsets(spv::Id msaa_samples);
  /**
   * The address must be a signed int.
   *
   * @param sample_0_address Signed int address of sample 0.
   * @param sample_index Index of the sample.
   * @param is_64bpp Bool for a 64bpp render target, or spv::NoResult for 32bpp.
   * @return Id of the sample address.
   */
  spv::Id FSI_AddSampleOffset(spv::Id sample_0_address, uint32_t sample_index,
                              spv::Id is_64bpp = spv::NoResult);
  /**
   * Updates main_fsi_sample_mask_. Call outside non uniform control flow as it takes derivatives
   * of the fragment depth.
   *
   * Per sample depths are computed manually since interpolateAtSample(gl_FragCoord) is invalid in
   * GLSL. The polygon offset is applied at the center without clamping so that derivatives scaled
   * by the sample locations added afterwards stay linear. The new depth is converted to 24 bit
   * rounding to nearest even. Stencil increment and decrement with wrap ignore the upper old
   * depth bits. Sample positions are the center for no MSAA, top left for native 2x, 2x as 4x
   * and 4x, and for guest 2x the bottom right, for guest 4x the bottom left.
   *
   * @param msaa_samples Uint Id of the MSAA sample count.
   * @param sample_mask_potentially_narrowed_previouly True when the sample mask may be narrowed.
   */
  void FSI_DepthStencilTest(spv::Id msaa_samples, bool sample_mask_potentially_narrowed_previouly);
  /**
   * Returns the first and second 32 bits as 2 uints. NaN is flushed to 0 rather than the minimum
   * value, and the extended range Xenos float16 is unsupported.
   *
   * @param color_float4 Float4 color to clamp and pack.
   * @param format_with_flags Uint Id of the color format with flags.
   * @return The packed color as 2 uints.
   */
  std::array<spv::Id, 2> FSI_ClampAndPackColor(spv::Id color_float4, spv::Id format_with_flags);
  std::array<spv::Id, 4> FSI_UnpackColor(std::array<spv::Id, 2> color_packed,
                                         spv::Id format_with_flags);
  /**
   * The bounds must have the same component count as the color or alpha.
   *
   * @param color_or_alpha Color or alpha value.
   * @param is_fixed_point Bool Id that is true for a fixed point format.
   * @param min_value Lower bound.
   * @param max_value Upper bound.
   * @return Id of the flushed and clamped value.
   */
  spv::Id FSI_FlushNaNClampAndInBlending(spv::Id color_or_alpha, spv::Id is_fixed_point,
                                         spv::Id min_value, spv::Id max_value);
  /**
   * A zero factor is excluded from the multiplication so infinity and NaN do not take part.
   *
   * @param value Color to scale.
   * @param is_fixed_point Bool Id that is true for a fixed point format.
   * @param clamp_min_value Lower clamp bound.
   * @param clamp_max_value Upper clamp bound.
   * @param factor Blend factor.
   * @param source_color Source color.
   * @param source_alpha Source alpha.
   * @param dest_color Destination color.
   * @param dest_alpha Destination alpha.
   * @param constant_color Blend constant color.
   * @param constant_alpha Blend constant alpha.
   * @return Id of the scaled color.
   */
  spv::Id FSI_ApplyColorBlendFactor(spv::Id value, spv::Id is_fixed_point, spv::Id clamp_min_value,
                                    spv::Id clamp_max_value, spv::Id factor, spv::Id source_color,
                                    spv::Id source_alpha, spv::Id dest_color, spv::Id dest_alpha,
                                    spv::Id constant_color, spv::Id constant_alpha);
  /**
   * A zero factor is excluded from the multiplication so infinity and NaN do not take part.
   *
   * @param value Alpha to scale.
   * @param is_fixed_point Bool Id that is true for a fixed point format.
   * @param clamp_min_value Lower clamp bound.
   * @param clamp_max_value Upper clamp bound.
   * @param factor Blend factor.
   * @param source_alpha Source alpha.
   * @param dest_alpha Destination alpha.
   * @param constant_alpha Blend constant alpha.
   * @return Id of the scaled alpha.
   */
  spv::Id FSI_ApplyAlphaBlendFactor(spv::Id value, spv::Id is_fixed_point, spv::Id clamp_min_value,
                                    spv::Id clamp_max_value, spv::Id factor, spv::Id source_alpha,
                                    spv::Id dest_alpha, spv::Id constant_alpha);
  /**
   * Blends the alpha when source_color_clamped, dest_color and constant_color_clamped are
   * spv::NoResult, otherwise the color. The result is unclamped, color packing clamps it.
   *
   * @param is_fixed_point Bool Id that is true for a fixed point format.
   * @param clamp_min_value Lower clamp bound.
   * @param clamp_max_value Upper clamp bound.
   * @param source_color_clamped Clamped source color.
   * @param source_alpha_clamped Clamped source alpha.
   * @param dest_color Destination color.
   * @param dest_alpha Destination alpha.
   * @param constant_color_clamped Clamped blend constant color.
   * @param constant_alpha_clamped Clamped blend constant alpha.
   * @param equation Blend equation.
   * @param source_factor Source blend factor.
   * @param dest_factor Destination blend factor.
   * @return Id of the unclamped blend result.
   */
  spv::Id FSI_BlendColorOrAlphaWithUnclampedResult(
      spv::Id is_fixed_point, spv::Id clamp_min_value, spv::Id clamp_max_value,
      spv::Id source_color_clamped, spv::Id source_alpha_clamped, spv::Id dest_color,
      spv::Id dest_alpha, spv::Id constant_color_clamped, spv::Id constant_alpha_clamped,
      spv::Id equation, spv::Id source_factor, spv::Id dest_factor);

  Features features_;
  bool native_2x_msaa_with_attachments_;
  bool native_2x_msaa_no_attachments_;

  /**
   * Interlock begin and end must be called only on the outermost control flow level of main,
   * with no return before either, matching the OpenGL fragment shader interlock limits.
   */
  bool edram_fragment_shader_interlock_;
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  /** Reports whether the empty depth only pixel shader is being written. */
  bool is_depth_only_fragment_shader_ = false;

  std::unique_ptr<SpirvBuilder> builder_;

  std::vector<spv::Id> id_vector_temp_;
  std::vector<spv::Id> id_vector_temp_util_;
  std::vector<unsigned int> uint_vector_temp_;
  std::vector<unsigned int> uint_vector_temp_util_;

  spv::Id ext_inst_glsl_std_450_;

  spv::Id type_void_;

  union {
    struct {
      spv::Id type_bool_;
      spv::Id type_bool2_;
      spv::Id type_bool3_;
      spv::Id type_bool4_;
    };
    spv::Id type_bool_vectors_[4];
  };
  union {
    struct {
      spv::Id type_int_;
      spv::Id type_int2_;
      spv::Id type_int3_;
      spv::Id type_int4_;
    };
    spv::Id type_int_vectors_[4];
  };
  union {
    struct {
      spv::Id type_uint_;
      spv::Id type_uint2_;
      spv::Id type_uint3_;
      spv::Id type_uint4_;
    };
    spv::Id type_uint_vectors_[4];
  };
  union {
    struct {
      spv::Id type_float_;
      spv::Id type_float2_;
      spv::Id type_float3_;
      spv::Id type_float4_;
    };
    spv::Id type_float_vectors_[4];
  };

  spv::Id const_int_0_;
  spv::Id const_int4_0_;
  spv::Id const_uint_0_;
  spv::Id const_uint4_0_;
  union {
    struct {
      spv::Id const_float_0_;
      spv::Id const_float2_0_;
      spv::Id const_float3_0_;
      spv::Id const_float4_0_;
    };
    spv::Id const_float_vectors_0_[4];
  };
  union {
    struct {
      spv::Id const_float_1_;
      spv::Id const_float2_1_;
      spv::Id const_float3_1_;
      spv::Id const_float4_1_;
    };
    spv::Id const_float_vectors_1_[4];
  };
  spv::Id const_float2_0_1_;

  enum SystemConstantIndex : unsigned int {
    kSystemConstantFlags,
    kSystemConstantVertexIndexLoadAddress,
    kSystemConstantVertexIndexEndian,
    kSystemConstantLineLoopClosingIndex,
    kSystemConstantVertexBaseIndex,
    kSystemConstantVertexIndexReset,
    kSystemConstantComputeMemExportVertexCount,
    kSystemConstantUserClipPlanes,
    kSystemConstantVertexIndexMin,
    kSystemConstantVertexIndexMax,
    kSystemConstantNdcScale,
    kSystemConstantPointVertexDiameterMin,
    kSystemConstantNdcOffset,
    kSystemConstantPointVertexDiameterMax,
    kSystemConstantPointConstantDiameter,
    kSystemConstantPointScreenDiameterToNdcRadius,
    kSystemConstantTexturesResolutionScaled,
    kSystemConstantTextureSwizzledSigns,
    kSystemConstantTextureSwizzles,
    kSystemConstantAlphaTestReference,
    kSystemConstantAlphaToMask,
    kSystemConstantEdram32bppTilePitchDwordsScaled,
    kSystemConstantEdramDepthBaseDwordsScaled,
    kSystemConstantColorExpBias,
    kSystemConstantEdramPolyOffsetFrontScale,
    kSystemConstantEdramPolyOffsetBackScale,
    kSystemConstantEdramPolyOffsetFrontOffset,
    kSystemConstantEdramPolyOffsetBackOffset,
    kSystemConstantEdramStencilFront,
    kSystemConstantEdramStencilBack,
    kSystemConstantEdramRTBaseDwordsScaled,
    kSystemConstantEdramRTFormatFlags,
    kSystemConstantEdramRTBlendFactorsOps,
    // Accessed as float4[2], not float2[4], due to std140 array stride alignment.
    kSystemConstantEdramRTKeepMask,
    kSystemConstantEdramRTClamp,
    kSystemConstantEdramBlendConstant,
  };
  spv::Id uniform_system_constants_;
  spv::Id uniform_float_constants_;
  spv::Id uniform_bool_loop_constants_;
  spv::Id uniform_fetch_constants_;

  spv::Id buffers_shared_memory_;
  spv::Id buffer_edram_;

  /**
   * Separate images and samplers, as maxPerStageDescriptorSamplers is often lower than
   * maxPerStageDescriptorSampledImages.
   */
  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;

  /** int for VS as VS, uint3 for VS as compute (kMemExportCompute). */
  spv::Id input_vertex_index_;
  /** int, VS as TES only. */
  spv::Id input_primitive_id_;
  /** float3, VS as TES only. */
  spv::Id input_tess_coord_;
  /** patch float4, VS as TES only. */
  spv::Id input_patch_control_point_indices_;
  /** float2, PS only when needed. */
  spv::Id input_point_coordinates_;
  /** float4, PS only when needed. */
  spv::Id input_fragment_coordinates_;
  /** bool, PS only when needed. */
  spv::Id input_front_facing_;
  /** int, PS only when needed. */
  spv::Id input_sample_id_;
  /** int[1], PS only when needed. */
  spv::Id input_sample_mask_;

  /**
   * float4 indexed by the guest interpolator index, spv::NoResult for unneeded ones. Adreno
   * requires separate variables in both stages, including the geometry shader, because it does not
   * link consecutive locations as array elements.
   */
  std::array<spv::Id, xenos::kMaxInterpolators> input_output_interpolators_;

  /** float2, VS kPointListAsTriangleStrip only when the PS needs it. */
  spv::Id output_point_coordinates_;
  /** float, VS only when needed. */
  spv::Id output_point_size_;

  enum OutputPerVertexMember : unsigned int {
    kOutputPerVertexMemberPosition,
    kOutputPerVertexMemberClipDistance,
    kOutputPerVertexMemberCullDistance,
    kOutputPerVertexMemberCountMax,
  };
  uint32_t output_per_vertex_member_count_;
  uint32_t output_per_vertex_member_clip_distance_;
  uint32_t output_per_vertex_member_cull_distance_;
  spv::Id type_output_per_vertex_;
  spv::Id output_per_vertex_;

  /**
   * Variables in main with fragment shader interlock, framebuffer color attachment outputs
   * otherwise.
   */
  std::array<spv::Id, xenos::kMaxColorRenderTargets> output_or_var_fragment_data_;
  /** float, host render targets only when needed. */
  spv::Id output_fragment_depth_;
  /** int[1], host render targets only when needed. */
  spv::Id output_fragment_sample_mask_;

  std::vector<spv::Id> main_interface_;
  spv::Function* function_main_;
  spv::Id main_system_constant_flags_;
  spv::Id var_main_predicate_;
  spv::Id var_main_loop_count_;
  spv::Id var_main_loop_address_;
  spv::Id var_main_address_register_;
  spv::Id var_main_previous_scalar_;
  /**
   * int, `base + index * stride` in dwords from the last vfetch_full, needed by vfetch_mini.
   */
  spv::Id var_main_vfetch_address_;
  spv::Id var_main_tfetch_lod_;
  spv::Id var_main_tfetch_gradients_h_;
  spv::Id var_main_tfetch_gradients_v_;
  spv::Id var_main_registers_;
  spv::Id var_main_memexport_address_;
  spv::Id var_main_memexport_data_[xenos::ucode::kMaxMemExportElementCount];
  /**
   * uint bit field of the eM# elements written by the invocation since the last memory write.
   */
  spv::Id var_main_memexport_data_written_;
  /**
   * bool, whether memory export is allowed in the current execution of the translated code. It is
   * disabled in some invocations when the shader runs several times for the same guest vertex or
   * pixel.
   */
  spv::Id main_memexport_allowed_;
  /** uint, VS rectangle list fallback only. */
  spv::Id var_main_rectangle_guest_vertex_index_;
  /**
   * float4[3] gl_Position, VS rectangle list fallback only. Not the gl_PerVertex block type, whose
   * BuiltIn member decorations are valid only for Input and Output variables.
   */
  spv::Id var_main_rectangle_per_vertex_;
  /** One per user clip plane plus 1 cull distance for vertex killing. */
  static constexpr size_t kMaxRectanglePerVertexDistances = 8;
  /**
   * float[3] per clip and cull distance, VS rectangle list fallback only. Kept separate from
   * float[3][count] so the ArrayStride decorated gl_PerVertex member type stays out of Function
   * storage.
   */
  std::array<spv::Id, kMaxRectanglePerVertexDistances> var_main_rectangle_clip_distances_;
  std::array<spv::Id, kMaxRectanglePerVertexDistances> var_main_rectangle_cull_distances_;
  /** float4[3] per used interpolator, VS rectangle list fallback only. */
  std::array<spv::Id, xenos::kMaxInterpolators> var_main_rectangle_interpolators_;
  /** float3 special exports, VS only. */
  spv::Id var_main_point_size_edge_flag_kill_vertex_;
  /** bool, PS only when needed. */
  spv::Id var_main_kill_pixel_;
  /** float, PS with fragment shader interlock and depth export. */
  spv::Id var_main_fragment_depth_;
  /**
   * uint, PS writing color render targets with fragment shader interlock only. Tracks which color
   * buffers were written, since Direct3D 9 exports no unwritten color register.
   */
  spv::Id var_main_fsi_color_written_;
  /**
   * Loaded by FSI_LoadSampleMask, modifiable only on the outermost control flow level of main.
   * Bits 0:3 are the per sample coverage at the current stage, affected by gl_SampleMaskIn, early
   * or late depth and stencil, and alpha to coverage. Bits 4:7 are the depth write deferred mask
   * for samples whose early depth and stencil result must be written after the shader. Early
   * rejection of the pixel is possible when bits 0:3 and 4:7 are both zero.
   */
  spv::Id main_fsi_sample_mask_;
  /** Loaded by FSI_LoadEdramOffsets, including the depth render target base. */
  spv::Id main_fsi_address_depth_;
  /** Excludes the render target base. */
  spv::Id main_fsi_offset_32bpp_;
  spv::Id main_fsi_offset_64bpp_;
  /**
   * Loaded by FSI_DepthStencilTest for early depth and stencil, the values to write at the end of
   * the shader for samples in main_fsi_sample_mask_ not discarded after the early test.
   */
  std::array<spv::Id, 4> main_fsi_late_write_depth_stencil_;
  spv::Block* main_fsi_early_depth_stencil_execute_quad_merge_;
  spv::Block* main_loop_header_;
  spv::Block* main_loop_continue_;
  /**
   * Convergence of loop body exits before per iteration finalization in the rectangle list vertex
   * fallback, otherwise the loop merge too.
   */
  spv::Block* main_loop_merge_;
  /**
   * Loop merge for the rectangle list vertex fallback, otherwise the same as main_loop_merge_.
   */
  spv::Block* main_loop_exit_;
  spv::Id main_loop_pc_next_;
  spv::Block* main_switch_header_;
  std::unique_ptr<spv::Instruction> main_switch_op_;
  spv::Block* main_switch_merge_;
  std::vector<spv::Id> main_switch_next_pc_phi_operands_;

  /**
   * Block after the open exec bool constant or predicate conditional, not yet added to the
   * function.
   */
  spv::Block* cf_exec_conditional_merge_;
  /**
   * Block after the open instruction level predicate conditional, not yet added to the function.
   */
  spv::Block* cf_instruction_predicate_merge_;
  /**
   * With cf_exec_conditional_merge_ set, the bool constant number or kCfExecBoolConstantPredicate
   * when based on the predicate value.
   */
  uint32_t cf_exec_bool_constant_or_predicate_;
  static constexpr uint32_t kCfExecBoolConstantPredicate = UINT32_MAX;
  /** With cf_exec_conditional_merge_ set, the expected bool constant or predicate value. */
  bool cf_exec_condition_;
  /** With cf_instruction_predicate_merge_ set, the expected predicate value. */
  bool cf_instruction_predicate_condition_;
  /**
   * True when a `setp` occurred in the current exec before the current instruction, so the
   * instruction level predicate can differ from the exec level one and two execs with the same
   * predicate condition cannot merge.
   */
  bool cf_exec_predicate_written_;
};

}  // namespace rex::gpu
