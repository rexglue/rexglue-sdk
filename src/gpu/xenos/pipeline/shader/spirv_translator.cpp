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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <SPIRV/GLSL.std.450.h>
#include <format>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/spirv.h"
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/math.h>
#include <rex/string/buffer.h>

namespace rex::gpu {

SPIRVShaderTranslator::Features::Features(bool all)
    : spirv_version(all ? spv::Spv_1_5 : spv::Spv_1_0),
      max_storage_buffer_range(all ? UINT32_MAX : (128 * 1024 * 1024)),
      full_draw_index_uint32(all),
      vertex_pipeline_stores_and_atomics(all),
      fragment_stores_and_atomics(all),
      clip_distance(all),
      cull_distance(all),
      image_view_format_swizzle(all),
      signed_zero_inf_nan_preserve_float32(all),
      denorm_flush_to_zero_float32(all),
      rounding_mode_rte_float32(all),
      fragment_shader_sample_interlock(all),
      demote_to_helper_invocation(all),
      sample_rate_shading(all) {}

SPIRVShaderTranslator::Features::Features(const gpu::vulkan::VulkanDevice* const vulkan_device)
    : max_storage_buffer_range(vulkan_device->GetProperties().maxStorageBufferRange),
      full_draw_index_uint32(vulkan_device->GetProperties().fullDrawIndexUint32),
      vertex_pipeline_stores_and_atomics(
          vulkan_device->GetProperties().vertexPipelineStoresAndAtomics),
      fragment_stores_and_atomics(vulkan_device->GetProperties().fragmentStoresAndAtomics),
      clip_distance(vulkan_device->GetProperties().shaderClipDistance),
      cull_distance(vulkan_device->GetProperties().shaderCullDistance),
      image_view_format_swizzle(vulkan_device->GetProperties().imageViewFormatSwizzle),
      signed_zero_inf_nan_preserve_float32(
          vulkan_device->GetProperties().shaderSignedZeroInfNanPreserveFloat32),
      denorm_flush_to_zero_float32(vulkan_device->GetProperties().shaderDenormFlushToZeroFloat32),
      rounding_mode_rte_float32(vulkan_device->GetProperties().shaderRoundingModeRTEFloat32),
      fragment_shader_sample_interlock(
          vulkan_device->GetProperties().fragmentShaderSampleInterlock),
      demote_to_helper_invocation(vulkan_device->GetProperties().shaderDemoteToHelperInvocation),
      sample_rate_shading(vulkan_device->GetProperties().sampleRateShading) {
  const u32 vulkan_api_version = vulkan_device->GetProperties().apiVersion;
  if (vulkan_api_version >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    spirv_version = spv::Spv_1_5;
  } else if (vulkan_device->GetExtensions().ext_1_2_KHR_spirv_1_4) {
    spirv_version = spv::Spv_1_4;
  } else if (vulkan_api_version >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    spirv_version = spv::Spv_1_3;
  } else {
    spirv_version = spv::Spv_1_0;
  }
}

u64 SPIRVShaderTranslator::GetDefaultVertexShaderModification(
    u32 dynamic_addressable_register_count,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  Modification shader_modification;
  shader_modification.vertex.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  shader_modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  return shader_modification.value;
}

u64 SPIRVShaderTranslator::GetDefaultPixelShaderModification(
    u32 dynamic_addressable_register_count) const {
  Modification shader_modification;
  shader_modification.pixel.dynamic_addressable_register_count = dynamic_addressable_register_count;
  return shader_modification.value;
}

std::vector<u8> SPIRVShaderTranslator::CreateDepthOnlyFragmentShader(
    Modification::DepthStencilMode depth_stencil_mode) {
  is_depth_only_fragment_shader_ = true;
  Shader shader(xenos::ShaderType::kPixel, 0, nullptr, 0);
  StringBuffer instruction_disassembly_buffer;
  shader.AnalyzeUcode(instruction_disassembly_buffer);
  Modification modification(GetDefaultPixelShaderModification(0));
  modification.pixel.depth_stencil_mode = depth_stencil_mode;
  Shader::Translation& translation = *shader.GetOrCreateTranslation(modification.value);
  TranslateAnalyzedShader(translation);
  is_depth_only_fragment_shader_ = false;
  return translation.GetTranslatedBinary();
}

void SPIRVShaderTranslator::Reset() {
  ShaderTranslator::Reset();

  builder_.reset();

  uniform_float_constants_ = spv::NoResult;

  input_vertex_index_ = spv::NoResult;
  input_primitive_id_ = spv::NoResult;
  input_tess_coord_ = spv::NoResult;
  input_patch_control_point_indices_ = spv::NoResult;
  input_point_coordinates_ = spv::NoResult;
  input_fragment_coordinates_ = spv::NoResult;
  input_front_facing_ = spv::NoResult;
  input_sample_id_ = spv::NoResult;
  input_sample_mask_ = spv::NoResult;
  std::fill(input_output_interpolators_.begin(), input_output_interpolators_.end(), spv::NoResult);
  output_point_coordinates_ = spv::NoResult;
  output_point_size_ = spv::NoResult;
  output_per_vertex_member_count_ = 0;
  output_per_vertex_member_clip_distance_ = UINT32_MAX;
  output_per_vertex_member_cull_distance_ = UINT32_MAX;
  type_output_per_vertex_ = spv::NoResult;
  output_per_vertex_ = spv::NoResult;
  output_fragment_depth_ = spv::NoResult;
  output_fragment_sample_mask_ = spv::NoResult;

  sampler_bindings_.clear();
  texture_bindings_.clear();

  main_interface_.clear();
  var_main_registers_ = spv::NoResult;
  var_main_memexport_address_ = spv::NoResult;
  std::ranges::fill(var_main_memexport_data_, spv::NoResult);
  var_main_memexport_data_written_ = spv::NoResult;
  main_memexport_allowed_ = spv::NoResult;
  var_main_rectangle_guest_vertex_index_ = spv::NoResult;
  var_main_rectangle_per_vertex_ = spv::NoResult;
  std::fill(var_main_rectangle_clip_distances_.begin(), var_main_rectangle_clip_distances_.end(),
            spv::NoResult);
  std::fill(var_main_rectangle_cull_distances_.begin(), var_main_rectangle_cull_distances_.end(),
            spv::NoResult);
  std::fill(var_main_rectangle_interpolators_.begin(), var_main_rectangle_interpolators_.end(),
            spv::NoResult);
  var_main_point_size_edge_flag_kill_vertex_ = spv::NoResult;
  var_main_kill_pixel_ = spv::NoResult;
  var_main_fragment_depth_ = spv::NoResult;
  var_main_fsi_color_written_ = spv::NoResult;

  main_switch_op_.reset();
  main_switch_next_pc_phi_operands_.clear();
  main_loop_exit_ = nullptr;

  cf_exec_conditional_merge_ = nullptr;
  cf_instruction_predicate_merge_ = nullptr;
}

u32 SPIRVShaderTranslator::GetModificationRegisterCount() const {
  Modification modification = GetSPIRVShaderModification();
  return IsVertexShader() ? modification.vertex.dynamic_addressable_register_count
                          : modification.pixel.dynamic_addressable_register_count;
}

void SPIRVShaderTranslator::StartTranslation() {
  builder_ = std::make_unique<SPIRVBuilder>(features_.spirv_version, (kSPIRVMagicToolId << 16) | 1,
                                            nullptr);

  builder_->addCapability(IsSPIRVTessEvalShader() ? spv::CapabilityTessellation
                                                  : spv::CapabilityShader);
  if (features_.spirv_version < spv::Spv_1_4) {
    if (features_.signed_zero_inf_nan_preserve_float32 || features_.denorm_flush_to_zero_float32 ||
        features_.rounding_mode_rte_float32) {
      builder_->addExtension("SPV_KHR_float_controls");
    }
  }
  ext_inst_glsl_std_450_ = builder_->import("GLSL.std.450");
  builder_->setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder_->setSource(spv::SourceLanguageUnknown, 0);

  type_void_ = builder_->makeVoidType();
  type_bool = builder_->makeBoolType();
  type_bool2 = builder_->makeVectorType(type_bool, 2);
  type_bool3 = builder_->makeVectorType(type_bool, 3);
  type_bool4 = builder_->makeVectorType(type_bool, 4);
  type_int = builder_->makeIntType(32);
  type_int2 = builder_->makeVectorType(type_int, 2);
  type_int3 = builder_->makeVectorType(type_int, 3);
  type_int4 = builder_->makeVectorType(type_int, 4);
  type_uint = builder_->makeUintType(32);
  type_uint2 = builder_->makeVectorType(type_uint, 2);
  type_uint3 = builder_->makeVectorType(type_uint, 3);
  type_uint4 = builder_->makeVectorType(type_uint, 4);
  type_float = builder_->makeFloatType(32);
  type_float2 = builder_->makeVectorType(type_float, 2);
  type_float3 = builder_->makeVectorType(type_float, 3);
  type_float4 = builder_->makeVectorType(type_float, 4);

  const_int_0_ = builder_->makeIntConstant(0);
  id_vector_temp_.clear();
  for (u32 i = 0; i < 4; ++i) {
    id_vector_temp_.push_back(const_int_0_);
  }
  const_int4_0_ = builder_->makeCompositeConstant(type_int4, id_vector_temp_);
  const_uint_0_ = builder_->makeUintConstant(0);
  id_vector_temp_.clear();
  for (u32 i = 0; i < 4; ++i) {
    id_vector_temp_.push_back(const_uint_0_);
  }
  const_uint4_0_ = builder_->makeCompositeConstant(type_uint4, id_vector_temp_);
  const_float_0 = builder_->makeFloatConstant(0.0f);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(const_float_0);
  for (u32 i = 1; i < 4; ++i) {
    id_vector_temp_.push_back(const_float_0);
    const_float_vectors_0[i] =
        builder_->makeCompositeConstant(type_float_vectors[i], id_vector_temp_);
  }
  const_float_1 = builder_->makeFloatConstant(1.0f);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(const_float_1);
  for (u32 i = 1; i < 4; ++i) {
    id_vector_temp_.push_back(const_float_1);
    const_float_vectors_1[i] =
        builder_->makeCompositeConstant(type_float_vectors[i], id_vector_temp_);
  }
  id_vector_temp_.clear();
  id_vector_temp_.push_back(const_float_0);
  id_vector_temp_.push_back(const_float_1);
  const_float2_0_1_ = builder_->makeCompositeConstant(type_float2, id_vector_temp_);

  struct SystemConstant {
    const char* name;
    size_t offset;
    spv::Id type;
  };
  spv::Id type_float4_array_4 =
      builder_->makeArrayType(type_float4, builder_->makeUintConstant(4), sizeof(f32) * 4);
  builder_->addDecoration(type_float4_array_4, spv::DecorationArrayStride, sizeof(f32) * 4);
  spv::Id type_float4_array_6 =
      builder_->makeArrayType(type_float4, builder_->makeUintConstant(6), sizeof(f32) * 4);
  builder_->addDecoration(type_float4_array_6, spv::DecorationArrayStride, sizeof(f32) * 4);
  spv::Id type_uint4_array_2 =
      builder_->makeArrayType(type_uint4, builder_->makeUintConstant(2), sizeof(u32) * 4);
  builder_->addDecoration(type_uint4_array_2, spv::DecorationArrayStride, sizeof(u32) * 4);
  spv::Id type_uint4_array_4 =
      builder_->makeArrayType(type_uint4, builder_->makeUintConstant(4), sizeof(u32) * 4);
  builder_->addDecoration(type_uint4_array_4, spv::DecorationArrayStride, sizeof(u32) * 4);
  const SystemConstant system_constants[] = {
      {"flags", offsetof(SystemConstants, flags), type_uint},
      {"vertex_index_load_address", offsetof(SystemConstants, vertex_index_load_address),
       type_uint},
      {"vertex_index_endian", offsetof(SystemConstants, vertex_index_endian), type_uint},
      {"line_loop_closing_index", offsetof(SystemConstants, line_loop_closing_index), type_uint},
      {"vertex_base_index", offsetof(SystemConstants, vertex_base_index), type_int},
      {"vertex_index_reset", offsetof(SystemConstants, vertex_index_reset), type_uint},
      {"compute_memexport_vertex_count", offsetof(SystemConstants, compute_memexport_vertex_count),
       type_uint},
      {"user_clip_planes", offsetof(SystemConstants, user_clip_planes), type_float4_array_6},
      {"vertex_index_min", offsetof(SystemConstants, vertex_index_min), type_uint},
      {"vertex_index_max", offsetof(SystemConstants, vertex_index_max), type_uint},
      {"ndc_scale", offsetof(SystemConstants, ndc_scale), type_float3},
      {"point_vertex_diameter_min", offsetof(SystemConstants, point_vertex_diameter_min),
       type_float},
      {"ndc_offset", offsetof(SystemConstants, ndc_offset), type_float3},
      {"point_vertex_diameter_max", offsetof(SystemConstants, point_vertex_diameter_max),
       type_float},
      {"point_constant_diameter", offsetof(SystemConstants, point_constant_diameter), type_float2},
      {"point_screen_diameter_to_ndc_radius",
       offsetof(SystemConstants, point_screen_diameter_to_ndc_radius), type_float2},
      {"textures_resolution_scaled", offsetof(SystemConstants, textures_resolution_scaled),
       type_uint},
      {"texture_swizzled_signs", offsetof(SystemConstants, texture_swizzled_signs),
       type_uint4_array_2},
      {"texture_swizzles", offsetof(SystemConstants, texture_swizzles), type_uint4_array_4},
      {"alpha_test_reference", offsetof(SystemConstants, alpha_test_reference), type_float},
      {"alpha_to_mask", offsetof(SystemConstants, alpha_to_mask), type_uint},
      {"edram_32bpp_tile_pitch_dwords_scaled",
       offsetof(SystemConstants, edram_32bpp_tile_pitch_dwords_scaled), type_uint},
      {"edram_depth_base_dwords_scaled", offsetof(SystemConstants, edram_depth_base_dwords_scaled),
       type_uint},
      {"color_exp_bias", offsetof(SystemConstants, color_exp_bias), type_float4},
      {"edram_poly_offset_front_scale", offsetof(SystemConstants, edram_poly_offset_front_scale),
       type_float},
      {"edram_poly_offset_back_scale", offsetof(SystemConstants, edram_poly_offset_back_scale),
       type_float},
      {"edram_poly_offset_front_offset", offsetof(SystemConstants, edram_poly_offset_front_offset),
       type_float},
      {"edram_poly_offset_back_offset", offsetof(SystemConstants, edram_poly_offset_back_offset),
       type_float},
      {"edram_stencil_front", offsetof(SystemConstants, edram_stencil_front), type_uint2},
      {"edram_stencil_back", offsetof(SystemConstants, edram_stencil_back), type_uint2},
      {"edram_rt_base_dwords_scaled", offsetof(SystemConstants, edram_rt_base_dwords_scaled),
       type_uint4},
      {"edram_rt_format_flags", offsetof(SystemConstants, edram_rt_format_flags), type_uint4},
      {"edram_rt_blend_factors_ops", offsetof(SystemConstants, edram_rt_blend_factors_ops),
       type_uint4},
      {"edram_rt_keep_mask", offsetof(SystemConstants, edram_rt_keep_mask), type_uint4_array_2},
      {"edram_rt_clamp", offsetof(SystemConstants, edram_rt_clamp), type_float4_array_4},
      {"edram_blend_constant", offsetof(SystemConstants, edram_blend_constant), type_float4},
  };
  id_vector_temp_.clear();
  id_vector_temp_.reserve(rex::CountOf(system_constants));
  for (const auto& constant : system_constants) {
    id_vector_temp_.push_back(constant.type);
  }
  spv::Id type_system_constants = builder_->makeStructType(id_vector_temp_, "XeSystemConstants");
  for (size_t i = 0; i < rex::CountOf(system_constants); ++i) {
    const SystemConstant& system_constant = system_constants[i];
    builder_->addMemberName(type_system_constants, static_cast<int>(i), system_constant.name);
    builder_->addMemberDecoration(type_system_constants, static_cast<unsigned int>(i),
                                  spv::DecorationOffset, int(system_constant.offset));
  }
  builder_->addDecoration(type_system_constants, spv::DecorationBlock);
  uniform_system_constants_ =
      builder_->createVariable(spv::NoPrecision, spv::StorageClassUniform, type_system_constants,
                               "xe_uniform_system_constants");
  builder_->addDecoration(uniform_system_constants_, spv::DecorationDescriptorSet,
                          int(kDescriptorSetConstants));
  builder_->addDecoration(uniform_system_constants_, spv::DecorationBinding,
                          int(kConstantBufferSystem));
  if (features_.spirv_version >= spv::Spv_1_4) {
    main_interface_.push_back(uniform_system_constants_);
  }

  bool memexport_used = IsMemoryExportUsed();

  if (!is_depth_only_fragment_shader_) {
    u32 float_constant_count = GetCurrentShader().GetConstantRegisterMap().float_count;
    if (float_constant_count) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeArrayType(
          type_float4, builder_->makeUintConstant(float_constant_count), sizeof(f32) * 4));
      // makeArrayType deduplicates by stride only, so apply the ArrayStride decoration.
      builder_->addDecoration(id_vector_temp_.back(), spv::DecorationArrayStride, sizeof(f32) * 4);
      spv::Id type_float_constants = builder_->makeStructType(id_vector_temp_, "XeFloatConstants");
      builder_->addMemberName(type_float_constants, 0, "float_constants");
      builder_->addMemberDecoration(type_float_constants, 0, spv::DecorationOffset, 0);
      builder_->addDecoration(type_float_constants, spv::DecorationBlock);
      uniform_float_constants_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassUniform, type_float_constants,
                                   "xe_uniform_float_constants");
      builder_->addDecoration(uniform_float_constants_, spv::DecorationDescriptorSet,
                              int(kDescriptorSetConstants));
      builder_->addDecoration(
          uniform_float_constants_, spv::DecorationBinding,
          int(IsPixelShader() ? kConstantBufferFloatPixel : kConstantBufferFloatVertex));
      if (features_.spirv_version >= spv::Spv_1_4) {
        main_interface_.push_back(uniform_float_constants_);
      }
    }

    // std140 pads scalar array elements to 16 bytes, so use arrays of 4-component vectors.
    id_vector_temp_.clear();
    id_vector_temp_.push_back(
        builder_->makeArrayType(type_uint4, builder_->makeUintConstant(2), sizeof(u32) * 4));
    builder_->addDecoration(id_vector_temp_.back(), spv::DecorationArrayStride, sizeof(u32) * 4);
    id_vector_temp_.push_back(
        builder_->makeArrayType(type_uint4, builder_->makeUintConstant(8), sizeof(u32) * 4));
    builder_->addDecoration(id_vector_temp_.back(), spv::DecorationArrayStride, sizeof(u32) * 4);
    spv::Id type_bool_loop_constants =
        builder_->makeStructType(id_vector_temp_, "XeBoolLoopConstants");
    builder_->addMemberName(type_bool_loop_constants, 0, "bool_constants");
    builder_->addMemberDecoration(type_bool_loop_constants, 0, spv::DecorationOffset, 0);
    builder_->addMemberName(type_bool_loop_constants, 1, "loop_constants");
    builder_->addMemberDecoration(type_bool_loop_constants, 1, spv::DecorationOffset,
                                  sizeof(u32) * 8);
    builder_->addDecoration(type_bool_loop_constants, spv::DecorationBlock);
    uniform_bool_loop_constants_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassUniform,
                                 type_bool_loop_constants, "xe_uniform_bool_loop_constants");
    builder_->addDecoration(uniform_bool_loop_constants_, spv::DecorationDescriptorSet,
                            int(kDescriptorSetConstants));
    builder_->addDecoration(uniform_bool_loop_constants_, spv::DecorationBinding,
                            int(kConstantBufferBoolLoop));
    if (features_.spirv_version >= spv::Spv_1_4) {
      main_interface_.push_back(uniform_bool_loop_constants_);
    }

    // 32 x 6 uints packed in std140 as 4-component vectors.
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeArrayType(
        type_uint4, builder_->makeUintConstant(32 * 6 / 4), sizeof(u32) * 4));
    builder_->addDecoration(id_vector_temp_.back(), spv::DecorationArrayStride, sizeof(u32) * 4);
    spv::Id type_fetch_constants = builder_->makeStructType(id_vector_temp_, "XeFetchConstants");
    builder_->addMemberName(type_fetch_constants, 0, "fetch_constants");
    builder_->addMemberDecoration(type_fetch_constants, 0, spv::DecorationOffset, 0);
    builder_->addDecoration(type_fetch_constants, spv::DecorationBlock);
    uniform_fetch_constants_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassUniform, type_fetch_constants,
                                 "xe_uniform_fetch_constants");
    builder_->addDecoration(uniform_fetch_constants_, spv::DecorationDescriptorSet,
                            int(kDescriptorSetConstants));
    builder_->addDecoration(uniform_fetch_constants_, spv::DecorationBinding,
                            int(kConstantBufferFetch));
    if (features_.spirv_version >= spv::Spv_1_4) {
      main_interface_.push_back(uniform_fetch_constants_);
    }

    // Shared memory uint[] SSBOs of 128 MB or larger packed in std430 without padding.
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeRuntimeArray(type_uint));
    builder_->addDecoration(id_vector_temp_.back(), spv::DecorationArrayStride, sizeof(u32));
    spv::Id type_shared_memory = builder_->makeStructType(id_vector_temp_, "XeSharedMemory");
    builder_->addMemberName(type_shared_memory, 0, "shared_memory");
    builder_->addMemberDecoration(type_shared_memory, 0, spv::DecorationRestrict);
    if (!memexport_used) {
      builder_->addMemberDecoration(type_shared_memory, 0, spv::DecorationNonWritable);
    }
    builder_->addMemberDecoration(type_shared_memory, 0, spv::DecorationOffset, 0);
    builder_->addDecoration(type_shared_memory, features_.spirv_version >= spv::Spv_1_3
                                                    ? spv::DecorationBlock
                                                    : spv::DecorationBufferBlock);
    unsigned int shared_memory_binding_count = 1 << GetSharedMemoryStorageBufferCountLog2();
    if (shared_memory_binding_count > 1) {
      type_shared_memory = builder_->makeArrayType(
          type_shared_memory, builder_->makeUintConstant(shared_memory_binding_count), 0);
    }
    buffers_shared_memory_ = builder_->createVariable(spv::NoPrecision,
                                                      features_.spirv_version >= spv::Spv_1_3
                                                          ? spv::StorageClassStorageBuffer
                                                          : spv::StorageClassUniform,
                                                      type_shared_memory, "xe_shared_memory");
    builder_->addDecoration(buffers_shared_memory_, spv::DecorationDescriptorSet,
                            int(kDescriptorSetSharedMemoryAndEDRAM));
    builder_->addDecoration(buffers_shared_memory_, spv::DecorationBinding, 0);
    if (features_.spirv_version >= spv::Spv_1_4) {
      main_interface_.push_back(buffers_shared_memory_);
    }
  }

  if (IsVertexShader()) {
    StartVertexOrTessEvalShaderBeforeMain();
  } else if (IsPixelShader()) {
    StartFragmentShaderBeforeMain();
  }

  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* function_main_entry;
  function_main_ =
      builder_->makeFunctionEntry(spv::NoPrecision, type_void_, "main", main_param_types,
                                  main_precisions, &function_main_entry);

  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantFlags));
  main_system_constant_flags_ =
      builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants_, id_vector_temp_),
                           spv::NoPrecision);

  if (!is_depth_only_fragment_shader_) {
    var_main_predicate_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_bool,
                                 "xe_var_predicate", builder_->makeBoolConstant(false));
    var_main_loop_count_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_uint4,
                                 "xe_var_loop_count", const_uint4_0_);
    var_main_address_register_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_int,
                                 "xe_var_address_register", const_int_0_);
    var_main_loop_address_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_int4,
                                 "xe_var_loop_address", const_int4_0_);
    var_main_previous_scalar_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float,
                                 "xe_var_previous_scalar", const_float_0);
    var_main_vfetch_address_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_int,
                                 "xe_var_vfetch_address", const_int_0_);
    var_main_tfetch_lod_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction,
                                                    type_float, "xe_var_tfetch_lod", const_float_0);
    var_main_tfetch_gradients_h_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float3,
                                 "xe_var_tfetch_gradients_h", const_float3_0);
    var_main_tfetch_gradients_v_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float3,
                                 "xe_var_tfetch_gradients_v", const_float3_0);
    if (GetRegisterCount()) {
      spv::Id type_register_array =
          builder_->makeArrayType(type_float4, builder_->makeUintConstant(GetRegisterCount()), 1);
      var_main_registers_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction,
                                                     type_register_array, "xe_var_registers");
    }
    if (IsSPIRVRectangleListVertexLoopEnabled()) {
      Modification shader_modification = GetSPIRVShaderModification();
      var_main_rectangle_guest_vertex_index_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_uint,
                                   "xe_var_rectangle_guest_vertex_index", const_uint_0_);
      // BuiltIn and ArrayStride are valid only in Input and Output storage, so use local arrays.
      spv::Id type_rectangle_per_vertex_array =
          builder_->makeArrayType(type_float4, builder_->makeUintConstant(3), 0);
      var_main_rectangle_per_vertex_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction,
                                   type_rectangle_per_vertex_array, "xe_var_rectangle_per_vertex");
      spv::Id type_rectangle_distance_array =
          builder_->makeArrayType(type_float, builder_->makeUintConstant(3), 0);
      u32 clip_distance_count = shader_modification.GetVertexClipDistanceCount();
      REX_ASSERT(clip_distance_count <= kMaxRectanglePerVertexDistances);
      for (u32 i = 0; i < clip_distance_count; ++i) {
        var_main_rectangle_clip_distances_[i] = builder_->createVariable(
            spv::NoPrecision, spv::StorageClassFunction, type_rectangle_distance_array,
            std::format("xe_var_rectangle_clip_distance_{}", i).c_str());
      }
      u32 cull_distance_count = shader_modification.GetVertexCullDistanceCount();
      REX_ASSERT(cull_distance_count <= kMaxRectanglePerVertexDistances);
      for (u32 i = 0; i < cull_distance_count; ++i) {
        var_main_rectangle_cull_distances_[i] = builder_->createVariable(
            spv::NoPrecision, spv::StorageClassFunction, type_rectangle_distance_array,
            std::format("xe_var_rectangle_cull_distance_{}", i).c_str());
      }
      u32 interpolators_remaining = GetModificationInterpolatorMask();
      u32 interpolator_index;
      while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
        interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
        spv::Id type_rectangle_interpolator_array =
            builder_->makeArrayType(type_float4, builder_->makeUintConstant(3), 0);
        var_main_rectangle_interpolators_[interpolator_index] = builder_->createVariable(
            spv::NoPrecision, spv::StorageClassFunction, type_rectangle_interpolator_array,
            std::format("xe_var_rectangle_interpolator_{}", interpolator_index).c_str());
      }
    }
    if (memexport_used) {
      var_main_memexport_address_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float4,
                                   "xe_var_memexport_address", const_float4_0);
      u8 memexport_em_remaining = GetCurrentShader().GetMemExportEMWritten();
      u32 memexport_em_index;
      while (rex::bit_scan_forward(memexport_em_remaining, &memexport_em_index)) {
        memexport_em_remaining &= ~(static_cast<u8>(1) << memexport_em_index);
        var_main_memexport_data_[memexport_em_index] = builder_->createVariable(
            spv::NoPrecision, spv::StorageClassFunction, type_float4,
            std::format("xe_var_memexport_data_{}", memexport_em_index).c_str(), const_float4_0);
      }
      var_main_memexport_data_written_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_uint,
                                   "xe_var_memexport_data_written", const_uint_0_);
    }
  }

  if (IsVertexShader()) {
    StartVertexOrTessEvalShaderInMain();
  } else if (IsPixelShader()) {
    StartFragmentShaderInMain();
  }

  if (is_depth_only_fragment_shader_) {
    return;
  }

  spv::Block& main_loop_pre_header = *builder_->getBuildPoint();
  main_loop_header_ = &builder_->makeNewBlock();
  spv::Block& main_loop_body = builder_->makeNewBlock();
  // Blocks must appear before all blocks they dominate, so this block is added later.
  main_loop_continue_ = new spv::Block(builder_->getUniqueId(), *function_main_);
  main_loop_merge_ = new spv::Block(builder_->getUniqueId(), *function_main_);
  main_loop_exit_ = main_loop_merge_;
  if (IsSPIRVRectangleListVertexLoopEnabled()) {
    main_loop_exit_ = new spv::Block(builder_->getUniqueId(), *function_main_);
  }
  builder_->createBranch(main_loop_header_);

  bool has_main_switch = !GetCurrentShader().GetLabelAddresses().empty();

  builder_->setBuildPoint(main_loop_header_);
  spv::Id main_loop_pc_current = spv::NoResult;
  if (has_main_switch) {
    // OpPhi must be the first instruction in the block.
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_int_0_);
    id_vector_temp_.push_back(main_loop_pre_header.getId());
    main_loop_pc_next_ = builder_->getUniqueId();
    id_vector_temp_.push_back(main_loop_pc_next_);
    id_vector_temp_.push_back(main_loop_continue_->getId());
    main_loop_pc_current = builder_->createOp(spv::OpPhi, type_int, id_vector_temp_);
  }
  uint_vector_temp_.clear();
  builder_->createLoopMerge(main_loop_exit_, main_loop_continue_, spv::LoopControlDontUnrollMask,
                            uint_vector_temp_);
  builder_->createBranch(&main_loop_body);

  builder_->setBuildPoint(&main_loop_body);
  if (has_main_switch) {
    main_switch_header_ = builder_->getBuildPoint();
    main_switch_merge_ = new spv::Block(builder_->getUniqueId(), *function_main_);
    builder_->createSelectionMerge(main_switch_merge_, spv::SelectionControlDontFlattenMask);
    main_switch_op_ = std::make_unique<spv::Instruction>(spv::OpSwitch);
    main_switch_op_->addIdOperand(main_loop_pc_current);
    main_switch_op_->addIdOperand(main_switch_merge_->getId());
    // The default case, the merge here, must have the header as a predecessor.
    main_switch_merge_->addPredecessor(main_switch_header_);
    spv::Block* main_switch_case_0_block = new spv::Block(builder_->getUniqueId(), *function_main_);
    main_switch_op_->addImmediateOperand(0);
    main_switch_op_->addIdOperand(main_switch_case_0_block->getId());
    // Every switch case must have the OpSelectionMerge and OpSwitch block as a predecessor.
    main_switch_case_0_block->addPredecessor(main_switch_header_);
    function_main_->addBlock(main_switch_case_0_block);
    builder_->setBuildPoint(main_switch_case_0_block);
  }
}

std::vector<u8> SPIRVShaderTranslator::CompleteTranslation() {
  bool rectangle_vertex_loop = IsSPIRVRectangleListVertexLoopEnabled();
  if (!is_depth_only_fragment_shader_) {
    CloseExecConditionals();
    bool has_main_switch = !GetCurrentShader().GetLabelAddresses().empty();
    if (!builder_->getBuildPoint()->isTerminated()) {
      builder_->createBranch(has_main_switch ? main_switch_merge_ : main_loop_merge_);
    }
    if (has_main_switch) {
      builder_->setBuildPoint(main_switch_header_);
      builder_->getBuildPoint()->addInstruction(std::move(main_switch_op_));
      function_main_->addBlock(main_switch_merge_);
      builder_->setBuildPoint(main_switch_merge_);
      builder_->createBranch(main_loop_merge_);
    }

    function_main_->addBlock(main_loop_continue_);
    builder_->setBuildPoint(main_loop_continue_);
    if (has_main_switch) {
      // OpPhi must be first in the block, and program counter -1 selects no switch case.
      if (main_switch_next_pc_phi_operands_.empty()) {
        main_switch_next_pc_phi_operands_.push_back(builder_->makeIntConstant(-1));
      }
      std::unique_ptr<spv::Instruction> main_loop_pc_next_op = std::make_unique<spv::Instruction>(
          main_loop_pc_next_, type_int,
          main_switch_next_pc_phi_operands_.size() >= 2 ? spv::OpPhi : spv::OpCopyObject);
      for (spv::Id operand : main_switch_next_pc_phi_operands_) {
        main_loop_pc_next_op->addIdOperand(operand);
      }
      builder_->getBuildPoint()->addInstruction(std::move(main_loop_pc_next_op));
    }
    builder_->createBranch(main_loop_header_);

    function_main_->addBlock(main_loop_merge_);
    builder_->setBuildPoint(main_loop_merge_);
    if (rectangle_vertex_loop) {
      Modification shader_modification = GetSPIRVShaderModification();
      u32 clip_distance_count = shader_modification.GetVertexClipDistanceCount();
      u32 cull_distance_count = shader_modification.GetVertexCullDistanceCount();

      CompleteVertexOrTessEvalShaderInMain();

      spv::Id rectangle_guest_vertex_index =
          builder_->createLoad(var_main_rectangle_guest_vertex_index_, spv::NoPrecision);
      spv::Id rectangle_guest_vertex_index_int =
          builder_->createUnaryOp(spv::OpBitcast, type_int, rectangle_guest_vertex_index);

      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kOutputPerVertexMemberPosition));
      spv::Id rectangle_vertex_position = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassOutput, output_per_vertex_, id_vector_temp_),
          spv::NoPrecision);
      id_vector_temp_.clear();
      id_vector_temp_.push_back(rectangle_guest_vertex_index_int);
      builder_->createStore(
          rectangle_vertex_position,
          builder_->createAccessChain(spv::StorageClassFunction, var_main_rectangle_per_vertex_,
                                      id_vector_temp_));
      auto store_rectangle_distance_members =
          [this, rectangle_guest_vertex_index_int](
              u32 member_index, u32 component_count,
              const std::array<spv::Id, kMaxRectanglePerVertexDistances>& variables) {
            for (u32 i = 0; i < component_count; ++i) {
              id_vector_temp_.clear();
              id_vector_temp_.push_back(builder_->makeIntConstant(int(member_index)));
              id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
              spv::Id value = builder_->createLoad(
                  builder_->createAccessChain(spv::StorageClassOutput, output_per_vertex_,
                                              id_vector_temp_),
                  spv::NoPrecision);
              id_vector_temp_.clear();
              id_vector_temp_.push_back(rectangle_guest_vertex_index_int);
              builder_->createStore(
                  value, builder_->createAccessChain(spv::StorageClassFunction, variables[i],
                                                     id_vector_temp_));
            }
          };
      if (clip_distance_count && output_per_vertex_member_clip_distance_ != UINT32_MAX) {
        store_rectangle_distance_members(output_per_vertex_member_clip_distance_,
                                         clip_distance_count, var_main_rectangle_clip_distances_);
      }
      if (cull_distance_count && output_per_vertex_member_cull_distance_ != UINT32_MAX) {
        store_rectangle_distance_members(output_per_vertex_member_cull_distance_,
                                         cull_distance_count, var_main_rectangle_cull_distances_);
      }
      u32 interpolators_remaining = GetModificationInterpolatorMask();
      u32 interpolator_index;
      while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
        interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
        id_vector_temp_.clear();
        id_vector_temp_.push_back(rectangle_guest_vertex_index_int);
        builder_->createStore(
            builder_->createLoad(input_output_interpolators_[interpolator_index], spv::NoPrecision),
            builder_->createAccessChain(spv::StorageClassFunction,
                                        var_main_rectangle_interpolators_[interpolator_index],
                                        id_vector_temp_));
      }

      spv::Id rectangle_guest_vertex_is_last = builder_->createBinOp(
          spv::OpIEqual, type_bool, rectangle_guest_vertex_index, builder_->makeUintConstant(2));
      SPIRVBuilder::IfBuilder if_not_last_guest_vertex(
          builder_->createUnaryOp(spv::OpLogicalNot, type_bool, rectangle_guest_vertex_is_last),
          spv::SelectionControlDontFlattenMask, *builder_);
      {
        spv::Id rectangle_guest_vertex_index_next = builder_->createBinOp(
            spv::OpIAdd, type_uint, rectangle_guest_vertex_index, builder_->makeUintConstant(1));
        builder_->createStore(rectangle_guest_vertex_index_next,
                              var_main_rectangle_guest_vertex_index_);

        builder_->createStore(builder_->makeBoolConstant(false), var_main_predicate_);
        builder_->createStore(const_uint4_0_, var_main_loop_count_);
        builder_->createStore(const_int_0_, var_main_address_register_);
        builder_->createStore(const_int4_0_, var_main_loop_address_);
        builder_->createStore(const_float_0, var_main_previous_scalar_);
        builder_->createStore(const_int_0_, var_main_vfetch_address_);
        builder_->createStore(const_float_0, var_main_tfetch_lod_);
        builder_->createStore(const_float3_0, var_main_tfetch_gradients_h_);
        builder_->createStore(const_float3_0, var_main_tfetch_gradients_v_);
        for (u32 i = 0; i < GetRegisterCount(); ++i) {
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
          builder_->createStore(const_float4_0,
                                builder_->createAccessChain(spv::StorageClassFunction,
                                                            var_main_registers_, id_vector_temp_));
        }
        u32 iteration_interpolators_remaining = GetModificationInterpolatorMask();
        while (rex::bit_scan_forward(iteration_interpolators_remaining, &interpolator_index)) {
          iteration_interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
          builder_->createStore(const_float4_0, input_output_interpolators_[interpolator_index]);
        }
        if (var_main_point_size_edge_flag_kill_vertex_ != spv::NoResult) {
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeFloatConstant(-1.0f));
          id_vector_temp_.push_back(const_float_0);
          id_vector_temp_.push_back(const_float_0);
          builder_->createStore(builder_->makeCompositeConstant(type_float3, id_vector_temp_),
                                var_main_point_size_edge_flag_kill_vertex_);
        }
        if (IsMemoryExportUsed()) {
          ExportToMemory(GetCurrentShader().GetMemExportEMPotentiallyWrittenBeforeEnd());
          builder_->createStore(const_float4_0, var_main_memexport_address_);
          u8 memexport_em_remaining = GetCurrentShader().GetMemExportEMWritten();
          u32 memexport_em_index;
          while (rex::bit_scan_forward(memexport_em_remaining, &memexport_em_index)) {
            memexport_em_remaining &= ~(static_cast<u8>(1) << memexport_em_index);
            builder_->createStore(const_float4_0, var_main_memexport_data_[memexport_em_index]);
          }
          builder_->createStore(const_uint_0_, var_main_memexport_data_written_);
        }

        if (GetRegisterCount()) {
          spv::Id rectangle_two_triangle_strip_vertex_index =
              builder_->createUnaryOp(spv::OpBitcast, type_uint,
                                      builder_->createLoad(input_vertex_index_, spv::NoPrecision));
          spv::Id rectangle_primitive_index = builder_->createBinOp(
              spv::OpShiftRightLogical, type_uint, rectangle_two_triangle_strip_vertex_index,
              builder_->makeUintConstant(2));
          spv::Id rectangle_vertex_index = builder_->createBinOp(
              spv::OpIAdd, type_uint,
              builder_->createBinOp(spv::OpIMul, type_uint, rectangle_primitive_index,
                                    builder_->makeUintConstant(3)),
              rectangle_guest_vertex_index_next);
          spv::Id rectangle_load_vertex_index = builder_->createBinOp(
              spv::OpINotEqual, type_bool,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                    builder_->makeUintConstant(static_cast<unsigned int>(
                                        kSysFlagComputeOrPrimitiveVertexIndexLoad))),
              const_uint_0_);
          SPIRVBuilder::IfBuilder rectangle_load_vertex_index_if(
              rectangle_load_vertex_index, spv::SelectionControlDontFlattenMask, *builder_);
          spv::Id rectangle_loaded_vertex_index;
          {
            spv::Id rectangle_vertex_index_is_32bit = builder_->createBinOp(
                spv::OpINotEqual, type_bool,
                builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                      builder_->makeUintConstant(static_cast<unsigned int>(
                                          kSysFlagComputeOrPrimitiveVertexIndexLoad32Bit))),
                const_uint_0_);
            id_vector_temp_.clear();
            id_vector_temp_.push_back(
                builder_->makeIntConstant(kSystemConstantVertexIndexLoadAddress));
            spv::Id rectangle_vertex_index_address = builder_->createBinOp(
                spv::OpIAdd, type_uint,
                builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                                id_vector_temp_),
                    spv::NoPrecision),
                builder_->createBinOp(
                    spv::OpShiftLeftLogical, type_uint, rectangle_vertex_index,
                    builder_->createTriOp(spv::OpSelect, type_uint, rectangle_vertex_index_is_32bit,
                                          builder_->makeUintConstant(2),
                                          builder_->makeUintConstant(1))));
            rectangle_loaded_vertex_index = LoadUint32FromSharedMemory(
                builder_->createUnaryOp(spv::OpBitcast, type_int,
                                        builder_->createBinOp(spv::OpShiftRightLogical, type_uint,
                                                              rectangle_vertex_index_address,
                                                              builder_->makeUintConstant(2))));
            rectangle_loaded_vertex_index = builder_->createTriOp(
                spv::OpSelect, type_uint, rectangle_vertex_index_is_32bit,
                rectangle_loaded_vertex_index,
                builder_->createTriOp(
                    spv::OpBitFieldUExtract, type_uint, rectangle_loaded_vertex_index,
                    builder_->createBinOp(spv::OpShiftLeftLogical, type_uint,
                                          builder_->createBinOp(spv::OpBitwiseAnd, type_uint,
                                                                rectangle_vertex_index_address,
                                                                builder_->makeUintConstant(2)),
                                          builder_->makeUintConstant(3)),
                    builder_->makeUintConstant(16)));
            id_vector_temp_.clear();
            id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexEndian));
            rectangle_loaded_vertex_index = EndianSwap32Uint(
                rectangle_loaded_vertex_index,
                builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                                id_vector_temp_),
                    spv::NoPrecision));
          }
          rectangle_load_vertex_index_if.MakeEndIf();
          rectangle_vertex_index = rectangle_load_vertex_index_if.CreateMergePhi(
              rectangle_loaded_vertex_index, rectangle_vertex_index);

          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexBaseIndex));
          spv::Id rectangle_vertex_base_index = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision);
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexMin));
          spv::Id rectangle_vertex_index_min = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision);
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexMax));
          spv::Id rectangle_vertex_index_max = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision);
          rectangle_vertex_index = builder_->createBinOp(
              spv::OpIAdd, type_uint, rectangle_vertex_index,
              builder_->createUnaryOp(spv::OpBitcast, type_uint, rectangle_vertex_base_index));
          rectangle_vertex_index =
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, rectangle_vertex_index,
                                    builder_->makeUintConstant(xenos::kVertexIndexMask));
          rectangle_vertex_index = builder_->CreateTriBuiltinCall(
              type_uint, ext_inst_glsl_std_450_, GLSLstd450UClamp, rectangle_vertex_index,
              rectangle_vertex_index_min, rectangle_vertex_index_max);
          spv::Id rectangle_vertex_index_float =
              builder_->createUnaryOp(spv::OpConvertUToF, type_float, rectangle_vertex_index);
          id_vector_temp_.clear();
          id_vector_temp_.push_back(const_int_0_);
          id_vector_temp_.push_back(const_int_0_);
          builder_->createStore(rectangle_vertex_index_float,
                                builder_->createAccessChain(spv::StorageClassFunction,
                                                            var_main_registers_, id_vector_temp_));
        }

        if (has_main_switch) {
          main_switch_next_pc_phi_operands_.push_back(const_int_0_);
          main_switch_next_pc_phi_operands_.push_back(builder_->getBuildPoint()->getId());
        }
        builder_->createBranch(main_loop_continue_);
      }
      if_not_last_guest_vertex.MakeBeginElse(false);
      {
        spv::Id rectangle_host_vertex_in_primitive = builder_->createBinOp(
            spv::OpBitwiseAnd, type_uint,
            builder_->createUnaryOp(spv::OpBitcast, type_uint,
                                    builder_->createLoad(input_vertex_index_, spv::NoPrecision)),
            builder_->makeUintConstant(3));
        spv::Id rectangle_host_vertex_is_synthetic =
            builder_->createBinOp(spv::OpIEqual, type_bool, rectangle_host_vertex_in_primitive,
                                  builder_->makeUintConstant(3));
        spv::Id rectangle_host_vertex_is_0 = builder_->createBinOp(
            spv::OpIEqual, type_bool, rectangle_host_vertex_in_primitive, const_uint_0_);
        spv::Id rectangle_host_vertex_is_1 =
            builder_->createBinOp(spv::OpIEqual, type_bool, rectangle_host_vertex_in_primitive,
                                  builder_->makeUintConstant(1));

        auto load_rectangle_position_xy = [this](spv::Id vertex_index, u32 component) {
          id_vector_temp_.clear();
          id_vector_temp_.push_back(vertex_index);
          id_vector_temp_.push_back(builder_->makeIntConstant(int(component)));
          return builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassFunction, var_main_rectangle_per_vertex_,
                                          id_vector_temp_),
              spv::NoPrecision);
        };

        spv::Id const_int_0 = builder_->makeIntConstant(0);
        spv::Id const_int_1 = builder_->makeIntConstant(1);
        spv::Id const_int_2 = builder_->makeIntConstant(2);
        spv::Id const_int_3 = builder_->makeIntConstant(3);

        spv::Id edge_lengths[3];
        for (u32 i = 0; i < 3; ++i) {
          spv::Id edge_0 = builder_->makeIntConstant(int((1 + i) % 3));
          spv::Id edge_1 = builder_->makeIntConstant(int((2 + i) % 3));
          spv::Id edge_x =
              builder_->createBinOp(spv::OpFSub, type_float, load_rectangle_position_xy(edge_1, 0),
                                    load_rectangle_position_xy(edge_0, 0));
          spv::Id edge_y =
              builder_->createBinOp(spv::OpFSub, type_float, load_rectangle_position_xy(edge_1, 1),
                                    load_rectangle_position_xy(edge_0, 1));
          edge_lengths[i] =
              builder_->createBinOp(spv::OpFAdd, type_float,
                                    builder_->createBinOp(spv::OpFMul, type_float, edge_x, edge_x),
                                    builder_->createBinOp(spv::OpFMul, type_float, edge_y, edge_y));
        }

        spv::Id rectangle_vertex_indices[3];
        rectangle_vertex_indices[0] = builder_->createTriOp(
            spv::OpSelect, type_int,
            builder_->createBinOp(spv::OpLogicalAnd, type_bool,
                                  builder_->createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                        edge_lengths[0], edge_lengths[1]),
                                  builder_->createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                        edge_lengths[0], edge_lengths[2])),
            const_int_0,
            builder_->createTriOp(spv::OpSelect, type_int,
                                  builder_->createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                        edge_lengths[1], edge_lengths[2]),
                                  const_int_1, const_int_2));
        for (u32 i = 1; i < 3; ++i) {
          spv::Id vertex_index_without_wrapping =
              builder_->createBinOp(spv::OpIAdd, type_int, rectangle_vertex_indices[0],
                                    builder_->makeIntConstant(int(i)));
          rectangle_vertex_indices[i] = builder_->createTriOp(
              spv::OpSelect, type_int,
              builder_->createBinOp(spv::OpSLessThan, type_bool, vertex_index_without_wrapping,
                                    const_int_3),
              vertex_index_without_wrapping,
              builder_->createBinOp(spv::OpISub, type_int, vertex_index_without_wrapping,
                                    const_int_3));
        }

        auto select_rectangle_vertex_component =
            [this, rectangle_host_vertex_is_0, rectangle_host_vertex_is_1](
                spv::Id value_0, spv::Id value_1, spv::Id value_2, spv::Id type) {
              return builder_->createTriOp(
                  spv::OpSelect, type, rectangle_host_vertex_is_0, value_0,
                  builder_->createTriOp(spv::OpSelect, type, rectangle_host_vertex_is_1, value_1,
                                        value_2));
            };

        interpolators_remaining = GetModificationInterpolatorMask();
        while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
          interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
          id_vector_temp_.clear();
          id_vector_temp_.push_back(rectangle_vertex_indices[0]);
          spv::Id interpolator_0 = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassFunction,
                                          var_main_rectangle_interpolators_[interpolator_index],
                                          id_vector_temp_),
              spv::NoPrecision);
          id_vector_temp_[0] = rectangle_vertex_indices[1];
          spv::Id interpolator_1 = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassFunction,
                                          var_main_rectangle_interpolators_[interpolator_index],
                                          id_vector_temp_),
              spv::NoPrecision);
          id_vector_temp_[0] = rectangle_vertex_indices[2];
          spv::Id interpolator_2 = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassFunction,
                                          var_main_rectangle_interpolators_[interpolator_index],
                                          id_vector_temp_),
              spv::NoPrecision);
          spv::Id interpolator_original = select_rectangle_vertex_component(
              interpolator_0, interpolator_1, interpolator_2, type_float4);
          spv::Id interpolator_synthetic = builder_->CreateNoContractionBinOp(
              spv::OpFAdd, type_float4,
              builder_->CreateNoContractionBinOp(spv::OpFSub, type_float4, interpolator_1,
                                                 interpolator_0),
              interpolator_2);
          builder_->createStore(
              builder_->createTriOp(spv::OpSelect, type_float4, rectangle_host_vertex_is_synthetic,
                                    interpolator_synthetic, interpolator_original),
              input_output_interpolators_[interpolator_index]);
        }

        auto load_rectangle_vertex_value = [this](spv::Id variable, spv::Id vertex_index) {
          id_vector_temp_.clear();
          id_vector_temp_.push_back(vertex_index);
          return builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassFunction, variable, id_vector_temp_),
              spv::NoPrecision);
        };

        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(kOutputPerVertexMemberPosition));
        spv::Id rectangle_position_ptr = builder_->createAccessChain(
            spv::StorageClassOutput, output_per_vertex_, id_vector_temp_);
        spv::Id position_0 = load_rectangle_vertex_value(var_main_rectangle_per_vertex_,
                                                         rectangle_vertex_indices[0]);
        spv::Id position_1 = load_rectangle_vertex_value(var_main_rectangle_per_vertex_,
                                                         rectangle_vertex_indices[1]);
        spv::Id position_2 = load_rectangle_vertex_value(var_main_rectangle_per_vertex_,
                                                         rectangle_vertex_indices[2]);
        spv::Id position_original =
            select_rectangle_vertex_component(position_0, position_1, position_2, type_float4);
        spv::Id position_synthetic = builder_->CreateNoContractionBinOp(
            spv::OpFAdd, type_float4,
            builder_->CreateNoContractionBinOp(spv::OpFSub, type_float4, position_1, position_0),
            position_2);
        builder_->createStore(
            builder_->createTriOp(spv::OpSelect, type_float4, rectangle_host_vertex_is_synthetic,
                                  position_synthetic, position_original),
            rectangle_position_ptr);

        auto write_rectangle_distance_member =
            [this, &select_rectangle_vertex_component, rectangle_host_vertex_is_synthetic,
             &load_rectangle_vertex_value, rectangle_vertex_indices](
                u32 member_index, u32 component_count,
                const std::array<spv::Id, kMaxRectanglePerVertexDistances>& variables) {
              for (u32 i = 0; i < component_count; ++i) {
                spv::Id value_0 =
                    load_rectangle_vertex_value(variables[i], rectangle_vertex_indices[0]);
                spv::Id value_1 =
                    load_rectangle_vertex_value(variables[i], rectangle_vertex_indices[1]);
                spv::Id value_2 =
                    load_rectangle_vertex_value(variables[i], rectangle_vertex_indices[2]);
                spv::Id value_original =
                    select_rectangle_vertex_component(value_0, value_1, value_2, type_float);
                spv::Id value_synthetic = builder_->CreateNoContractionBinOp(
                    spv::OpFAdd, type_float,
                    builder_->CreateNoContractionBinOp(spv::OpFSub, type_float, value_1, value_0),
                    value_2);
                id_vector_temp_.clear();
                id_vector_temp_.push_back(builder_->makeIntConstant(int(member_index)));
                id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
                builder_->createStore(
                    builder_->createTriOp(spv::OpSelect, type_float,
                                          rectangle_host_vertex_is_synthetic, value_synthetic,
                                          value_original),
                    builder_->createAccessChain(spv::StorageClassOutput, output_per_vertex_,
                                                id_vector_temp_));
              }
            };

        if (clip_distance_count && output_per_vertex_member_clip_distance_ != UINT32_MAX) {
          write_rectangle_distance_member(output_per_vertex_member_clip_distance_,
                                          clip_distance_count, var_main_rectangle_clip_distances_);
        }
        if (cull_distance_count && output_per_vertex_member_cull_distance_ != UINT32_MAX) {
          write_rectangle_distance_member(output_per_vertex_member_cull_distance_,
                                          cull_distance_count, var_main_rectangle_cull_distances_);
        }
      }
      if_not_last_guest_vertex.MakeEndIf();
    }
    if (main_loop_exit_ != main_loop_merge_) {
      if (!builder_->getBuildPoint()->isTerminated()) {
        builder_->createBranch(main_loop_exit_);
      }
      function_main_->addBlock(main_loop_exit_);
      builder_->setBuildPoint(main_loop_exit_);
    }
  }

  ExportToMemory(GetCurrentShader().GetMemExportEMPotentiallyWrittenBeforeEnd());

  if (IsVertexShader() && !IsSPIRVComputeShader() && !rectangle_vertex_loop) {
    CompleteVertexOrTessEvalShaderInMain();
  } else if (IsPixelShader()) {
    CompleteFragmentShaderInMain();
  }

  builder_->leaveFunction();

  spv::ExecutionModel execution_model;
  if (IsPixelShader()) {
    execution_model = spv::ExecutionModelFragment;
    builder_->addExecutionMode(function_main_, spv::ExecutionModeOriginUpperLeft);
    if (output_fragment_depth_ != spv::NoResult) {
      builder_->addExecutionMode(function_main_, spv::ExecutionModeDepthReplacing);
      if (!GetCurrentShader().GetWritesDepth() &&
          GetSPIRVShaderModification().pixel.depth_stencil_mode ==
              Modification::DepthStencilMode::kFloat24Truncating) {
        builder_->addExecutionMode(function_main_, spv::ExecutionModeDepthLess);
      }
    }
    if (IsExecutionModeEarlyFragmentTests()) {
      builder_->addExecutionMode(function_main_, spv::ExecutionModeEarlyFragmentTests);
    }
    if (edram_fragment_shader_interlock_) {
      if (features_.fragment_shader_sample_interlock) {
        builder_->addCapability(spv::CapabilityFragmentShaderSampleInterlockEXT);
        builder_->addExecutionMode(function_main_, spv::ExecutionModeSampleInterlockOrderedEXT);
      } else {
        builder_->addCapability(spv::CapabilityFragmentShaderPixelInterlockEXT);
        builder_->addExecutionMode(function_main_, spv::ExecutionModePixelInterlockOrderedEXT);
      }
    }
  } else {
    REX_ASSERT(IsVertexShader());
    bool is_compute_shader = IsSPIRVComputeShader();
    bool is_tess_eval_shader = IsSPIRVTessEvalShader();
    execution_model = is_compute_shader
                          ? spv::ExecutionModelGLCompute
                          : (is_tess_eval_shader ? spv::ExecutionModelTessellationEvaluation
                                                 : spv::ExecutionModelVertex);
    if (is_compute_shader) {
      builder_->addExecutionMode(function_main_, spv::ExecutionModeLocalSize, 1, 1, 1);
    } else if (is_tess_eval_shader) {
      Modification shader_modification = GetSPIRVShaderModification();
      switch (shader_modification.vertex.host_vertex_shader_type) {
        case Shader::HostVertexShaderType::kLineDomainCPIndexed:
        case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
          builder_->addExecutionMode(function_main_, spv::ExecutionModeIsolines);
          break;
        case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
        case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          builder_->addExecutionMode(function_main_, spv::ExecutionModeTriangles);
          break;
        case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
        case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
          builder_->addExecutionMode(function_main_, spv::ExecutionModeQuads);
          break;
        default:
          REX_UNHANDLED_CASE(shader_modification.vertex.host_vertex_shader_type);
          break;
      }
      builder_->addExecutionMode(function_main_,
                                 (shader_modification.vertex.tessellation_mode ==
                                  std::to_underlying(xenos::TessellationMode::kDiscrete))
                                     ? spv::ExecutionModeSpacingEqual
                                     : spv::ExecutionModeSpacingFractionalEven);
      builder_->addExecutionMode(function_main_, spv::ExecutionModeVertexOrderCw);
    }
  }
  if (features_.denorm_flush_to_zero_float32) {
    builder_->addCapability(spv::CapabilityDenormFlushToZero);
    builder_->addExecutionMode(function_main_, spv::ExecutionModeDenormFlushToZero, 32);
  }
  if (features_.signed_zero_inf_nan_preserve_float32) {
    builder_->addCapability(spv::CapabilitySignedZeroInfNanPreserve);
    builder_->addExecutionMode(function_main_, spv::ExecutionModeSignedZeroInfNanPreserve, 32);
  }
  if (features_.rounding_mode_rte_float32) {
    builder_->addCapability(spv::CapabilityRoundingModeRTE);
    builder_->addExecutionMode(function_main_, spv::ExecutionModeRoundingModeRTE, 32);
  }
  spv::Instruction* entry_point = builder_->addEntryPoint(execution_model, function_main_, "main");
  for (spv::Id interface_id : main_interface_) {
    entry_point->addIdOperand(interface_id);
  }

  if (!is_depth_only_fragment_shader_) {
    size_t texture_binding_count = texture_bindings_.size();
    size_t sampler_binding_count = sampler_bindings_.size();
    for (size_t i = 0; i < sampler_binding_count; ++i) {
      builder_->addDecoration(sampler_bindings_[i].variable, spv::DecorationBinding,
                              int(texture_binding_count + i));
    }
  }

  std::vector<unsigned int> module_uints;
  builder_->dump(module_uints);
  std::vector<u8> module_bytes;
  module_bytes.reserve(sizeof(unsigned int) * module_uints.size());
  module_bytes.insert(module_bytes.cend(), reinterpret_cast<const u8*>(module_uints.data()),
                      reinterpret_cast<const u8*>(module_uints.data()) +
                          sizeof(unsigned int) * module_uints.size());
  return module_bytes;
}

void SPIRVShaderTranslator::PostTranslation() {
  Shader::Translation& translation = GetCurrentTranslation();
  if (!translation.IsValid()) {
    return;
  }
  SPIRVShader* spirv_shader = dynamic_cast<SPIRVShader*>(&translation.GetShader());
  if (spirv_shader &&
      !spirv_shader->bindings_setup_entered_.test_and_set(std::memory_order_relaxed)) {
    spirv_shader->translated_texture_bindings_.clear();
    spirv_shader->translated_texture_bindings_.reserve(texture_bindings_.size());
    for (const TextureBinding& translator_binding : texture_bindings_) {
      SPIRVShader::TextureBinding& shader_binding =
          spirv_shader->translated_texture_bindings_.emplace_back();
      std::memset(&shader_binding, 0, sizeof(shader_binding));
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.dimension = translator_binding.dimension;
      shader_binding.is_signed = translator_binding.is_signed;
      spirv_shader->used_texture_mask_ |= UINT32_C(1) << translator_binding.fetch_constant;
    }
    spirv_shader->sampler_bindings_.clear();
    spirv_shader->sampler_bindings_.reserve(sampler_bindings_.size());
    for (const SamplerBinding& translator_binding : sampler_bindings_) {
      SPIRVShader::SamplerBinding& shader_binding = spirv_shader->sampler_bindings_.emplace_back();
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.mag_filter = translator_binding.mag_filter;
      shader_binding.min_filter = translator_binding.min_filter;
      shader_binding.mip_filter = translator_binding.mip_filter;
      shader_binding.aniso_filter = translator_binding.aniso_filter;
    }
  }
}

void SPIRVShaderTranslator::ProcessLabel(u32 cf_index) {
  if (cf_index == 0) {
    return;
  }

  REX_ASSERT(!GetCurrentShader().GetLabelAddresses().empty());

  CloseExecConditionals();

  spv::Function& function = builder_->getBuildPoint()->getParent();
  spv::Block* new_case = new spv::Block(builder_->getUniqueId(), function);
  main_switch_op_->addImmediateOperand(cf_index);
  main_switch_op_->addIdOperand(new_case->getId());
  // Every switch case must have the OpSelectionMerge and OpSwitch block as a predecessor.
  new_case->addPredecessor(main_switch_header_);
  if (!builder_->getBuildPoint()->isTerminated()) {
    builder_->createBranch(new_case);
  }
  function.addBlock(new_case);
  builder_->setBuildPoint(new_case);
}

void SPIRVShaderTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr) {
  UpdateExecConditionals(instr.type, instr.bool_constant_index, instr.condition);
}

void SPIRVShaderTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr) {
  if (instr.is_end) {
    CloseInstructionPredication();
    if (!builder_->getBuildPoint()->isTerminated()) {
      builder_->createBranch(GetCurrentShader().GetLabelAddresses().empty() ? main_loop_merge_
                                                                            : main_switch_merge_);
    }
  }
  UpdateExecConditionals(instr.type, instr.bool_constant_index, instr.condition);
}

void SPIRVShaderTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) {
  CloseExecConditionals();

  EnsureBuildPointAvailable();

  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeIntConstant(1));
  id_vector_temp_.push_back(builder_->makeIntConstant(int(instr.loop_constant_index >> 2)));
  id_vector_temp_.push_back(builder_->makeIntConstant(int(instr.loop_constant_index & 3)));
  spv::Id loop_constant = builder_->createLoad(
      builder_->createAccessChain(spv::StorageClassUniform, uniform_bool_loop_constants_,
                                  id_vector_temp_),
      spv::NoPrecision);

  spv::Id const_int_8 = builder_->makeIntConstant(8);

  spv::Id loop_count_stack_old = builder_->createLoad(var_main_loop_count_, spv::NoPrecision);
  spv::Id loop_count_new = builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, loop_constant,
                                                 const_int_0_, const_int_8);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(loop_count_new);
  for (unsigned int i = 0; i < 3; ++i) {
    id_vector_temp_.push_back(builder_->createCompositeExtract(loop_count_stack_old, type_uint, i));
  }
  builder_->createStore(builder_->createCompositeConstruct(type_uint4, id_vector_temp_),
                        var_main_loop_count_);

  spv::Id address_relative_stack_old =
      builder_->createLoad(var_main_loop_address_, spv::NoPrecision);
  id_vector_temp_.clear();
  if (instr.is_repeat) {
    id_vector_temp_.emplace_back();
  } else {
    id_vector_temp_.push_back(
        builder_->createUnaryOp(spv::OpBitcast, type_int,
                                builder_->createTriOp(spv::OpBitFieldUExtract, type_uint,
                                                      loop_constant, const_int_8, const_int_8)));
  }
  for (unsigned int i = 0; i < 3; ++i) {
    id_vector_temp_.push_back(
        builder_->createCompositeExtract(address_relative_stack_old, type_int, i));
  }
  if (instr.is_repeat) {
    id_vector_temp_[0] = id_vector_temp_[1];
  }
  builder_->createStore(builder_->createCompositeConstruct(type_int4, id_vector_temp_),
                        var_main_loop_address_);

  spv::Block& head_block = *builder_->getBuildPoint();
  spv::Id loop_count_zero =
      builder_->createBinOp(spv::OpIEqual, type_bool, loop_count_new, const_uint_0_);
  spv::Block& skip_block = builder_->makeNewBlock();
  spv::Block& body_block = builder_->makeNewBlock();
  builder_->createSelectionMerge(&body_block, spv::SelectionControlMaskNone);
  {
    std::unique_ptr<spv::Instruction> branch_conditional_op =
        std::make_unique<spv::Instruction>(spv::OpBranchConditional);
    branch_conditional_op->addIdOperand(loop_count_zero);
    branch_conditional_op->addIdOperand(skip_block.getId());
    branch_conditional_op->addIdOperand(body_block.getId());
    branch_conditional_op->addImmediateOperand(1);
    branch_conditional_op->addImmediateOperand(2);
    head_block.addInstruction(std::move(branch_conditional_op));
  }
  skip_block.addPredecessor(&head_block);
  body_block.addPredecessor(&head_block);
  builder_->setBuildPoint(&skip_block);
  main_switch_next_pc_phi_operands_.push_back(
      builder_->makeIntConstant(int(instr.loop_skip_address)));
  main_switch_next_pc_phi_operands_.push_back(builder_->getBuildPoint()->getId());
  builder_->createBranch(main_loop_continue_);
  builder_->setBuildPoint(&body_block);
}

void SPIRVShaderTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) {
  CloseExecConditionals();

  EnsureBuildPointAvailable();

  spv::Id loop_count_stack_old = builder_->createLoad(var_main_loop_count_, spv::NoPrecision);
  spv::Id loop_count = builder_->createBinOp(
      spv::OpISub, type_uint, builder_->createCompositeExtract(loop_count_stack_old, type_uint, 0),
      builder_->makeUintConstant(1));
  spv::Id address_relative_stack_old =
      builder_->createLoad(var_main_loop_address_, spv::NoPrecision);

  bool break_is_true = instr.is_predicated_break && instr.predicate_condition;
  spv::Id condition = builder_->createBinOp(break_is_true ? spv::OpIEqual : spv::OpINotEqual,
                                            type_bool, loop_count, const_uint_0_);
  if (instr.is_predicated_break) {
    condition = builder_->createBinOp(
        instr.predicate_condition ? spv::OpLogicalOr : spv::OpLogicalAnd, type_bool, condition,
        builder_->createLoad(var_main_predicate_, spv::NoPrecision));
  }

  spv::Block& body_block = *builder_->getBuildPoint();
  spv::Block& continue_block = builder_->makeNewBlock();
  spv::Block& break_block = builder_->makeNewBlock();
  builder_->createSelectionMerge(&break_block, spv::SelectionControlMaskNone);
  {
    std::unique_ptr<spv::Instruction> branch_conditional_op =
        std::make_unique<spv::Instruction>(spv::OpBranchConditional);
    branch_conditional_op->addIdOperand(condition);
    if (break_is_true) {
      branch_conditional_op->addIdOperand(break_block.getId());
      branch_conditional_op->addIdOperand(continue_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
    } else {
      branch_conditional_op->addIdOperand(continue_block.getId());
      branch_conditional_op->addIdOperand(break_block.getId());
      branch_conditional_op->addImmediateOperand(2);
      branch_conditional_op->addImmediateOperand(1);
    }
    body_block.addInstruction(std::move(branch_conditional_op));
  }
  continue_block.addPredecessor(&body_block);
  break_block.addPredecessor(&body_block);

  builder_->setBuildPoint(&continue_block);
  builder_->createStore(
      builder_->createCompositeInsert(loop_count, loop_count_stack_old, type_uint4, 0),
      var_main_loop_count_);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeIntConstant(1));
  id_vector_temp_.push_back(builder_->makeIntConstant(int(instr.loop_constant_index >> 2)));
  id_vector_temp_.push_back(builder_->makeIntConstant(int(instr.loop_constant_index & 3)));
  spv::Id loop_constant = builder_->createLoad(
      builder_->createAccessChain(spv::StorageClassUniform, uniform_bool_loop_constants_,
                                  id_vector_temp_),
      spv::NoPrecision);
  spv::Id address_relative_old =
      builder_->createCompositeExtract(address_relative_stack_old, type_int, 0);
  builder_->createStore(
      builder_->createCompositeInsert(
          builder_->createBinOp(
              spv::OpIAdd, type_int, address_relative_old,
              builder_->createTriOp(
                  spv::OpBitFieldSExtract, type_int,
                  builder_->createUnaryOp(spv::OpBitcast, type_int, loop_constant),
                  builder_->makeIntConstant(16), builder_->makeIntConstant(8))),
          address_relative_stack_old, type_int4, 0),
      var_main_loop_address_);
  main_switch_next_pc_phi_operands_.push_back(
      builder_->makeIntConstant(int(instr.loop_body_address)));
  main_switch_next_pc_phi_operands_.push_back(builder_->getBuildPoint()->getId());
  builder_->createBranch(main_loop_continue_);

  builder_->setBuildPoint(&break_block);
  id_vector_temp_.clear();
  for (unsigned int i = 1; i < 4; ++i) {
    id_vector_temp_.push_back(builder_->createCompositeExtract(loop_count_stack_old, type_uint, i));
  }
  id_vector_temp_.push_back(const_uint_0_);
  builder_->createStore(builder_->createCompositeConstruct(type_uint4, id_vector_temp_),
                        var_main_loop_count_);
  id_vector_temp_.clear();
  for (unsigned int i = 1; i < 4; ++i) {
    id_vector_temp_.push_back(
        builder_->createCompositeExtract(address_relative_stack_old, type_int, i));
  }
  id_vector_temp_.push_back(const_int_0_);
  builder_->createStore(builder_->createCompositeConstruct(type_int4, id_vector_temp_),
                        var_main_loop_address_);
}

void SPIRVShaderTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr) {
  ParsedExecInstruction::Type type;
  if (instr.type == ParsedJumpInstruction::Type::kConditional) {
    type = ParsedExecInstruction::Type::kConditional;
  } else if (instr.type == ParsedJumpInstruction::Type::kPredicated) {
    type = ParsedExecInstruction::Type::kPredicated;
  } else {
    type = ParsedExecInstruction::Type::kUnconditional;
  }
  UpdateExecConditionals(type, instr.bool_constant_index, instr.condition);

  CloseInstructionPredication();

  if (builder_->getBuildPoint()->isTerminated()) {
    return;
  }
  main_switch_next_pc_phi_operands_.push_back(builder_->makeIntConstant(int(instr.target_address)));
  main_switch_next_pc_phi_operands_.push_back(builder_->getBuildPoint()->getId());
  builder_->createBranch(main_loop_continue_);
}

void SPIRVShaderTranslator::ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                                                    u8 export_em) {
  bool start_memexport =
      instr.type == xenos::ucode::AllocType::kMemory && GetCurrentShader().GetMemExportEMWritten();
  if (export_em || start_memexport) {
    CloseExecConditionals();
  }

  if (export_em) {
    ExportToMemory(export_em);
    builder_->createStore(const_uint_0_, var_main_memexport_data_written_);
    u8 export_em_remaining = export_em;
    u32 em_index;
    while (rex::bit_scan_forward(export_em_remaining, &em_index)) {
      export_em_remaining &= ~(static_cast<u8>(1) << em_index);
      builder_->createStore(const_float4_0, var_main_memexport_data_[em_index]);
    }
  }

  if (start_memexport) {
    builder_->createStore(const_float4_0, var_main_memexport_address_);
  }
}

spv::Id SPIRVShaderTranslator::SPIRVSmearScalarResultOrConstant(spv::Id scalar,
                                                                spv::Id vector_type) {
  bool is_constant = builder_->isConstant(scalar);
  bool is_spec_constant = builder_->isSpecConstant(scalar);
  if (!is_constant && !is_spec_constant) {
    return builder_->smearScalar(spv::NoPrecision, scalar, vector_type);
  }
  REX_ASSERT(builder_->getTypeClass(builder_->getTypeId(scalar)) ==
             builder_->getTypeClass(builder_->getScalarTypeId(vector_type)));
  if (!builder_->isVectorType(vector_type)) {
    REX_ASSERT(builder_->isScalarType(vector_type));
    return scalar;
  }
  int num_components = builder_->getNumTypeComponents(vector_type);
  id_vector_temp_util_.clear();
  for (int i = 0; i < num_components; ++i) {
    id_vector_temp_util_.push_back(scalar);
  }
  return builder_->makeCompositeConstant(vector_type, id_vector_temp_util_, is_spec_constant);
}

u32 SPIRVShaderTranslator::GetPsParamGenInterpolator() const {
  REX_ASSERT(IsPixelShader());
  Modification modification = GetSPIRVShaderModification();
  return (modification.pixel.param_gen_enable &&
          modification.pixel.param_gen_interpolator < GetRegisterCount())
             ? modification.pixel.param_gen_interpolator
             : UINT32_MAX;
}

void SPIRVShaderTranslator::EnsureBuildPointAvailable() {
  if (!builder_->getBuildPoint()->isTerminated()) {
    return;
  }
  spv::Block& new_block = builder_->makeNewBlock();
  new_block.setUnreachable();
  builder_->setBuildPoint(&new_block);
}

void SPIRVShaderTranslator::StartVertexOrTessEvalShaderBeforeMain() {
  if (IsSPIRVComputeShader()) {
    input_vertex_index_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                   type_uint3, "gl_GlobalInvocationID");
    builder_->addDecoration(input_vertex_index_, spv::DecorationBuiltIn,
                            spv::BuiltInGlobalInvocationId);
    main_interface_.push_back(input_vertex_index_);
    return;
  }

  if (IsSPIRVTessEvalShader()) {
    input_primitive_id_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                   type_int, "gl_PrimitiveID");
    builder_->addDecoration(input_primitive_id_, spv::DecorationBuiltIn, spv::BuiltInPrimitiveId);
    main_interface_.push_back(input_primitive_id_);
    input_tess_coord_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                 type_float3, "gl_TessCoord");
    builder_->addDecoration(input_tess_coord_, spv::DecorationBuiltIn, spv::BuiltInTessCoord);
    main_interface_.push_back(input_tess_coord_);
    input_patch_control_point_indices_ = builder_->createVariable(
        spv::NoPrecision, spv::StorageClassInput, type_float4, "xe_in_patch_control_point_indices");
    builder_->addDecoration(input_patch_control_point_indices_, spv::DecorationLocation, 0);
    builder_->addDecoration(input_patch_control_point_indices_, spv::DecorationPatch);
    main_interface_.push_back(input_patch_control_point_indices_);
  } else {
    input_vertex_index_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                   type_int, "gl_VertexIndex");
    builder_->addDecoration(input_vertex_index_, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);
    main_interface_.push_back(input_vertex_index_);
  }

  u32 output_location = 0;

  {
    u32 interpolators_remaining = GetModificationInterpolatorMask();
    u32 interpolator_index;
    while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
      interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
      spv::Id interpolator = builder_->createVariable(
          spv::NoPrecision, spv::StorageClassOutput, type_float4,
          std::format("xe_out_interpolator_{}", interpolator_index).c_str());
      input_output_interpolators_[interpolator_index] = interpolator;
      builder_->addDecoration(interpolator, spv::DecorationLocation, int(output_location));
      builder_->addDecoration(interpolator, spv::DecorationInvariant);
      main_interface_.push_back(interpolator);
      ++output_location;
    }
  }

  Modification shader_modification = GetSPIRVShaderModification();

  if (shader_modification.vertex.output_point_parameters) {
    if (shader_modification.vertex.host_vertex_shader_type ==
        Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
      output_point_coordinates_ = builder_->createVariable(
          spv::NoPrecision, spv::StorageClassOutput, type_float2, "xe_out_point_coordinates");
      builder_->addDecoration(output_point_coordinates_, spv::DecorationLocation,
                              int(output_location));
      builder_->addDecoration(output_point_coordinates_, spv::DecorationInvariant);
      main_interface_.push_back(output_point_coordinates_);
      ++output_location;
    } else {
      output_point_size_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                    type_float, "xe_out_point_size");
      builder_->addDecoration(output_point_size_, spv::DecorationLocation, int(output_location));
      builder_->addDecoration(output_point_size_, spv::DecorationInvariant);
      main_interface_.push_back(output_point_size_);
      ++output_location;
    }
  }

  u32 clip_distance_count = shader_modification.GetVertexClipDistanceCount();
  u32 cull_distance_count = shader_modification.GetVertexCullDistanceCount();
  std::vector<spv::Id> struct_per_vertex_members;
  struct_per_vertex_members.reserve(kOutputPerVertexMemberCountMax);
  struct_per_vertex_members.push_back(type_float4);
  output_per_vertex_member_count_ = static_cast<u32>(struct_per_vertex_members.size());
  output_per_vertex_member_clip_distance_ = UINT32_MAX;
  output_per_vertex_member_cull_distance_ = UINT32_MAX;
  if (clip_distance_count) {
    REX_ASSERT(features_.clip_distance);
    builder_->addCapability(spv::CapabilityClipDistance);
    spv::Id type_clip_distance =
        builder_->makeArrayType(type_float, builder_->makeUintConstant(clip_distance_count), 0);
    builder_->addDecoration(type_clip_distance, spv::DecorationArrayStride, sizeof(f32));
    output_per_vertex_member_clip_distance_ = static_cast<u32>(struct_per_vertex_members.size());
    struct_per_vertex_members.push_back(type_clip_distance);
    output_per_vertex_member_count_ = static_cast<u32>(struct_per_vertex_members.size());
  }
  if (cull_distance_count) {
    REX_ASSERT(features_.cull_distance);
    builder_->addCapability(spv::CapabilityCullDistance);
    spv::Id type_cull_distance =
        builder_->makeArrayType(type_float, builder_->makeUintConstant(cull_distance_count), 0);
    builder_->addDecoration(type_cull_distance, spv::DecorationArrayStride, sizeof(f32));
    output_per_vertex_member_cull_distance_ = static_cast<u32>(struct_per_vertex_members.size());
    struct_per_vertex_members.push_back(type_cull_distance);
    output_per_vertex_member_count_ = static_cast<u32>(struct_per_vertex_members.size());
  }
  spv::Id type_struct_per_vertex =
      builder_->makeStructType(struct_per_vertex_members, "gl_PerVertex");
  builder_->addMemberName(type_struct_per_vertex, kOutputPerVertexMemberPosition, "gl_Position");
  builder_->addMemberDecoration(type_struct_per_vertex, kOutputPerVertexMemberPosition,
                                spv::DecorationBuiltIn, spv::BuiltInPosition);
  if (output_per_vertex_member_clip_distance_ != UINT32_MAX) {
    builder_->addMemberName(type_struct_per_vertex,
                            static_cast<int>(output_per_vertex_member_clip_distance_),
                            "gl_ClipDistance");
    builder_->addMemberDecoration(type_struct_per_vertex, output_per_vertex_member_clip_distance_,
                                  spv::DecorationBuiltIn, spv::BuiltInClipDistance);
  }
  if (output_per_vertex_member_cull_distance_ != UINT32_MAX) {
    builder_->addMemberName(type_struct_per_vertex,
                            static_cast<int>(output_per_vertex_member_cull_distance_),
                            "gl_CullDistance");
    builder_->addMemberDecoration(type_struct_per_vertex, output_per_vertex_member_cull_distance_,
                                  spv::DecorationBuiltIn, spv::BuiltInCullDistance);
  }
  builder_->addDecoration(type_struct_per_vertex, spv::DecorationBlock);
  type_output_per_vertex_ = type_struct_per_vertex;
  output_per_vertex_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                type_struct_per_vertex, "");
  builder_->addDecoration(output_per_vertex_, spv::DecorationInvariant);
  main_interface_.push_back(output_per_vertex_);
}

void SPIRVShaderTranslator::StartVertexOrTessEvalShaderInMain() {
  Modification shader_modification = GetSPIRVShaderModification();
  Shader::HostVertexShaderType host_vertex_shader_type =
      shader_modification.vertex.host_vertex_shader_type;

  if (GetCurrentShader().GetWritesPointSizeEdgeFlagKillVertex() & 0b101) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeFloatConstant(-1.0f));
    id_vector_temp_.push_back(const_float_0);
    id_vector_temp_.push_back(const_float_0);
    var_main_point_size_edge_flag_kill_vertex_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float3,
                                 "xe_var_point_size_edge_flag_kill_vertex",
                                 builder_->makeCompositeConstant(type_float3, id_vector_temp_));
  }

  for (u32 i = 0; i < GetRegisterCount(); ++i) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
    builder_->createStore(
        const_float4_0, builder_->createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                                    id_vector_temp_));
  }

  if (!IsSPIRVComputeShader()) {
    u32 interpolators_remaining = GetModificationInterpolatorMask();
    u32 interpolator_index;
    while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
      interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
      builder_->createStore(const_float4_0, input_output_interpolators_[interpolator_index]);
    }
  }

  if (IsMemoryExportUsed()) {
    spv::Id memexport_allowed_for_host_vertex_of_guest_primitive = spv::NoResult;
    if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
        host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      spv::Id host_vertex_in_guest_primitive = builder_->createBinOp(
          spv::OpBitwiseAnd, type_uint,
          builder_->createUnaryOp(spv::OpBitcast, type_uint,
                                  builder_->createLoad(input_vertex_index_, spv::NoPrecision)),
          builder_->makeUintConstant(3));
      if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
        memexport_allowed_for_host_vertex_of_guest_primitive = builder_->createBinOp(
            spv::OpIEqual, type_bool, host_vertex_in_guest_primitive, const_uint_0_);
      } else {
        if (IsSPIRVRectangleListVertexLoopEnabled()) {
          memexport_allowed_for_host_vertex_of_guest_primitive = builder_->createBinOp(
              spv::OpIEqual, type_bool, host_vertex_in_guest_primitive, const_uint_0_);
        } else {
          memexport_allowed_for_host_vertex_of_guest_primitive =
              builder_->createBinOp(spv::OpINotEqual, type_bool, host_vertex_in_guest_primitive,
                                    builder_->makeUintConstant(3));
        }
      }
    }

    if (memexport_allowed_for_host_vertex_of_guest_primitive != spv::NoResult) {
      main_memexport_allowed_ =
          main_memexport_allowed_ != spv::NoResult
              ? builder_->createBinOp(spv::OpLogicalAnd, type_bool, main_memexport_allowed_,
                                      memexport_allowed_for_host_vertex_of_guest_primitive)
              : memexport_allowed_for_host_vertex_of_guest_primitive;
    }
  }

  if (GetRegisterCount()) {
    auto store_register_component = [this](u32 register_index, u32 component, spv::Id value) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(int(register_index)));
      id_vector_temp_.push_back(builder_->makeIntConstant(int(component)));
      builder_->createStore(
          value, builder_->createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                             id_vector_temp_));
    };
    if (IsSPIRVTessEvalShader()) {
      auto load_tess_coord_component = [this](u32 component) {
        return builder_->createCompositeExtract(
            builder_->createLoad(input_tess_coord_, spv::NoPrecision), type_float, component);
      };
      auto load_patch_control_point_index = [this](u32 component) {
        return builder_->createCompositeExtract(
            builder_->createLoad(input_patch_control_point_indices_, spv::NoPrecision), type_float,
            component);
      };
      switch (shader_modification.vertex.host_vertex_shader_type) {
        case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
          store_register_component(0, 0, load_tess_coord_component(2));
          store_register_component(0, 1, load_tess_coord_component(1));
          store_register_component(0, 2, load_tess_coord_component(0));
          if (GetRegisterCount() >= 2) {
            store_register_component(1, 0, load_patch_control_point_index(0));
            store_register_component(1, 1, load_patch_control_point_index(1));
            store_register_component(1, 2, load_patch_control_point_index(2));
          }
          break;
        case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          store_register_component(0, 0, load_tess_coord_component(2));
          store_register_component(0, 1, load_tess_coord_component(1));
          store_register_component(0, 2, load_tess_coord_component(0));
          if (GetRegisterCount() >= 2) {
            store_register_component(1, 0, load_patch_control_point_index(0));
            store_register_component(1, 1, const_float_0);
          }
          break;
        case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
          store_register_component(0, 0, load_tess_coord_component(0));
          store_register_component(0, 1, load_tess_coord_component(1));
          store_register_component(0, 2, load_patch_control_point_index(0));
          if (GetRegisterCount() >= 2) {
            store_register_component(1, 0, load_patch_control_point_index(1));
            store_register_component(1, 1, load_patch_control_point_index(2));
            store_register_component(1, 2, load_patch_control_point_index(3));
          }
          break;
        case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
          store_register_component(0, 0, load_patch_control_point_index(0));
          store_register_component(0, 1, load_tess_coord_component(0));
          store_register_component(0, 2, load_tess_coord_component(1));
          if (GetRegisterCount() >= 2) {
            store_register_component(1, 0, const_float_0);
          }
          break;
        default:
          break;
      }
    }
    if (IsSPIRVVertexShader() || IsSPIRVComputeShader()) {
      spv::Id vertex_index;
      spv::Id compute_vertex_processing_index = spv::NoResult;
      spv::Id compute_load_vertex_index = spv::NoResult;
      spv::Id compute_vertex_index_is_32bit = spv::NoResult;
      if (IsSPIRVComputeShader()) {
        spv::Id global_invocation_id = builder_->createLoad(input_vertex_index_, spv::NoPrecision);
        spv::Id global_invocation_x =
            builder_->createCompositeExtract(global_invocation_id, type_uint, 0);
        spv::Id global_invocation_y =
            builder_->createCompositeExtract(global_invocation_id, type_uint, 1);
        spv::Id global_invocation_z =
            builder_->createCompositeExtract(global_invocation_id, type_uint, 2);
        compute_vertex_processing_index = builder_->createBinOp(
            spv::OpIAdd, type_uint, global_invocation_x,
            builder_->createBinOp(
                spv::OpIAdd, type_uint,
                builder_->createBinOp(spv::OpIMul, type_uint, global_invocation_y,
                                      builder_->makeUintConstant(kMemExportComputeDispatchX)),
                builder_->createBinOp(spv::OpIMul, type_uint, global_invocation_z,
                                      builder_->makeUintConstant(kMemExportComputeDispatchX *
                                                                 kMemExportComputeDispatchY))));

        id_vector_temp_.clear();
        id_vector_temp_.push_back(
            builder_->makeIntConstant(kSystemConstantComputeMemExportVertexCount));
        spv::Id compute_memexport_vertex_count = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);
        spv::Id compute_vertex_in_range =
            builder_->createBinOp(spv::OpULessThan, type_bool, compute_vertex_processing_index,
                                  compute_memexport_vertex_count);
        main_memexport_allowed_ =
            main_memexport_allowed_ != spv::NoResult
                ? builder_->createBinOp(spv::OpLogicalAnd, type_bool, main_memexport_allowed_,
                                        compute_vertex_in_range)
                : compute_vertex_in_range;
        vertex_index = builder_->createTriOp(spv::OpSelect, type_uint, compute_vertex_in_range,
                                             compute_vertex_processing_index, const_uint_0_);
        compute_vertex_processing_index = vertex_index;
      } else {
        vertex_index = builder_->createUnaryOp(
            spv::OpBitcast, type_uint, builder_->createLoad(input_vertex_index_, spv::NoPrecision));
      }
      if (IsSPIRVVertexShader()) {
        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantLineLoopClosingIndex));
        spv::Id line_loop_closing_index = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);
        vertex_index =
            builder_->createTriOp(spv::OpSelect, type_uint,
                                  builder_->createBinOp(spv::OpINotEqual, type_bool, vertex_index,
                                                        line_loop_closing_index),
                                  vertex_index, const_uint_0_);
      }
      if (IsSPIRVVertexShader() &&
          (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
           host_vertex_shader_type ==
               Shader::HostVertexShaderType::kRectangleListAsTriangleStrip)) {
        spv::Id const_uint_2 = builder_->makeUintConstant(2);
        spv::Id two_triangle_strip_vertex_index = vertex_index;
        vertex_index = builder_->createBinOp(spv::OpShiftRightLogical, type_uint,
                                             two_triangle_strip_vertex_index, const_uint_2);
        if (host_vertex_shader_type ==
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
          spv::Id const_uint_3 = builder_->makeUintConstant(3);
          spv::Id guest_vertex_in_guest_primitive;
          if (IsSPIRVRectangleListVertexLoopEnabled()) {
            guest_vertex_in_guest_primitive =
                builder_->createLoad(var_main_rectangle_guest_vertex_index_, spv::NoPrecision);
          } else {
            spv::Id host_vertex_in_guest_primitive = builder_->createBinOp(
                spv::OpBitwiseAnd, type_uint, two_triangle_strip_vertex_index, const_uint_3);
            guest_vertex_in_guest_primitive = builder_->createTriOp(
                spv::OpSelect, type_uint,
                builder_->createBinOp(spv::OpIEqual, type_bool, host_vertex_in_guest_primitive,
                                      const_uint_3),
                const_uint_2, host_vertex_in_guest_primitive);
          }
          vertex_index = builder_->createBinOp(
              spv::OpIAdd, type_uint,
              builder_->createBinOp(spv::OpIMul, type_uint, vertex_index, const_uint_3),
              guest_vertex_in_guest_primitive);
        }
        spv::Id load_vertex_index = builder_->createBinOp(
            spv::OpINotEqual, type_bool,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                  builder_->makeUintConstant(static_cast<unsigned int>(
                                      kSysFlagComputeOrPrimitiveVertexIndexLoad))),
            const_uint_0_);
        SPIRVBuilder::IfBuilder load_vertex_index_if(
            load_vertex_index, spv::SelectionControlDontFlattenMask, *builder_);
        spv::Id loaded_vertex_index;
        {
          spv::Id vertex_index_is_32bit = builder_->createBinOp(
              spv::OpINotEqual, type_bool,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                    builder_->makeUintConstant(static_cast<unsigned int>(
                                        kSysFlagComputeOrPrimitiveVertexIndexLoad32Bit))),
              const_uint_0_);
          id_vector_temp_.clear();
          id_vector_temp_.push_back(
              builder_->makeIntConstant(kSystemConstantVertexIndexLoadAddress));
          spv::Id vertex_index_address = builder_->createBinOp(
              spv::OpIAdd, type_uint,
              builder_->createLoad(
                  builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                              id_vector_temp_),
                  spv::NoPrecision),
              builder_->createBinOp(
                  spv::OpShiftLeftLogical, type_uint, vertex_index,
                  builder_->createTriOp(spv::OpSelect, type_uint, vertex_index_is_32bit,
                                        const_uint_2, builder_->makeUintConstant(1))));
          loaded_vertex_index = LoadUint32FromSharedMemory(
              builder_->createUnaryOp(spv::OpBitcast, type_int,
                                      builder_->createBinOp(spv::OpShiftRightLogical, type_uint,
                                                            vertex_index_address, const_uint_2)));
          loaded_vertex_index = builder_->createTriOp(
              spv::OpSelect, type_uint, vertex_index_is_32bit, loaded_vertex_index,
              builder_->createTriOp(
                  spv::OpBitFieldUExtract, type_uint, loaded_vertex_index,
                  builder_->createBinOp(spv::OpShiftLeftLogical, type_uint,
                                        builder_->createBinOp(spv::OpBitwiseAnd, type_uint,
                                                              vertex_index_address, const_uint_2),
                                        builder_->makeUintConstant(4 - 1)),
                  builder_->makeUintConstant(16)));
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexEndian));
          loaded_vertex_index = EndianSwap32Uint(
              loaded_vertex_index,
              builder_->createLoad(
                  builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                              id_vector_temp_),
                  spv::NoPrecision));
        }
        load_vertex_index_if.MakeEndIf();
        vertex_index = load_vertex_index_if.CreateMergePhi(loaded_vertex_index, vertex_index);
      } else {
        if (IsSPIRVComputeShader()) {
          spv::Id patch_index_from_invocation = builder_->createBinOp(
              spv::OpINotEqual, type_bool,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                    builder_->makeUintConstant(static_cast<unsigned int>(
                                        kSysFlagComputeMemExportPatchIndexFromInvocation))),
              const_uint_0_);
          compute_load_vertex_index = builder_->createBinOp(
              spv::OpINotEqual, type_bool,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                    builder_->makeUintConstant(static_cast<unsigned int>(
                                        kSysFlagComputeOrPrimitiveVertexIndexLoad))),
              const_uint_0_);
          compute_load_vertex_index = builder_->createBinOp(
              spv::OpLogicalAnd, type_bool, compute_load_vertex_index,
              builder_->createUnaryOp(spv::OpLogicalNot, type_bool, patch_index_from_invocation));
          compute_vertex_index_is_32bit = builder_->createBinOp(
              spv::OpINotEqual, type_bool,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                    builder_->makeUintConstant(static_cast<unsigned int>(
                                        kSysFlagComputeOrPrimitiveVertexIndexLoad32Bit))),
              const_uint_0_);
          SPIRVBuilder::IfBuilder load_vertex_index_if(
              compute_load_vertex_index, spv::SelectionControlDontFlattenMask, *builder_);
          spv::Id loaded_vertex_index;
          {
            id_vector_temp_.clear();
            id_vector_temp_.push_back(
                builder_->makeIntConstant(kSystemConstantVertexIndexLoadAddress));
            spv::Id vertex_index_address = builder_->createBinOp(
                spv::OpIAdd, type_uint,
                builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                                id_vector_temp_),
                    spv::NoPrecision),
                builder_->createBinOp(
                    spv::OpShiftLeftLogical, type_uint, vertex_index,
                    builder_->createTriOp(spv::OpSelect, type_uint, compute_vertex_index_is_32bit,
                                          builder_->makeUintConstant(2),
                                          builder_->makeUintConstant(1))));
            loaded_vertex_index = LoadUint32FromSharedMemory(builder_->createUnaryOp(
                spv::OpBitcast, type_int,
                builder_->createBinOp(spv::OpShiftRightLogical, type_uint, vertex_index_address,
                                      builder_->makeUintConstant(2))));
            loaded_vertex_index = builder_->createTriOp(
                spv::OpSelect, type_uint, compute_vertex_index_is_32bit, loaded_vertex_index,
                builder_->createTriOp(
                    spv::OpBitFieldUExtract, type_uint, loaded_vertex_index,
                    builder_->createBinOp(
                        spv::OpShiftLeftLogical, type_uint,
                        builder_->createBinOp(spv::OpBitwiseAnd, type_uint, vertex_index_address,
                                              builder_->makeUintConstant(2)),
                        builder_->makeUintConstant(3)),
                    builder_->makeUintConstant(16)));
            id_vector_temp_.clear();
            id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexEndian));
            loaded_vertex_index = EndianSwap32Uint(
                loaded_vertex_index,
                builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                                id_vector_temp_),
                    spv::NoPrecision));
          }
          load_vertex_index_if.MakeEndIf();
          vertex_index = load_vertex_index_if.CreateMergePhi(loaded_vertex_index, vertex_index);
        } else {
          if (!features_.full_draw_index_uint32) {
            spv::Id load_vertex_index = builder_->createBinOp(
                spv::OpINotEqual, type_bool,
                builder_->createBinOp(
                    spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                    builder_->makeUintConstant(static_cast<unsigned int>(kSysFlagVertexIndexLoad))),
                const_uint_0_);
            SPIRVBuilder::IfBuilder load_vertex_index_if(
                load_vertex_index, spv::SelectionControlDontFlattenMask, *builder_);
            spv::Id loaded_vertex_index;
            {
              id_vector_temp_.clear();
              id_vector_temp_.push_back(
                  builder_->makeIntConstant(kSystemConstantVertexIndexLoadAddress));
              loaded_vertex_index = LoadUint32FromSharedMemory(builder_->createUnaryOp(
                  spv::OpBitcast, type_int,
                  builder_->createBinOp(
                      spv::OpIAdd, type_uint,
                      builder_->createBinOp(
                          spv::OpShiftRightLogical, type_uint,
                          builder_->createLoad(builder_->createAccessChain(
                                                   spv::StorageClassUniform,
                                                   uniform_system_constants_, id_vector_temp_),
                                               spv::NoPrecision),
                          builder_->makeUintConstant(2)),
                      vertex_index)));
            }
            load_vertex_index_if.MakeEndIf();
            vertex_index = load_vertex_index_if.CreateMergePhi(loaded_vertex_index, vertex_index);
          }
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexEndian));
          vertex_index = EndianSwap32Uint(
              vertex_index, builder_->createLoad(builder_->createAccessChain(
                                                     spv::StorageClassUniform,
                                                     uniform_system_constants_, id_vector_temp_),
                                                 spv::NoPrecision));
        }
      }
      if (IsSPIRVComputeShader() && IsMemoryExportUsed()) {
        spv::Id primitive_reset_enabled = builder_->createBinOp(
            spv::OpINotEqual, type_bool,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                  builder_->makeUintConstant(static_cast<unsigned int>(
                                      kSysFlagComputeOrPrimitiveVertexIndexReset))),
            const_uint_0_);
        spv::Id vertex_index_is_32bit = builder_->createBinOp(
            spv::OpINotEqual, type_bool,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                  builder_->makeUintConstant(static_cast<unsigned int>(
                                      kSysFlagComputeOrPrimitiveVertexIndexLoad32Bit))),
            const_uint_0_);
        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexReset));
        spv::Id vertex_index_reset = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);
        spv::Id vertex_index_not_reset_16 =
            builder_->createBinOp(spv::OpINotEqual, type_bool, vertex_index, vertex_index_reset);
        spv::Id vertex_index_not_reset_32 = builder_->createBinOp(
            spv::OpINotEqual, type_bool,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint, vertex_index,
                                  builder_->makeUintConstant(xenos::kVertexIndexMask)),
            vertex_index_reset);
        spv::Id vertex_index_not_reset =
            builder_->createTriOp(spv::OpSelect, type_bool, vertex_index_is_32bit,
                                  vertex_index_not_reset_32, vertex_index_not_reset_16);
        spv::Id memexport_allowed_for_non_reset_index = builder_->createBinOp(
            spv::OpLogicalOr, type_bool,
            builder_->createUnaryOp(spv::OpLogicalNot, type_bool, primitive_reset_enabled),
            vertex_index_not_reset);
        main_memexport_allowed_ =
            main_memexport_allowed_ != spv::NoResult
                ? builder_->createBinOp(spv::OpLogicalAnd, type_bool, main_memexport_allowed_,
                                        memexport_allowed_for_non_reset_index)
                : memexport_allowed_for_non_reset_index;
      }
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexBaseIndex));
      spv::Id vertex_base_index = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexMin));
      spv::Id vertex_index_min = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexMax));
      spv::Id vertex_index_max = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      auto remap_vertex_index_to_float = [this, vertex_base_index, vertex_index_min,
                                          vertex_index_max](spv::Id index) {
        index = builder_->createBinOp(
            spv::OpIAdd, type_uint, index,
            builder_->createUnaryOp(spv::OpBitcast, type_uint, vertex_base_index));
        index = builder_->createBinOp(spv::OpBitwiseAnd, type_uint, index,
                                      builder_->makeUintConstant(xenos::kVertexIndexMask));
        index = builder_->CreateTriBuiltinCall(type_uint, ext_inst_glsl_std_450_, GLSLstd450UClamp,
                                               index, vertex_index_min, vertex_index_max);
        return builder_->createUnaryOp(spv::OpConvertUToF, type_float, index);
      };
      spv::Id vertex_index_float = remap_vertex_index_to_float(vertex_index);
      if (IsSPIRVComputeShader()) {
        auto load_compute_vertex_index_float = [this, compute_load_vertex_index,
                                                compute_vertex_index_is_32bit,
                                                &remap_vertex_index_to_float](
                                                   spv::Id vertex_processing_index) {
          spv::Id index = vertex_processing_index;
          SPIRVBuilder::IfBuilder load_vertex_index_if(
              compute_load_vertex_index, spv::SelectionControlDontFlattenMask, *builder_);
          spv::Id loaded_vertex_index;
          {
            id_vector_temp_.clear();
            id_vector_temp_.push_back(
                builder_->makeIntConstant(kSystemConstantVertexIndexLoadAddress));
            spv::Id vertex_index_address = builder_->createBinOp(
                spv::OpIAdd, type_uint,
                builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                                id_vector_temp_),
                    spv::NoPrecision),
                builder_->createBinOp(
                    spv::OpShiftLeftLogical, type_uint, vertex_processing_index,
                    builder_->createTriOp(spv::OpSelect, type_uint, compute_vertex_index_is_32bit,
                                          builder_->makeUintConstant(2),
                                          builder_->makeUintConstant(1))));
            loaded_vertex_index = LoadUint32FromSharedMemory(builder_->createUnaryOp(
                spv::OpBitcast, type_int,
                builder_->createBinOp(spv::OpShiftRightLogical, type_uint, vertex_index_address,
                                      builder_->makeUintConstant(2))));
            loaded_vertex_index = builder_->createTriOp(
                spv::OpSelect, type_uint, compute_vertex_index_is_32bit, loaded_vertex_index,
                builder_->createTriOp(
                    spv::OpBitFieldUExtract, type_uint, loaded_vertex_index,
                    builder_->createBinOp(
                        spv::OpShiftLeftLogical, type_uint,
                        builder_->createBinOp(spv::OpBitwiseAnd, type_uint, vertex_index_address,
                                              builder_->makeUintConstant(2)),
                        builder_->makeUintConstant(3)),
                    builder_->makeUintConstant(16)));
            id_vector_temp_.clear();
            id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantVertexIndexEndian));
            loaded_vertex_index = EndianSwap32Uint(
                loaded_vertex_index,
                builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                                id_vector_temp_),
                    spv::NoPrecision));
          }
          load_vertex_index_if.MakeEndIf();
          index = load_vertex_index_if.CreateMergePhi(loaded_vertex_index, index);
          return remap_vertex_index_to_float(index);
        };
        spv::Id triangle_cp_indexed = builder_->createBinOp(
            spv::OpINotEqual, type_bool,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                  builder_->makeUintConstant(static_cast<unsigned int>(
                                      kSysFlagComputeMemExportTriangleCPIndexed))),
            const_uint_0_);
        spv::Id quad_cp_indexed = builder_->createBinOp(
            spv::OpINotEqual, type_bool,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                  builder_->makeUintConstant(static_cast<unsigned int>(
                                      kSysFlagComputeMemExportQuadCPIndexed))),
            const_uint_0_);
        spv::Id cp_indexed = builder_->createBinOp(spv::OpLogicalOr, type_bool, triangle_cp_indexed,
                                                   quad_cp_indexed);
        SPIRVBuilder::IfBuilder if_cp_indexed(cp_indexed, spv::SelectionControlDontFlattenMask,
                                              *builder_);
        {
          spv::Id cp_count =
              builder_->createTriOp(spv::OpSelect, type_uint, triangle_cp_indexed,
                                    builder_->makeUintConstant(3), builder_->makeUintConstant(4));
          spv::Id cp_in_patch = builder_->createBinOp(spv::OpUMod, type_uint,
                                                      compute_vertex_processing_index, cp_count);
          spv::Id patch_first_vertex_processing_index = builder_->createBinOp(
              spv::OpISub, type_uint, compute_vertex_processing_index, cp_in_patch);
          spv::Id cp_index_0 = load_compute_vertex_index_float(patch_first_vertex_processing_index);
          spv::Id cp_index_1 = load_compute_vertex_index_float(
              builder_->createBinOp(spv::OpIAdd, type_uint, patch_first_vertex_processing_index,
                                    builder_->makeUintConstant(1)));
          spv::Id cp_index_2 = load_compute_vertex_index_float(
              builder_->createBinOp(spv::OpIAdd, type_uint, patch_first_vertex_processing_index,
                                    builder_->makeUintConstant(2)));

          SPIRVBuilder::IfBuilder if_triangle_cp(triangle_cp_indexed,
                                                 spv::SelectionControlDontFlattenMask, *builder_);
          {
            spv::Id cp_is_0 =
                builder_->createBinOp(spv::OpIEqual, type_bool, cp_in_patch, const_uint_0_);
            spv::Id cp_is_1 = builder_->createBinOp(spv::OpIEqual, type_bool, cp_in_patch,
                                                    builder_->makeUintConstant(1));
            spv::Id cp_is_2 = builder_->createBinOp(spv::OpIEqual, type_bool, cp_in_patch,
                                                    builder_->makeUintConstant(2));
            spv::Id domain_z = builder_->createTriOp(spv::OpSelect, type_float, cp_is_2,
                                                     const_float_1, const_float_0);
            spv::Id domain_y = builder_->createTriOp(spv::OpSelect, type_float, cp_is_1,
                                                     const_float_1, const_float_0);
            spv::Id domain_x = builder_->createTriOp(spv::OpSelect, type_float, cp_is_0,
                                                     const_float_1, const_float_0);
            store_register_component(0, 0, domain_z);
            store_register_component(0, 1, domain_y);
            store_register_component(0, 2, domain_x);
            if (GetRegisterCount() >= 2) {
              store_register_component(1, 0, cp_index_0);
              store_register_component(1, 1, cp_index_1);
              store_register_component(1, 2, cp_index_2);
            }
          }
          if_triangle_cp.MakeBeginElse();
          {
            spv::Id cp_is_1 = builder_->createBinOp(spv::OpIEqual, type_bool, cp_in_patch,
                                                    builder_->makeUintConstant(1));
            spv::Id cp_is_2 = builder_->createBinOp(spv::OpIEqual, type_bool, cp_in_patch,
                                                    builder_->makeUintConstant(2));
            spv::Id cp_is_3 = builder_->createBinOp(spv::OpIEqual, type_bool, cp_in_patch,
                                                    builder_->makeUintConstant(3));
            spv::Id domain_x = builder_->createTriOp(
                spv::OpSelect, type_float,
                builder_->createBinOp(spv::OpLogicalOr, type_bool, cp_is_1, cp_is_2), const_float_1,
                const_float_0);
            spv::Id domain_y = builder_->createTriOp(
                spv::OpSelect, type_float,
                builder_->createBinOp(spv::OpLogicalOr, type_bool, cp_is_2, cp_is_3), const_float_1,
                const_float_0);
            store_register_component(0, 0, domain_x);
            store_register_component(0, 1, domain_y);
            store_register_component(0, 2, cp_index_0);
            if (GetRegisterCount() >= 2) {
              spv::Id cp_index_3 = load_compute_vertex_index_float(
                  builder_->createBinOp(spv::OpIAdd, type_uint, patch_first_vertex_processing_index,
                                        builder_->makeUintConstant(3)));
              store_register_component(1, 0, cp_index_1);
              store_register_component(1, 1, cp_index_2);
              store_register_component(1, 2, cp_index_3);
            }
          }
          if_triangle_cp.MakeEndIf();
        }
        if_cp_indexed.MakeBeginElse();
        if (GetRegisterCount() >= 2) {
          spv::Id patch_index_in_register_1 = builder_->createBinOp(
              spv::OpINotEqual, type_bool,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                    builder_->makeUintConstant(static_cast<unsigned int>(
                                        kSysFlagComputeMemExportPatchIndexInRegister1))),
              const_uint_0_);
          SPIRVBuilder::IfBuilder if_patch_index_in_register_1(
              patch_index_in_register_1, spv::SelectionControlDontFlattenMask, *builder_);
          store_register_component(1, 0, vertex_index_float);
          if_patch_index_in_register_1.MakeBeginElse();
          store_register_component(0, 0, vertex_index_float);
          if_patch_index_in_register_1.MakeEndIf();
        } else {
          store_register_component(0, 0, vertex_index_float);
        }
        if_cp_indexed.MakeEndIf();
      } else {
        store_register_component(0, 0, vertex_index_float);
      }
    }
  }
}

void SPIRVShaderTranslator::CompleteVertexOrTessEvalShaderInMain() {
  Modification shader_modification = GetSPIRVShaderModification();

  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeIntConstant(kOutputPerVertexMemberPosition));
  spv::Id position_ptr =
      builder_->createAccessChain(spv::StorageClassOutput, output_per_vertex_, id_vector_temp_);
  spv::Id guest_position = builder_->createLoad(position_ptr, spv::NoPrecision);

  spv::Id position_w = builder_->createCompositeExtract(guest_position, type_float, 3);
  spv::Id is_w_not_reciprocal = builder_->createBinOp(
      spv::OpINotEqual, type_bool,
      builder_->createBinOp(
          spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
          builder_->makeUintConstant(static_cast<unsigned int>(kSysFlagWNotReciprocal))),
      const_uint_0_);
  spv::Id guest_position_w_inv =
      builder_->CreateNoContractionBinOp(spv::OpFDiv, type_float, const_float_1, position_w);
  position_w = builder_->createTriOp(spv::OpSelect, type_float, is_w_not_reciprocal, position_w,
                                     guest_position_w_inv);

  spv::Id position_xyz;

  {
    uint_vector_temp_.clear();
    uint_vector_temp_.push_back(0);
    uint_vector_temp_.push_back(1);
    spv::Id position_xy = builder_->createRvalueSwizzle(spv::NoPrecision, type_float2,
                                                        guest_position, uint_vector_temp_);
    spv::Id is_xy_divided_by_w = builder_->createBinOp(
        spv::OpINotEqual, type_bool,
        builder_->createBinOp(
            spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
            builder_->makeUintConstant(static_cast<unsigned int>(kSysFlagXYDividedByW))),
        const_uint_0_);
    spv::Id guest_position_xy_mul_w = builder_->CreateNoContractionBinOp(
        spv::OpVectorTimesScalar, type_float2, position_xy, position_w);
    position_xy = builder_->createTriOp(
        spv::OpSelect, type_float2,
        builder_->smearScalar(spv::NoPrecision, is_xy_divided_by_w, type_bool2),
        guest_position_xy_mul_w, position_xy);

    spv::Id position_z = builder_->createCompositeExtract(guest_position, type_float, 2);
    spv::Id is_z_divided_by_w = builder_->createBinOp(
        spv::OpINotEqual, type_bool,
        builder_->createBinOp(
            spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
            builder_->makeUintConstant(static_cast<unsigned int>(kSysFlagZDividedByW))),
        const_uint_0_);
    spv::Id guest_position_z_mul_w =
        builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, position_z, position_w);
    position_z = builder_->createTriOp(spv::OpSelect, type_float, is_z_divided_by_w,
                                       guest_position_z_mul_w, position_z);

    {
      std::unique_ptr<spv::Instruction> composite_construct_op = std::make_unique<spv::Instruction>(
          builder_->getUniqueId(), type_float3, spv::OpCompositeConstruct);
      composite_construct_op->addIdOperand(position_xy);
      composite_construct_op->addIdOperand(position_z);
      position_xyz = composite_construct_op->getResultId();
      builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
    }
  }

  u32 clip_distance_next_component = 0;
  u32 cull_distance_next_component = 0;
  if (shader_modification.vertex.user_clip_plane_count) {
    u32 output_per_vertex_member_clip_or_cull_distance =
        shader_modification.vertex.user_clip_plane_cull ? output_per_vertex_member_cull_distance_
                                                        : output_per_vertex_member_clip_distance_;
    REX_ASSERT(output_per_vertex_member_clip_or_cull_distance != UINT32_MAX);
    spv::Id guest_position_formatted;
    {
      // Vectors can be concatenated by createCompositeConstruct despite its assertion.
      std::unique_ptr<spv::Instruction> composite_construct_op = std::make_unique<spv::Instruction>(
          builder_->getUniqueId(), type_float4, spv::OpCompositeConstruct);
      composite_construct_op->addIdOperand(position_xyz);
      composite_construct_op->addIdOperand(position_w);
      guest_position_formatted = composite_construct_op->getResultId();
      builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
    }
    u32& user_clip_plane_next_component = shader_modification.vertex.user_clip_plane_cull
                                              ? cull_distance_next_component
                                              : clip_distance_next_component;
    for (u32 i = 0; i < shader_modification.vertex.user_clip_plane_count; ++i) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantUserClipPlanes));
      id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
      spv::Id user_clip_plane = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      spv::Id user_clip_distance =
          builder_->createBinOp(spv::OpDot, type_float, guest_position_formatted, user_clip_plane);
      id_vector_temp_.clear();
      id_vector_temp_.push_back(
          builder_->makeIntConstant(int(output_per_vertex_member_clip_or_cull_distance)));
      id_vector_temp_.push_back(builder_->makeIntConstant(int(user_clip_plane_next_component)));
      builder_->createStore(user_clip_distance,
                            builder_->createAccessChain(spv::StorageClassOutput, output_per_vertex_,
                                                        id_vector_temp_));
      ++user_clip_plane_next_component;
    }
  }

  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantNdcScale));
  spv::Id ndc_scale =
      builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants_, id_vector_temp_),
                           spv::NoPrecision);
  position_xyz =
      builder_->CreateNoContractionBinOp(spv::OpFMul, type_float3, position_xyz, ndc_scale);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantNdcOffset));
  spv::Id ndc_offset =
      builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants_, id_vector_temp_),
                           spv::NoPrecision);
  spv::Id ndc_offset_mul_w = builder_->CreateNoContractionBinOp(
      spv::OpVectorTimesScalar, type_float3, ndc_offset, position_w);
  position_xyz =
      builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float3, position_xyz, ndc_offset_mul_w);

  spv::Id kill_vertex = spv::NoResult;
  if (GetCurrentShader().GetWritesPointSizeEdgeFlagKillVertex() & 0b100) {
    REX_ASSERT(var_main_point_size_edge_flag_kill_vertex_ != spv::NoResult);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(2));
    spv::Id kill_vertex_bits = builder_->createUnaryOp(
        spv::OpBitcast, type_uint,
        builder_->createLoad(builder_->createAccessChain(spv::StorageClassFunction,
                                                         var_main_point_size_edge_flag_kill_vertex_,
                                                         id_vector_temp_),
                             spv::NoPrecision));
    kill_vertex = builder_->createBinOp(
        spv::OpINotEqual, type_bool,
        builder_->createBinOp(spv::OpBitwiseAnd, type_uint, kill_vertex_bits,
                              builder_->makeUintConstant(UINT32_C(0x7FFFFFFF))),
        const_uint_0_);
  }
  if (kill_vertex != spv::NoResult && shader_modification.vertex.vertex_kill_and &&
      output_per_vertex_member_cull_distance_ != UINT32_MAX) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(
        builder_->makeIntConstant(int(output_per_vertex_member_cull_distance_)));
    id_vector_temp_.push_back(builder_->makeIntConstant(int(cull_distance_next_component)));
    builder_->createStore(
        builder_->createTriOp(spv::OpSelect, type_float, kill_vertex,
                              builder_->makeFloatConstant(-1.0f), const_float_0),
        builder_->createAccessChain(spv::StorageClassOutput, output_per_vertex_, id_vector_temp_));
    ++cull_distance_next_component;
  } else if (kill_vertex != spv::NoResult) {
    position_w = builder_->createTriOp(
        spv::OpSelect, type_float, kill_vertex,
        builder_->createUnaryOp(spv::OpBitcast, type_float,
                                builder_->makeUintConstant(UINT32_C(0x7FC00000))),
        position_w);
  }

  if (output_point_size_ != spv::NoResult) {
    spv::Id point_size;
    if (GetCurrentShader().GetWritesPointSizeEdgeFlagKillVertex() & 0b001) {
      REX_ASSERT(var_main_point_size_edge_flag_kill_vertex_ != spv::NoResult);
      id_vector_temp_.clear();
      id_vector_temp_.push_back(const_int_0_);
      point_size = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassFunction,
                                      var_main_point_size_edge_flag_kill_vertex_, id_vector_temp_),
          spv::NoPrecision);
    } else {
      point_size = builder_->makeFloatConstant(-1.0f);
    }
    builder_->createStore(point_size, output_point_size_);
  }

  if (shader_modification.vertex.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeUintConstant(0b10));
    id_vector_temp_.push_back(builder_->makeUintConstant(0b01));
    spv::Id point_vertex_positive = builder_->createBinOp(
        spv::OpINotEqual, type_bool2,
        builder_->createBinOp(
            spv::OpBitwiseAnd, type_uint2,
            builder_->smearScalar(spv::NoPrecision,
                                  builder_->createUnaryOp(
                                      spv::OpBitcast, type_uint,
                                      builder_->createLoad(input_vertex_index_, spv::NoPrecision)),
                                  type_uint2),
            builder_->createCompositeConstruct(type_uint2, id_vector_temp_)),
        SPIRVSmearScalarResultOrConstant(const_uint_0_, type_uint2));

    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantPointConstantDiameter));
    spv::Id point_guest_diameter = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    if (GetCurrentShader().GetWritesPointSizeEdgeFlagKillVertex() & 0b001) {
      REX_ASSERT(var_main_point_size_edge_flag_kill_vertex_ != spv::NoResult);
      id_vector_temp_.clear();
      id_vector_temp_.push_back(const_int_0_);
      spv::Id point_vertex_diameter = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassFunction,
                                      var_main_point_size_edge_flag_kill_vertex_, id_vector_temp_),
          spv::NoPrecision);
      point_guest_diameter = builder_->createTriOp(
          spv::OpSelect, type_float2,
          builder_->smearScalar(spv::NoPrecision,
                                builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool,
                                                      point_vertex_diameter, const_float_0),
                                type_bool2),
          builder_->smearScalar(spv::NoPrecision, point_vertex_diameter, type_float2),
          point_guest_diameter);
    }
    id_vector_temp_.clear();
    id_vector_temp_.push_back(
        builder_->makeIntConstant(kSystemConstantPointScreenDiameterToNdcRadius));
    spv::Id point_radius = builder_->CreateNoContractionBinOp(
        spv::OpFMul, type_float2, point_guest_diameter,
        builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision));
    point_radius = builder_->CreateNoContractionBinOp(spv::OpVectorTimesScalar, type_float2,
                                                      point_radius, position_w);

    uint_vector_temp_.clear();
    uint_vector_temp_.push_back(0);
    uint_vector_temp_.push_back(1);
    spv::Id point_position_xy = builder_->CreateNoContractionBinOp(
        spv::OpFAdd, type_float2,
        builder_->createRvalueSwizzle(spv::NoPrecision, type_float2, position_xyz,
                                      uint_vector_temp_),
        builder_->createTriOp(
            spv::OpSelect, type_float2, point_vertex_positive, point_radius,
            builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float2, point_radius)));
    spv::Id point_position_z = builder_->createCompositeExtract(position_xyz, type_float, 2);

    spv::Id position;
    {
      // Vectors can be concatenated by createCompositeConstruct despite its assertion.
      std::unique_ptr<spv::Instruction> composite_construct_op = std::make_unique<spv::Instruction>(
          builder_->getUniqueId(), type_float4, spv::OpCompositeConstruct);
      composite_construct_op->addIdOperand(point_position_xy);
      composite_construct_op->addIdOperand(point_position_z);
      composite_construct_op->addIdOperand(position_w);
      position = composite_construct_op->getResultId();
      builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
    }
    builder_->createStore(position, position_ptr);

    if (output_point_coordinates_ != spv::NoResult) {
      builder_->createStore(builder_->createTriOp(spv::OpSelect, type_float2, point_vertex_positive,
                                                  const_float2_1, const_float2_0),
                            output_point_coordinates_);
    }

    bool point_recalculate_clip_distances = shader_modification.vertex.user_clip_plane_count &&
                                            !shader_modification.vertex.user_clip_plane_cull &&
                                            shader_modification.vertex.point_ps_ucp_mode >= 2;
    bool point_recalculate_cull_distances = shader_modification.vertex.user_clip_plane_count &&
                                            shader_modification.vertex.user_clip_plane_cull &&
                                            shader_modification.vertex.point_ps_ucp_mode >= 3;
    if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
      u32 output_per_vertex_member_user_clip_distance =
          point_recalculate_cull_distances ? output_per_vertex_member_cull_distance_
                                           : output_per_vertex_member_clip_distance_;
      REX_ASSERT(output_per_vertex_member_user_clip_distance != UINT32_MAX);

      spv::Id point_position_xyz;
      {
        std::unique_ptr<spv::Instruction> composite_construct_op =
            std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float3,
                                               spv::OpCompositeConstruct);
        composite_construct_op->addIdOperand(point_position_xy);
        composite_construct_op->addIdOperand(point_position_z);
        point_position_xyz = composite_construct_op->getResultId();
        builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
      }
      spv::Id point_guest_position_xyz = builder_->CreateNoContractionBinOp(
          spv::OpFSub, type_float3, point_position_xyz,
          builder_->CreateNoContractionBinOp(spv::OpVectorTimesScalar, type_float3, ndc_offset,
                                             position_w));
      point_guest_position_xyz = builder_->CreateNoContractionBinOp(
          spv::OpFDiv, type_float3, point_guest_position_xyz, ndc_scale);
      spv::Id point_guest_position;
      {
        std::unique_ptr<spv::Instruction> composite_construct_op =
            std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float4,
                                               spv::OpCompositeConstruct);
        composite_construct_op->addIdOperand(point_guest_position_xyz);
        composite_construct_op->addIdOperand(position_w);
        point_guest_position = composite_construct_op->getResultId();
        builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
      }

      for (u32 i = 0; i < shader_modification.vertex.user_clip_plane_count; ++i) {
        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantUserClipPlanes));
        id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
        spv::Id user_clip_plane = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);
        spv::Id user_clip_distance =
            builder_->createBinOp(spv::OpDot, type_float, point_guest_position, user_clip_plane);
        id_vector_temp_.clear();
        id_vector_temp_.push_back(
            builder_->makeIntConstant(int(output_per_vertex_member_user_clip_distance)));
        id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
        builder_->createStore(user_clip_distance,
                              builder_->createAccessChain(spv::StorageClassOutput,
                                                          output_per_vertex_, id_vector_temp_));
      }
    }
  } else {
    spv::Id position;
    {
      // Vectors can be concatenated by createCompositeConstruct despite its assertion.
      std::unique_ptr<spv::Instruction> composite_construct_op = std::make_unique<spv::Instruction>(
          builder_->getUniqueId(), type_float4, spv::OpCompositeConstruct);
      composite_construct_op->addIdOperand(position_xyz);
      composite_construct_op->addIdOperand(position_w);
      position = composite_construct_op->getResultId();
      builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
    }
    builder_->createStore(position, position_ptr);
  }
}

void SPIRVShaderTranslator::StartFragmentShaderBeforeMain() {
  Modification shader_modification = GetSPIRVShaderModification();

  if (edram_fragment_shader_interlock_) {
    builder_->addExtension("SPV_EXT_fragment_shader_interlock");

    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeRuntimeArray(type_uint));
    builder_->addDecoration(id_vector_temp_.back(), spv::DecorationArrayStride, sizeof(u32));
    spv::Id type_edram = builder_->makeStructType(id_vector_temp_, "XeEdram");
    builder_->addMemberName(type_edram, 0, "edram");
    builder_->addMemberDecoration(type_edram, 0, spv::DecorationCoherent);
    builder_->addMemberDecoration(type_edram, 0, spv::DecorationRestrict);
    builder_->addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
    builder_->addDecoration(type_edram, features_.spirv_version >= spv::Spv_1_3
                                            ? spv::DecorationBlock
                                            : spv::DecorationBufferBlock);
    buffer_edram_ = builder_->createVariable(spv::NoPrecision,
                                             features_.spirv_version >= spv::Spv_1_3
                                                 ? spv::StorageClassStorageBuffer
                                                 : spv::StorageClassUniform,
                                             type_edram, "xe_edram");
    builder_->addDecoration(buffer_edram_, spv::DecorationDescriptorSet,
                            int(kDescriptorSetSharedMemoryAndEDRAM));
    builder_->addDecoration(buffer_edram_, spv::DecorationBinding, 1);
    if (features_.spirv_version >= spv::Spv_1_4) {
      main_interface_.push_back(buffer_edram_);
    }
  }

  bool param_gen_needed =
      !is_depth_only_fragment_shader_ && GetPsParamGenInterpolator() != UINT32_MAX;

  if (!is_depth_only_fragment_shader_) {
    u32 input_location = 0;

    {
      u32 interpolators_remaining = GetModificationInterpolatorMask();
      u32 interpolator_index;
      while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
        interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
        spv::Id interpolator = builder_->createVariable(
            spv::NoPrecision, spv::StorageClassInput, type_float4,
            std::format("xe_in_interpolator_{}", interpolator_index).c_str());
        input_output_interpolators_[interpolator_index] = interpolator;
        builder_->addDecoration(interpolator, spv::DecorationLocation, int(input_location));
        if (shader_modification.pixel.interpolators_centroid &
            (UINT32_C(1) << interpolator_index)) {
          builder_->addDecoration(interpolator, spv::DecorationCentroid);
        }
        main_interface_.push_back(interpolator);
        ++input_location;
      }
    }

    if (shader_modification.pixel.param_gen_point) {
      if (param_gen_needed) {
        input_point_coordinates_ = builder_->createVariable(
            spv::NoPrecision, spv::StorageClassInput, type_float2, "xe_in_point_coordinates");
        builder_->addDecoration(input_point_coordinates_, spv::DecorationLocation,
                                int(input_location));
        main_interface_.push_back(input_point_coordinates_);
      }
      ++input_location;
    }
  }

  bool float24_depth_conversion = IsWritingFloat24Depth();
  bool float24_depth_from_fragment_coordinates =
      float24_depth_conversion && !GetCurrentShader().GetWritesDepth();
  bool alpha_to_coverage_possible = !is_depth_only_fragment_shader_ &&
                                    GetCurrentShader().GetWritesColorTarget(0) &&
                                    !IsExecutionModeEarlyFragmentTests();

  bool memexport_resolution_scaled = GetCurrentShader().GetMemExportEMWritten() &&
                                     (draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1);
  if (edram_fragment_shader_interlock_ || param_gen_needed || memexport_resolution_scaled ||
      float24_depth_from_fragment_coordinates || alpha_to_coverage_possible) {
    input_fragment_coordinates_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                           type_float4, "gl_FragCoord");
    builder_->addDecoration(input_fragment_coordinates_, spv::DecorationBuiltIn,
                            spv::BuiltInFragCoord);
    main_interface_.push_back(input_fragment_coordinates_);
  }

  if (edram_fragment_shader_interlock_ ||
      (param_gen_needed && !GetSPIRVShaderModification().pixel.param_gen_point)) {
    input_front_facing_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                   type_bool, "gl_FrontFacing");
    builder_->addDecoration(input_front_facing_, spv::DecorationBuiltIn, spv::BuiltInFrontFacing);
    main_interface_.push_back(input_front_facing_);
  }

  if (edram_fragment_shader_interlock_ || IsSampleRate() ||
      (alpha_to_coverage_possible && features_.sample_rate_shading)) {
    // SampleMask depends on SampleRateShading in some SPIR-V revisions.
    builder_->addCapability(spv::CapabilitySampleRateShading);
  }
  if (IsSampleRate()) {
    input_sample_id_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
    builder_->addDecoration(input_sample_id_, spv::DecorationFlat);
    builder_->addDecoration(input_sample_id_, spv::DecorationBuiltIn, spv::BuiltInSampleId);
    main_interface_.push_back(input_sample_id_);
  }
  if (edram_fragment_shader_interlock_) {
    input_sample_mask_ = builder_->createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder_->makeArrayType(type_int, builder_->makeUintConstant(1), 0), "gl_SampleMaskIn");
    builder_->addDecoration(input_sample_mask_, spv::DecorationFlat);
    builder_->addDecoration(input_sample_mask_, spv::DecorationBuiltIn, spv::BuiltInSampleMask);
    main_interface_.push_back(input_sample_mask_);
  }

  if (!edram_fragment_shader_interlock_) {
    std::fill(output_or_var_fragment_data_.begin(), output_or_var_fragment_data_.end(),
              spv::NoResult);
    if (!is_depth_only_fragment_shader_) {
      static const char* const kFragmentDataOutputNames[] = {
          "xe_out_fragment_data_0",
          "xe_out_fragment_data_1",
          "xe_out_fragment_data_2",
          "xe_out_fragment_data_3",
      };
      u32 color_targets_remaining = GetCurrentShader().GetWritesColorTargets();
      u32 color_target_index;
      while (rex::bit_scan_forward(color_targets_remaining, &color_target_index)) {
        color_targets_remaining &= ~(UINT32_C(1) << color_target_index);
        spv::Id output_fragment_data_rt =
            builder_->createVariable(spv::NoPrecision, spv::StorageClassOutput, type_float4,
                                     kFragmentDataOutputNames[color_target_index]);
        output_or_var_fragment_data_[color_target_index] = output_fragment_data_rt;
        builder_->addDecoration(output_fragment_data_rt, spv::DecorationLocation,
                                int(color_target_index));
        builder_->addDecoration(output_fragment_data_rt, spv::DecorationInvariant);
        main_interface_.push_back(output_fragment_data_rt);
      }
    }
    output_fragment_depth_ = spv::NoResult;
    output_fragment_sample_mask_ = spv::NoResult;
    if (GetCurrentShader().GetWritesDepth() || float24_depth_conversion) {
      output_fragment_depth_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                        type_float, "gl_FragDepth");
      builder_->addDecoration(output_fragment_depth_, spv::DecorationBuiltIn,
                              spv::BuiltInFragDepth);
      main_interface_.push_back(output_fragment_depth_);
    }
    if (alpha_to_coverage_possible && features_.sample_rate_shading) {
      output_fragment_sample_mask_ = builder_->createVariable(
          spv::NoPrecision, spv::StorageClassOutput,
          builder_->makeArrayType(type_int, builder_->makeUintConstant(1), 0), "gl_SampleMask");
      builder_->addDecoration(output_fragment_sample_mask_, spv::DecorationBuiltIn,
                              spv::BuiltInSampleMask);
      main_interface_.push_back(output_fragment_sample_mask_);
    }
  }
}

void SPIRVShaderTranslator::StartFragmentShaderInMain() {
  if (GetCurrentShader().GetKillsPixels()) {
    if (features_.demote_to_helper_invocation) {
      builder_->addExtension("SPV_EXT_demote_to_helper_invocation");
      builder_->addCapability(spv::CapabilityDemoteToHelperInvocationEXT);
    } else {
      var_main_kill_pixel_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_bool,
                                   "xe_var_kill_pixel", builder_->makeBoolConstant(false));
    }
  }

  if (edram_fragment_shader_interlock_) {
    std::fill(output_or_var_fragment_data_.begin(), output_or_var_fragment_data_.end(),
              spv::NoResult);
    var_main_fragment_depth_ = spv::NoResult;
    var_main_fsi_color_written_ = spv::NoResult;
    if (GetCurrentShader().GetWritesDepth()) {
      var_main_fragment_depth_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float,
                                   "xe_var_fragment_depth", const_float_0);
    }
    u32 color_targets_written = GetCurrentShader().GetWritesColorTargets();
    if (color_targets_written) {
      static const char* const kFragmentDataVariableNames[] = {
          "xe_var_fragment_data_0",
          "xe_var_fragment_data_1",
          "xe_var_fragment_data_2",
          "xe_var_fragment_data_3",
      };
      u32 color_targets_remaining = color_targets_written;
      u32 color_target_index;
      while (rex::bit_scan_forward(color_targets_remaining, &color_target_index)) {
        color_targets_remaining &= ~(UINT32_C(1) << color_target_index);
        output_or_var_fragment_data_[color_target_index] = builder_->createVariable(
            spv::NoPrecision, spv::StorageClassFunction, type_float4,
            kFragmentDataVariableNames[color_target_index], const_float4_0);
      }
      var_main_fsi_color_written_ =
          builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_uint,
                                   "xe_var_fsi_color_written", const_uint_0_);
    }
  }

  if (edram_fragment_shader_interlock_ && FSIIsDepthStencilEarly()) {
    spv::Id msaa_samples = LoadMSAASamplesFromFlags();
    FSILoadSampleMask(msaa_samples);
    FSILoadEDRAMOffsets(msaa_samples);
    builder_->createNoResultOp(spv::OpBeginInvocationInterlockEXT);
    FSIDepthStencilTest(msaa_samples, false);
    if (!is_depth_only_fragment_shader_) {
      spv::Id quad_needs_execution =
          builder_->createBinOp(spv::OpINotEqual, type_bool, main_fsi_sample_mask_, const_uint_0_);
      builder_->addCapability(spv::CapabilityDerivativeControl);
      quad_needs_execution = builder_->createBinOp(
          spv::OpLogicalOr, type_bool, quad_needs_execution,
          builder_->createBinOp(
              spv::OpFOrdNotEqual, type_bool,
              builder_->createUnaryOp(
                  spv::OpDPdxFine, type_float,
                  builder_->createTriOp(spv::OpSelect, type_float, quad_needs_execution,
                                        const_float_1, const_float_0)),
              const_float_0));
      quad_needs_execution = builder_->createBinOp(
          spv::OpLogicalOr, type_bool, quad_needs_execution,
          builder_->createBinOp(
              spv::OpFOrdNotEqual, type_bool,
              builder_->createUnaryOp(
                  spv::OpDPdyCoarse, type_float,
                  builder_->createTriOp(spv::OpSelect, type_float, quad_needs_execution,
                                        const_float_1, const_float_0)),
              const_float_0));
      spv::Block& main_fsi_early_depth_stencil_execute_quad = builder_->makeNewBlock();
      main_fsi_early_depth_stencil_execute_quad_merge_ = &builder_->makeNewBlock();
      builder_->createSelectionMerge(main_fsi_early_depth_stencil_execute_quad_merge_,
                                     spv::SelectionControlDontFlattenMask);
      builder_->createConditionalBranch(quad_needs_execution,
                                        &main_fsi_early_depth_stencil_execute_quad,
                                        main_fsi_early_depth_stencil_execute_quad_merge_);
      builder_->setBuildPoint(&main_fsi_early_depth_stencil_execute_quad);
    }
  }

  if (is_depth_only_fragment_shader_) {
    return;
  }

  if (GetCurrentShader().GetMemExportEMWritten()) {
    u32 resolution_scaled_axes = static_cast<u32>(draw_resolution_scale_x_ > 1) |
                                 (static_cast<u32>(draw_resolution_scale_y_ > 1) << 1);
    if (resolution_scaled_axes) {
      REX_ASSERT(input_fragment_coordinates_ != spv::NoResult);
      spv::Id memexport_allowed = spv::NoResult;
      for (u32 i = 0; i < 2; ++i) {
        if (!(resolution_scaled_axes & (UINT32_C(1) << i))) {
          continue;
        }
        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
        spv::Id position = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                        id_vector_temp_),
            spv::NoPrecision);
        spv::Id position_mod = builder_->createBinOp(
            spv::OpUMod, type_uint,
            builder_->createUnaryOp(spv::OpConvertFToU, type_uint, position),
            builder_->makeUintConstant(i ? draw_resolution_scale_y_ : draw_resolution_scale_x_));
        spv::Id position_is_center =
            builder_->createBinOp(spv::OpIEqual, type_bool, position_mod,
                                  builder_->makeUintConstant(i ? (draw_resolution_scale_y_ >> 1)
                                                               : (draw_resolution_scale_x_ >> 1)));
        memexport_allowed = memexport_allowed != spv::NoResult
                                ? builder_->createBinOp(spv::OpLogicalAnd, type_bool,
                                                        memexport_allowed, position_is_center)
                                : position_is_center;
      }
      REX_ASSERT(memexport_allowed != spv::NoResult);
      main_memexport_allowed_ =
          main_memexport_allowed_ != spv::NoResult
              ? builder_->createBinOp(spv::OpLogicalAnd, type_bool, main_memexport_allowed_,
                                      memexport_allowed)
              : memexport_allowed;
    }
    if (IsSampleRate()) {
      REX_ASSERT(input_sample_id_ != spv::NoResult);
      spv::Id sample_id = builder_->createUnaryOp(
          spv::OpBitcast, type_uint, builder_->createLoad(input_sample_id_, spv::NoPrecision));
      spv::Id msaa_samples = LoadMSAASamplesFromFlags();
      spv::Id preferred_sample = builder_->createTriOp(
          spv::OpSelect, type_uint,
          builder_->createBinOp(
              spv::OpUGreaterThanEqual, type_bool, msaa_samples,
              builder_->makeUintConstant(std::to_underlying(xenos::MSAASamples::k4X))),
          builder_->makeUintConstant(3),
          builder_->createTriOp(
              spv::OpSelect, type_uint,
              builder_->createBinOp(
                  spv::OpUGreaterThanEqual, type_bool, msaa_samples,
                  builder_->makeUintConstant(std::to_underlying(xenos::MSAASamples::k2X))),
              builder_->makeUintConstant(native_2x_msaa_with_attachments_ ? 1u : 3u),
              const_uint_0_));
      spv::Id sample_preferred =
          builder_->createBinOp(spv::OpIEqual, type_bool, sample_id, preferred_sample);
      main_memexport_allowed_ =
          main_memexport_allowed_ != spv::NoResult
              ? builder_->createBinOp(spv::OpLogicalAnd, type_bool, main_memexport_allowed_,
                                      sample_preferred)
              : sample_preferred;
    }
  }

  u32 param_gen_interpolator = GetPsParamGenInterpolator();

  u32 interpolator_mask = GetModificationInterpolatorMask();
  for (u32 i = 0; i < GetRegisterCount(); ++i) {
    if (i == param_gen_interpolator) {
      continue;
    }
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(int(i)));
    builder_->createStore(
        (i < xenos::kMaxInterpolators && (interpolator_mask & (UINT32_C(1) << i)))
            ? builder_->createLoad(input_output_interpolators_[i], spv::NoPrecision)
            : const_float4_0,
        builder_->createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                    id_vector_temp_));
  }

  if (param_gen_interpolator != UINT32_MAX) {
    Modification modification = GetSPIRVShaderModification();
    // OpFNegate flips the sign bit of 0.0 only since SPIR-V 1.5 revision 2, so use OpBitwiseXor.
    spv::Id const_sign_bit = builder_->makeUintConstant(UINT32_C(1) << 31);
    REX_ASSERT(input_fragment_coordinates_ != spv::NoResult);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_int_0_);
    spv::Id param_gen_x = builder_->CreateUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450_, GLSLstd450Floor,
        builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                        id_vector_temp_),
            spv::NoPrecision));
    if (draw_resolution_scale_x_ > 1) {
      param_gen_x = builder_->CreateNoContractionBinOp(
          spv::OpFMul, type_float, param_gen_x,
          builder_->makeFloatConstant(1.0f / static_cast<f32>(draw_resolution_scale_x_)));
    }
    param_gen_x = builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_,
                                                   GLSLstd450FAbs, param_gen_x);
    if (!modification.pixel.param_gen_point) {
      REX_ASSERT(input_front_facing_ != spv::NoResult);
      param_gen_x = builder_->createTriOp(
          spv::OpSelect, type_float,
          builder_->createBinOp(
              spv::OpLogicalOr, type_bool,
              builder_->createBinOp(
                  spv::OpIEqual, type_bool,
                  builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                        builder_->makeUintConstant(kSysFlagPrimitivePolygonal)),
                  const_uint_0_),
              builder_->createLoad(input_front_facing_, spv::NoPrecision)),
          param_gen_x,
          builder_->createUnaryOp(
              spv::OpBitcast, type_float,
              builder_->createBinOp(spv::OpBitwiseXor, type_uint,
                                    builder_->createUnaryOp(spv::OpBitcast, type_uint, param_gen_x),
                                    const_sign_bit)));
    }
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(1));
    spv::Id param_gen_y = builder_->CreateUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450_, GLSLstd450Floor,
        builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                        id_vector_temp_),
            spv::NoPrecision));
    if (draw_resolution_scale_y_ > 1) {
      param_gen_y = builder_->CreateNoContractionBinOp(
          spv::OpFMul, type_float, param_gen_y,
          builder_->makeFloatConstant(1.0f / static_cast<f32>(draw_resolution_scale_y_)));
    }
    param_gen_y = builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_,
                                                   GLSLstd450FAbs, param_gen_y);
    if (modification.pixel.param_gen_point) {
      param_gen_y = builder_->createUnaryOp(
          spv::OpBitcast, type_float,
          builder_->createBinOp(spv::OpBitwiseXor, type_uint,
                                builder_->createUnaryOp(spv::OpBitcast, type_uint, param_gen_y),
                                const_sign_bit));
    }
    spv::Id param_gen_z, param_gen_w;
    if (modification.pixel.param_gen_point) {
      REX_ASSERT(input_point_coordinates_ != spv::NoResult);
      spv::Id param_gen_point_coordinates = builder_->CreateTriBuiltinCall(
          type_float2, ext_inst_glsl_std_450_, GLSLstd450NClamp,
          builder_->createLoad(input_point_coordinates_, spv::NoPrecision), const_float2_0,
          const_float2_1);
      param_gen_z = builder_->createCompositeExtract(param_gen_point_coordinates, type_float, 0);
      param_gen_w = builder_->createCompositeExtract(param_gen_point_coordinates, type_float, 1);
    } else {
      param_gen_z = builder_->createUnaryOp(
          spv::OpBitcast, type_float,
          builder_->createTriOp(
              spv::OpSelect, type_uint,
              builder_->createBinOp(
                  spv::OpINotEqual, type_bool,
                  builder_->createBinOp(spv::OpBitwiseAnd, type_uint, main_system_constant_flags_,
                                        builder_->makeUintConstant(kSysFlagPrimitiveLine)),
                  const_uint_0_),
              const_sign_bit, const_uint_0_));
      param_gen_w = const_float_0;
    }
    id_vector_temp_.clear();
    id_vector_temp_.push_back(param_gen_x);
    id_vector_temp_.push_back(param_gen_y);
    id_vector_temp_.push_back(param_gen_z);
    id_vector_temp_.push_back(param_gen_w);
    spv::Id param_gen = builder_->createCompositeConstruct(type_float4, id_vector_temp_);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(int(param_gen_interpolator)));
    builder_->createStore(
        param_gen, builder_->createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                               id_vector_temp_));
  }

  if (!edram_fragment_shader_interlock_) {
    for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      spv::Id output_fragment_data_rt = output_or_var_fragment_data_[i];
      if (output_fragment_data_rt != spv::NoResult) {
        builder_->createStore(const_float4_0, output_fragment_data_rt);
      }
    }
    if (output_fragment_depth_ != spv::NoResult) {
      builder_->createStore(const_float_0, output_fragment_depth_);
    }
  }
}

void SPIRVShaderTranslator::UpdateExecConditionals(ParsedExecInstruction::Type type,
                                                   u32 bool_constant_index, bool condition) {
  if (type == ParsedExecInstruction::Type::kConditional) {
    if (cf_exec_conditional_merge_ && cf_exec_bool_constant_or_predicate_ == bool_constant_index &&
        cf_exec_condition_ == condition) {
      return;
    }
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    if (!cf_exec_predicate_written_ && cf_exec_conditional_merge_ &&
        cf_exec_bool_constant_or_predicate_ == kCfExecBoolConstantPredicate &&
        cf_exec_condition_ == condition) {
      return;
    }
  } else {
    REX_ASSERT(type == ParsedExecInstruction::Type::kUnconditional);
    if (!cf_exec_conditional_merge_) {
      return;
    }
  }

  CloseExecConditionals();

  if (type == ParsedExecInstruction::Type::kUnconditional) {
    return;
  }

  EnsureBuildPointAvailable();
  spv::Id condition_id;
  if (type == ParsedExecInstruction::Type::kConditional) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_int_0_);
    id_vector_temp_.push_back(builder_->makeIntConstant(int(bool_constant_index >> 7)));
    id_vector_temp_.push_back(builder_->makeIntConstant(int((bool_constant_index >> 5) & 3)));
    spv::Id bool_constant_scalar = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_bool_loop_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    condition_id = builder_->createBinOp(
        spv::OpINotEqual, type_bool,
        builder_->createBinOp(
            spv::OpBitwiseAnd, type_uint, bool_constant_scalar,
            builder_->makeUintConstant(static_cast<u32>(1) << (bool_constant_index & 31))),
        const_uint_0_);
    cf_exec_bool_constant_or_predicate_ = bool_constant_index;
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    condition_id = builder_->createLoad(var_main_predicate_, spv::NoPrecision);
    cf_exec_bool_constant_or_predicate_ = kCfExecBoolConstantPredicate;
  } else {
    REX_UNHANDLED_CASE(type);
    return;
  }
  cf_exec_condition_ = condition;
  cf_exec_conditional_merge_ =
      new spv::Block(builder_->getUniqueId(), builder_->getBuildPoint()->getParent());
  builder_->createSelectionMerge(cf_exec_conditional_merge_, spv::SelectionControlDontFlattenMask);
  spv::Block& inner_block = builder_->makeNewBlock();
  builder_->createConditionalBranch(condition_id,
                                    condition ? &inner_block : cf_exec_conditional_merge_,
                                    condition ? cf_exec_conditional_merge_ : &inner_block);
  builder_->setBuildPoint(&inner_block);
}

void SPIRVShaderTranslator::UpdateInstructionPredication(bool predicated, bool condition) {
  if (!predicated) {
    CloseInstructionPredication();
    return;
  }

  if (cf_instruction_predicate_merge_) {
    if (cf_instruction_predicate_condition_ == condition) {
      return;
    }
    CloseInstructionPredication();
  }

  if (!cf_exec_predicate_written_ && cf_exec_conditional_merge_ &&
      cf_exec_bool_constant_or_predicate_ == kCfExecBoolConstantPredicate &&
      cf_exec_condition_ == condition) {
    return;
  }

  cf_instruction_predicate_condition_ = condition;
  EnsureBuildPointAvailable();
  spv::Id predicate_id = builder_->createLoad(var_main_predicate_, spv::NoPrecision);
  spv::Block& predicated_block = builder_->makeNewBlock();
  cf_instruction_predicate_merge_ =
      new spv::Block(builder_->getUniqueId(), builder_->getBuildPoint()->getParent());
  builder_->createSelectionMerge(cf_instruction_predicate_merge_, spv::SelectionControlMaskNone);
  builder_->createConditionalBranch(
      predicate_id, condition ? &predicated_block : cf_instruction_predicate_merge_,
      condition ? cf_instruction_predicate_merge_ : &predicated_block);
  builder_->setBuildPoint(&predicated_block);
}

void SPIRVShaderTranslator::CloseInstructionPredication() {
  if (!cf_instruction_predicate_merge_) {
    return;
  }
  spv::Block& inner_block = *builder_->getBuildPoint();
  if (!inner_block.isTerminated()) {
    builder_->createBranch(cf_instruction_predicate_merge_);
  }
  inner_block.getParent().addBlock(cf_instruction_predicate_merge_);
  builder_->setBuildPoint(cf_instruction_predicate_merge_);
  cf_instruction_predicate_merge_ = nullptr;
}

void SPIRVShaderTranslator::CloseExecConditionals() {
  CloseInstructionPredication();
  if (cf_exec_conditional_merge_) {
    spv::Block& inner_block = *builder_->getBuildPoint();
    if (!inner_block.isTerminated()) {
      builder_->createBranch(cf_exec_conditional_merge_);
    }
    inner_block.getParent().addBlock(cf_exec_conditional_merge_);
    builder_->setBuildPoint(cf_exec_conditional_merge_);
    cf_exec_conditional_merge_ = nullptr;
  }
  cf_exec_predicate_written_ = false;
}

spv::Id SPIRVShaderTranslator::GetStorageAddressingIndex(
    InstructionStorageAddressingMode addressing_mode, u32 storage_index, bool is_float_constant) {
  const Shader::ConstantRegisterMap& constant_register_map =
      GetCurrentShader().GetConstantRegisterMap();
  EnsureBuildPointAvailable();
  spv::Id base_pointer = spv::NoResult;
  switch (addressing_mode) {
    case InstructionStorageAddressingMode::kAbsolute: {
      u32 static_storage_index = storage_index;
      if (is_float_constant) {
        static_storage_index = constant_register_map.GetPackedFloatConstantIndex(storage_index);
        REX_ASSERT(static_storage_index != UINT32_MAX);
        if (static_storage_index == UINT32_MAX) {
          static_storage_index = 0;
        }
      }
      return builder_->makeIntConstant(int(static_storage_index));
    }
    case InstructionStorageAddressingMode::kAddressRegisterRelative:
      base_pointer = var_main_address_register_;
      break;
    case InstructionStorageAddressingMode::kLoopRelative:
      id_vector_temp_util_.clear();
      id_vector_temp_util_.push_back(const_int_0_);
      base_pointer = builder_->createAccessChain(spv::StorageClassFunction, var_main_loop_address_,
                                                 id_vector_temp_util_);
      break;
  }
  REX_ASSERT(!is_float_constant || constant_register_map.float_dynamic_addressing);
  REX_ASSERT(base_pointer != spv::NoResult);
  spv::Id index = builder_->createLoad(base_pointer, spv::NoPrecision);
  if (storage_index) {
    index = builder_->createBinOp(spv::OpIAdd, type_int, index,
                                  builder_->makeIntConstant(int(storage_index)));
  }
  return index;
}

spv::Id SPIRVShaderTranslator::LoadOperandStorage(const InstructionOperand& operand) {
  spv::Id index =
      GetStorageAddressingIndex(operand.storage_addressing_mode, operand.storage_index,
                                operand.storage_source == InstructionStorageSource::kConstantFloat);
  EnsureBuildPointAvailable();
  spv::Id vec4_pointer = spv::NoResult;
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      REX_ASSERT(var_main_registers_ != spv::NoResult);
      id_vector_temp_util_.clear();
      id_vector_temp_util_.push_back(index);
      vec4_pointer = builder_->createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                                 id_vector_temp_util_);
      break;
    case InstructionStorageSource::kConstantFloat:
      REX_ASSERT(uniform_float_constants_ != spv::NoResult);
      id_vector_temp_util_.clear();
      id_vector_temp_util_.push_back(const_int_0_);
      id_vector_temp_util_.push_back(index);
      vec4_pointer = builder_->createAccessChain(spv::StorageClassUniform, uniform_float_constants_,
                                                 id_vector_temp_util_);
      break;
    default:
      REX_UNHANDLED_CASE(operand.storage_source);
  }
  REX_ASSERT(vec4_pointer != spv::NoResult);
  return builder_->createLoad(vec4_pointer, spv::NoPrecision);
}

spv::Id SPIRVShaderTranslator::ApplyOperandModifiers(spv::Id operand_value,
                                                     const InstructionOperand& original_operand,
                                                     bool invert_negate, bool force_absolute) {
  spv::Id type = builder_->getTypeId(operand_value);
  REX_ASSERT(type != spv::NoType);
  if (type == spv::NoType) {
    return operand_value;
  }
  if (original_operand.is_absolute_value || force_absolute) {
    EnsureBuildPointAvailable();
    operand_value = builder_->CreateUnaryBuiltinCall(type, ext_inst_glsl_std_450_, GLSLstd450FAbs,
                                                     operand_value);
  }
  if (original_operand.is_negated != invert_negate) {
    EnsureBuildPointAvailable();
    operand_value = builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type, operand_value);
  }
  return operand_value;
}

spv::Id SPIRVShaderTranslator::GetUnmodifiedOperandComponents(
    spv::Id operand_storage, const InstructionOperand& original_operand, u32 components) {
  REX_ASSERT(components != 0);
  if (!components) {
    return spv::NoResult;
  }
  REX_ASSERT(components <= 0b1111);
  if (components == 0b1111 && original_operand.IsStandardSwizzle()) {
    return operand_storage;
  }
  EnsureBuildPointAvailable();
  u32 component_count = rex::BitCount(components);
  if (component_count == 1) {
    u32 scalar_index;
    rex::bit_scan_forward(components, &scalar_index);
    return builder_->createCompositeExtract(
        operand_storage, type_float,
        static_cast<unsigned int>(original_operand.GetComponent(scalar_index)) -
            static_cast<unsigned int>(SwizzleSource::kX));
  }
  uint_vector_temp_util_.clear();
  u32 components_remaining = components;
  u32 component_index;
  while (rex::bit_scan_forward(components_remaining, &component_index)) {
    components_remaining &= ~(static_cast<u32>(1) << component_index);
    uint_vector_temp_util_.push_back(
        static_cast<unsigned int>(original_operand.GetComponent(component_index)) -
        static_cast<unsigned int>(SwizzleSource::kX));
  }
  return builder_->createRvalueSwizzle(spv::NoPrecision, type_float_vectors[component_count - 1],
                                       operand_storage, uint_vector_temp_util_);
}

void SPIRVShaderTranslator::GetOperandScalarXY(spv::Id operand_storage,
                                               const InstructionOperand& original_operand,
                                               spv::Id& a_out, spv::Id& b_out, bool invert_negate,
                                               bool force_absolute) {
  spv::Id a = GetOperandComponents(operand_storage, original_operand, 0b0001, invert_negate,
                                   force_absolute);
  a_out = a;
  b_out = original_operand.GetComponent(0) != original_operand.GetComponent(1)
              ? GetOperandComponents(operand_storage, original_operand, 0b0010, invert_negate,
                                     force_absolute)
              : a;
}

spv::Id SPIRVShaderTranslator::GetAbsoluteOperand(spv::Id operand_storage,
                                                  const InstructionOperand& original_operand) {
  if (original_operand.is_absolute_value && !original_operand.is_negated) {
    return operand_storage;
  }
  EnsureBuildPointAvailable();
  return builder_->CreateUnaryBuiltinCall(builder_->getTypeId(operand_storage),
                                          ext_inst_glsl_std_450_, GLSLstd450FAbs, operand_storage);
}

void SPIRVShaderTranslator::StoreResult(const InstructionResult& result, spv::Id value,
                                        bool can_store_memexport_address) {
  u32 used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }
  bool is_clamped = result.is_clamped;

  EnsureBuildPointAvailable();

  spv::Id target_pointer = spv::NoResult;
  switch (result.storage_target) {
    case InstructionStorageTarget::kNone:
      break;
    case InstructionStorageTarget::kRegister: {
      REX_ASSERT(var_main_registers_ != spv::NoResult);
      // Call GetStorageAddressingIndex first, as it uses id_vector_temp_util_.
      spv::Id register_index =
          GetStorageAddressingIndex(result.storage_addressing_mode, result.storage_index);
      id_vector_temp_util_.clear();
      id_vector_temp_util_.push_back(register_index);
      target_pointer = builder_->createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                                   id_vector_temp_util_);
    } break;
    case InstructionStorageTarget::kInterpolator: {
      REX_ASSERT(IsVertexShader());
      target_pointer = input_output_interpolators_[result.storage_index];
    } break;
    case InstructionStorageTarget::kPosition: {
      REX_ASSERT(IsVertexShader());
      id_vector_temp_util_.clear();
      id_vector_temp_util_.push_back(builder_->makeIntConstant(kOutputPerVertexMemberPosition));
      target_pointer = builder_->createAccessChain(spv::StorageClassOutput, output_per_vertex_,
                                                   id_vector_temp_util_);
    } break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex: {
      REX_ASSERT(IsVertexShader());
      REX_ASSERT((used_write_mask & 0b1000) == 0);
      target_pointer = var_main_point_size_edge_flag_kill_vertex_;
    } break;
    case InstructionStorageTarget::kColor: {
      REX_ASSERT(IsPixelShader());
      REX_ASSERT(used_write_mask != 0);
      REX_ASSERT(GetCurrentShader().GetWritesColorTarget(result.storage_index));
      target_pointer = output_or_var_fragment_data_[result.storage_index];
      if (edram_fragment_shader_interlock_) {
        REX_ASSERT(var_main_fsi_color_written_ != spv::NoResult);
        builder_->createStore(
            builder_->createBinOp(
                spv::OpBitwiseOr, type_uint,
                builder_->createLoad(var_main_fsi_color_written_, spv::NoPrecision),
                builder_->makeUintConstant(static_cast<u32>(1) << result.storage_index)),
            var_main_fsi_color_written_);
      }
    } break;
    case InstructionStorageTarget::kDepth: {
      REX_ASSERT(IsPixelShader());
      REX_ASSERT(used_write_mask == 0b0001);
      REX_ASSERT(GetCurrentShader().GetWritesDepth());
      target_pointer =
          edram_fragment_shader_interlock_ ? var_main_fragment_depth_ : output_fragment_depth_;
      // Guest depth output is expected to be [0, 1].
      is_clamped = true;
    } break;
    case InstructionStorageTarget::kExportAddress: {
      if (!can_store_memexport_address) {
        return;
      }
      if (!GetCurrentShader().GetMemExportEMWritten()) {
        return;
      }
      target_pointer = var_main_memexport_address_;
    } break;
    case InstructionStorageTarget::kExportData: {
      REX_ASSERT((GetCurrentShader().GetMemExportEMWritten() &
                  (static_cast<u8>(1) << result.storage_index)) != 0);
      target_pointer = var_main_memexport_data_[result.storage_index];
      if (target_pointer != spv::NoResult) {
        REX_ASSERT(var_main_memexport_data_written_ != spv::NoResult);
        builder_->createStore(
            builder_->createBinOp(
                spv::OpBitwiseOr, type_uint,
                builder_->createLoad(var_main_memexport_data_written_, spv::NoPrecision),
                builder_->makeUintConstant(static_cast<u32>(1) << result.storage_index)),
            var_main_memexport_data_written_);
      }
    } break;
    default:
      REX_UNHANDLED_CASE(result.storage_target);
      return;
  }
  if (target_pointer == spv::NoResult) {
    return;
  }

  u32 constant_values;
  u32 constant_components = result.GetUsedConstantComponents(constant_values);
  if (value == spv::NoResult) {
    constant_components = used_write_mask;
  }
  u32 non_constant_components = used_write_mask & ~constant_components;

  unsigned int value_num_components =
      value != spv::NoResult ? static_cast<unsigned int>(builder_->getNumComponents(value)) : 0;

  if (is_clamped && non_constant_components) {
    value = builder_->CreateTriBuiltinCall(type_float_vectors[value_num_components - 1],
                                           ext_inst_glsl_std_450_, GLSLstd450NClamp, value,
                                           const_float_vectors_0[value_num_components - 1],
                                           const_float_vectors_1[value_num_components - 1]);
  }

  u32 used_result_components = result.GetUsedResultComponents();
  unsigned int result_unswizzled_value_components[4] = {};
  if (value_num_components > 1) {
    unsigned int value_component = 0;
    u32 used_result_components_remaining = used_result_components;
    u32 result_component;
    while (rex::bit_scan_forward(used_result_components_remaining, &result_component)) {
      used_result_components_remaining &= ~(static_cast<u32>(1) << result_component);
      result_unswizzled_value_components[result_component] =
          std::min(value_component++, value_num_components - 1);
    }
  }

  unsigned int result_swizzled_value_components[4] = {};
  for (u32 i = 0; i < 4; ++i) {
    if (!(non_constant_components & (1 << i))) {
      continue;
    }
    SwizzleSource swizzle = result.components[i];
    REX_ASSERT(swizzle >= SwizzleSource::kX && swizzle <= SwizzleSource::kW);
    result_swizzled_value_components[i] =
        result_unswizzled_value_components[static_cast<u32>(swizzle) -
                                           std::to_underlying(SwizzleSource::kX)];
  }

  spv::Id target_type = builder_->getDerefTypeId(target_pointer);
  unsigned int target_num_components = builder_->getNumTypeComponents(target_type);
  REX_ASSERT(target_num_components ==
             GetInstructionStorageTargetUsedComponentCount(result.storage_target));
  u32 target_component_mask = (1 << target_num_components) - 1;
  REX_ASSERT((used_write_mask & ~target_component_mask) == 0);

  spv::Id value_to_store;
  if (target_component_mask == used_write_mask) {
    if (!constant_components) {
      if (target_num_components > 1) {
        if (value_num_components > 1) {
          bool is_identity_swizzle = target_num_components == value_num_components;
          for (u32 i = 0; is_identity_swizzle && i < target_num_components; ++i) {
            is_identity_swizzle &= result_swizzled_value_components[i] == i;
          }
          if (is_identity_swizzle) {
            value_to_store = value;
          } else {
            uint_vector_temp_util_.clear();
            uint_vector_temp_util_.insert(uint_vector_temp_util_.cend(),
                                          result_swizzled_value_components,
                                          result_swizzled_value_components + target_num_components);
            value_to_store = builder_->createRvalueSwizzle(spv::NoPrecision, target_type, value,
                                                           uint_vector_temp_util_);
          }
        } else {
          value_to_store = builder_->smearScalar(spv::NoPrecision, value, target_type);
        }
      } else {
        if (value_num_components > 1) {
          value_to_store = builder_->createCompositeExtract(value, type_float,
                                                            result_swizzled_value_components[0]);
        } else {
          value_to_store = value;
        }
      }
    } else if (!non_constant_components) {
      if (target_num_components > 1) {
        id_vector_temp_util_.clear();
        for (u32 i = 0; i < target_num_components; ++i) {
          id_vector_temp_util_.push_back((constant_values & (1 << i)) ? const_float_1
                                                                      : const_float_0);
        }
        value_to_store = builder_->makeCompositeConstant(target_type, id_vector_temp_util_);
      } else {
        value_to_store = (constant_values & 0b0001) ? const_float_1 : const_float_0;
      }
    } else {
      REX_ASSERT(target_num_components > 1);
      if (value_num_components > 1) {
        std::unique_ptr<spv::Instruction> shuffle_op = std::make_unique<spv::Instruction>(
            builder_->getUniqueId(), target_type, spv::OpVectorShuffle);
        shuffle_op->addIdOperand(value);
        shuffle_op->addIdOperand(const_float2_0_1_);
        for (u32 i = 0; i < target_num_components; ++i) {
          shuffle_op->addImmediateOperand((constant_components & (1 << i))
                                              ? value_num_components + ((constant_values >> i) & 1)
                                              : result_swizzled_value_components[i]);
        }
        value_to_store = shuffle_op->getResultId();
        builder_->getBuildPoint()->addInstruction(std::move(shuffle_op));
      } else {
        id_vector_temp_util_.clear();
        for (u32 i = 0; i < target_num_components; ++i) {
          if (constant_components & (1 << i)) {
            id_vector_temp_util_.push_back((constant_values & (1 << i)) ? const_float_1
                                                                        : const_float_0);
          } else {
            id_vector_temp_util_.push_back(value);
          }
        }
        value_to_store = builder_->createCompositeConstruct(target_type, id_vector_temp_util_);
      }
    }
  } else {
    REX_ASSERT(target_num_components > 1);
    value_to_store = builder_->createLoad(target_pointer, spv::NoPrecision);
    if (constant_components) {
      std::unique_ptr<spv::Instruction> shuffle_op = std::make_unique<spv::Instruction>(
          builder_->getUniqueId(), target_type, spv::OpVectorShuffle);
      shuffle_op->addIdOperand(value_to_store);
      shuffle_op->addIdOperand(const_float2_0_1_);
      for (u32 i = 0; i < target_num_components; ++i) {
        shuffle_op->addImmediateOperand((constant_components & (1 << i))
                                            ? target_num_components + ((constant_values >> i) & 1)
                                            : i);
      }
      value_to_store = shuffle_op->getResultId();
      builder_->getBuildPoint()->addInstruction(std::move(shuffle_op));
    }
    if (non_constant_components) {
      if (value_num_components > 1) {
        std::unique_ptr<spv::Instruction> shuffle_op = std::make_unique<spv::Instruction>(
            builder_->getUniqueId(), target_type, spv::OpVectorShuffle);
        shuffle_op->addIdOperand(value_to_store);
        shuffle_op->addIdOperand(value);
        for (u32 i = 0; i < target_num_components; ++i) {
          shuffle_op->addImmediateOperand((non_constant_components & (1 << i))
                                              ? target_num_components +
                                                    result_swizzled_value_components[i]
                                              : i);
        }
        value_to_store = shuffle_op->getResultId();
        builder_->getBuildPoint()->addInstruction(std::move(shuffle_op));
      } else {
        for (u32 i = 0; i < target_num_components; ++i) {
          if (non_constant_components & (1 << i)) {
            value_to_store = builder_->createCompositeInsert(value, value_to_store, target_type, i);
          }
        }
      }
    }
  }

  if (result.storage_target == InstructionStorageTarget::kPointSizeEdgeFlagKillVertex &&
      used_write_mask & 0b001) {
    spv::Id point_size = builder_->createUnaryOp(
        spv::OpBitcast, type_int, builder_->createCompositeExtract(value_to_store, type_float, 0));
    id_vector_temp_util_.clear();
    id_vector_temp_util_.push_back(
        builder_->makeIntConstant(kSystemConstantPointVertexDiameterMin));
    spv::Id point_vertex_diameter_min = builder_->createUnaryOp(
        spv::OpBitcast, type_int,
        builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_util_),
            spv::NoPrecision));
    point_size = builder_->CreateBinBuiltinCall(type_int, ext_inst_glsl_std_450_, GLSLstd450SMax,
                                                point_vertex_diameter_min, point_size);
    id_vector_temp_util_.clear();
    id_vector_temp_util_.push_back(
        builder_->makeIntConstant(kSystemConstantPointVertexDiameterMax));
    spv::Id point_vertex_diameter_max = builder_->createUnaryOp(
        spv::OpBitcast, type_int,
        builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_util_),
            spv::NoPrecision));
    point_size = builder_->CreateBinBuiltinCall(type_int, ext_inst_glsl_std_450_, GLSLstd450SMin,
                                                point_vertex_diameter_max, point_size);
    value_to_store = builder_->createCompositeInsert(
        builder_->createUnaryOp(spv::OpBitcast, type_float, point_size), value_to_store,
        type_float3, 0);
  }

  builder_->createStore(value_to_store, target_pointer);
}

spv::Id SPIRVShaderTranslator::EndianSwap32Uint(spv::Id value, spv::Id endian) {
  spv::Id type = builder_->getTypeId(value);
  spv::Id const_uint_8_scalar = builder_->makeUintConstant(8);
  spv::Id const_uint_00ff00ff_scalar = builder_->makeUintConstant(0x00FF00FF);
  spv::Id const_uint_16_scalar = builder_->makeUintConstant(16);
  spv::Id const_uint_8_typed, const_uint_00ff00ff_typed, const_uint_16_typed;
  int num_components = builder_->getNumTypeComponents(type);
  if (num_components > 1) {
    id_vector_temp_.clear();
    id_vector_temp_.insert(id_vector_temp_.cend(), num_components, const_uint_8_scalar);
    const_uint_8_typed = builder_->makeCompositeConstant(type, id_vector_temp_);
    id_vector_temp_.clear();
    id_vector_temp_.insert(id_vector_temp_.cend(), num_components, const_uint_00ff00ff_scalar);
    const_uint_00ff00ff_typed = builder_->makeCompositeConstant(type, id_vector_temp_);
    id_vector_temp_.clear();
    id_vector_temp_.insert(id_vector_temp_.cend(), num_components, const_uint_16_scalar);
    const_uint_16_typed = builder_->makeCompositeConstant(type, id_vector_temp_);
  } else {
    const_uint_8_typed = const_uint_8_scalar;
    const_uint_00ff00ff_typed = const_uint_00ff00ff_scalar;
    const_uint_16_typed = const_uint_16_scalar;
  }

  spv::Id is_8in16 = builder_->createBinOp(
      spv::OpIEqual, type_bool, endian,
      builder_->makeUintConstant(static_cast<unsigned int>(xenos::Endian::k8in16)));
  spv::Id is_8in32 = builder_->createBinOp(
      spv::OpIEqual, type_bool, endian,
      builder_->makeUintConstant(static_cast<unsigned int>(xenos::Endian::k8in32)));
  spv::Id is_8in16_or_8in32 =
      builder_->createBinOp(spv::OpLogicalOr, type_bool, is_8in16, is_8in32);
  SPIRVBuilder::IfBuilder if_8in16(is_8in16_or_8in32, spv::SelectionControlMaskNone, *builder_);
  spv::Id swapped_8in16;
  {
    swapped_8in16 = builder_->createBinOp(
        spv::OpBitwiseOr, type,
        builder_->createBinOp(
            spv::OpBitwiseAnd, type,
            builder_->createBinOp(spv::OpShiftRightLogical, type, value, const_uint_8_typed),
            const_uint_00ff00ff_typed),
        builder_->createBinOp(
            spv::OpShiftLeftLogical, type,
            builder_->createBinOp(spv::OpBitwiseAnd, type, value, const_uint_00ff00ff_typed),
            const_uint_8_typed));
  }
  if_8in16.MakeEndIf();
  value = if_8in16.CreateMergePhi(swapped_8in16, value);

  spv::Id is_16in32 = builder_->createBinOp(
      spv::OpIEqual, type_bool, endian,
      builder_->makeUintConstant(static_cast<unsigned int>(xenos::Endian::k16in32)));
  spv::Id is_8in32_or_16in32 =
      builder_->createBinOp(spv::OpLogicalOr, type_bool, is_8in32, is_16in32);
  SPIRVBuilder::IfBuilder if_16in32(is_8in32_or_16in32, spv::SelectionControlMaskNone, *builder_);
  spv::Id swapped_16in32;
  {
    swapped_16in32 = builder_->CreateQuadOp(
        spv::OpBitFieldInsert, type,
        builder_->createBinOp(spv::OpShiftRightLogical, type, value, const_uint_16_typed), value,
        builder_->makeIntConstant(16), builder_->makeIntConstant(16));
  }
  if_16in32.MakeEndIf();
  value = if_16in32.CreateMergePhi(swapped_16in32, value);

  return value;
}

spv::Id SPIRVShaderTranslator::EndianSwap128Uint4(spv::Id value, spv::Id endian) {
  spv::Id is_8in64 = builder_->createBinOp(
      spv::OpIEqual, type_bool, endian,
      builder_->makeUintConstant(static_cast<unsigned int>(xenos::Endian128::k8in64)));
  uint_vector_temp_.clear();
  uint_vector_temp_.push_back(1);
  uint_vector_temp_.push_back(0);
  uint_vector_temp_.push_back(3);
  uint_vector_temp_.push_back(2);
  value = builder_->createTriOp(
      spv::OpSelect, type_uint4, is_8in64,
      builder_->createRvalueSwizzle(spv::NoPrecision, type_uint4, value, uint_vector_temp_), value);

  spv::Id is_8in128 = builder_->createBinOp(
      spv::OpIEqual, type_bool, endian,
      builder_->makeUintConstant(static_cast<unsigned int>(xenos::Endian128::k8in128)));
  uint_vector_temp_.clear();
  uint_vector_temp_.push_back(3);
  uint_vector_temp_.push_back(2);
  uint_vector_temp_.push_back(1);
  uint_vector_temp_.push_back(0);
  value = builder_->createTriOp(
      spv::OpSelect, type_uint4, is_8in128,
      builder_->createRvalueSwizzle(spv::NoPrecision, type_uint4, value, uint_vector_temp_), value);

  endian = builder_->createTriOp(
      spv::OpSelect, type_uint,
      builder_->createBinOp(spv::OpLogicalOr, type_bool, is_8in64, is_8in128),
      builder_->makeUintConstant(static_cast<unsigned int>(xenos::Endian128::k8in32)), endian);

  return EndianSwap32Uint(value, endian);
}

spv::Id SPIRVShaderTranslator::LoadUint32FromSharedMemory(spv::Id address_dwords_int) {
  spv::StorageClass storage_class = features_.spirv_version >= spv::Spv_1_3
                                        ? spv::StorageClassStorageBuffer
                                        : spv::StorageClassUniform;

  u32 binding_count_log2 = GetSharedMemoryStorageBufferCountLog2();

  if (!binding_count_log2) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_int_0_);
    id_vector_temp_.push_back(address_dwords_int);
    return builder_->createLoad(
        builder_->createAccessChain(storage_class, buffers_shared_memory_, id_vector_temp_),
        spv::NoPrecision);
  }

  u32 binding_address_bits = (29 - 2) - binding_count_log2;
  spv::Id binding_index =
      builder_->createBinOp(spv::OpShiftRightLogical, type_uint,
                            builder_->createUnaryOp(spv::OpBitcast, type_uint, address_dwords_int),
                            builder_->makeUintConstant(binding_address_bits));
  spv::Id binding_address = builder_->createBinOp(
      spv::OpBitwiseAnd, type_int, address_dwords_int,
      builder_->makeIntConstant(int((static_cast<u32>(1) << binding_address_bits) - 1)));

  auto value_phi_op =
      std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_uint, spv::OpPhi);
  value_phi_op->addIdOperand(const_uint_0_);
  value_phi_op->addIdOperand(builder_->getBuildPoint()->getId());

  SPIRVBuilder::SwitchBuilder binding_switch(binding_index, spv::SelectionControlDontFlattenMask,
                                             *builder_);
  u32 binding_count = static_cast<u32>(1) << binding_count_log2;

  id_vector_temp_.clear();
  id_vector_temp_.push_back(spv::NoResult);
  id_vector_temp_.push_back(const_int_0_);
  id_vector_temp_.push_back(binding_address);

  for (u32 i = 0; i < binding_count; ++i) {
    binding_switch.MakeBeginCase(i);
    id_vector_temp_[0] = builder_->makeIntConstant(int(i));
    value_phi_op->addIdOperand(builder_->createLoad(
        builder_->createAccessChain(storage_class, buffers_shared_memory_, id_vector_temp_),
        spv::NoPrecision));
    value_phi_op->addIdOperand(builder_->getBuildPoint()->getId());
  }

  binding_switch.MakeEndSwitch();

  spv::Id value_phi_result = value_phi_op->getResultId();
  builder_->getBuildPoint()->addInstruction(std::move(value_phi_op));
  return value_phi_result;
}

void SPIRVShaderTranslator::StoreUint32ToSharedMemory(spv::Id value, spv::Id address_dwords_int,
                                                      spv::Id replace_mask) {
  spv::StorageClass storage_class = features_.spirv_version >= spv::Spv_1_3
                                        ? spv::StorageClassStorageBuffer
                                        : spv::StorageClassUniform;

  spv::Id keep_mask = spv::NoResult;
  if (replace_mask != spv::NoResult) {
    keep_mask = builder_->createUnaryOp(spv::OpNot, type_uint, replace_mask);
    value = builder_->createBinOp(spv::OpBitwiseAnd, type_uint, value, replace_mask);
  }

  auto store = [&](spv::Id pointer) {
    if (replace_mask != spv::NoResult) {
      spv::Id const_scope_device =
          builder_->makeUintConstant(static_cast<unsigned int>(spv::ScopeDevice));
      spv::Id const_semantics_relaxed = const_uint_0_;
      builder_->CreateQuadOp(spv::OpAtomicAnd, type_uint, pointer, const_scope_device,
                             const_semantics_relaxed, keep_mask);
      builder_->CreateQuadOp(spv::OpAtomicOr, type_uint, pointer, const_scope_device,
                             const_semantics_relaxed, value);
    } else {
      builder_->createStore(value, pointer);
    }
  };

  u32 binding_count_log2 = GetSharedMemoryStorageBufferCountLog2();

  if (!binding_count_log2) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_int_0_);
    id_vector_temp_.push_back(address_dwords_int);
    store(builder_->createAccessChain(storage_class, buffers_shared_memory_, id_vector_temp_));
    return;
  }

  u32 binding_address_bits = (29 - 2) - binding_count_log2;
  spv::Id binding_index =
      builder_->createBinOp(spv::OpShiftRightLogical, type_uint,
                            builder_->createUnaryOp(spv::OpBitcast, type_uint, address_dwords_int),
                            builder_->makeUintConstant(binding_address_bits));
  spv::Id binding_address = builder_->createBinOp(
      spv::OpBitwiseAnd, type_int, address_dwords_int,
      builder_->makeIntConstant(int((static_cast<u32>(1) << binding_address_bits) - 1)));

  SPIRVBuilder::SwitchBuilder binding_switch(binding_index, spv::SelectionControlDontFlattenMask,
                                             *builder_);
  u32 binding_count = static_cast<u32>(1) << binding_count_log2;

  id_vector_temp_.clear();
  id_vector_temp_.push_back(spv::NoResult);
  id_vector_temp_.push_back(const_int_0_);
  id_vector_temp_.push_back(binding_address);

  for (u32 i = 0; i < binding_count; ++i) {
    binding_switch.MakeBeginCase(i);
    id_vector_temp_[0] = builder_->makeIntConstant(int(i));
    store(builder_->createAccessChain(storage_class, buffers_shared_memory_, id_vector_temp_));
  }

  binding_switch.MakeEndSwitch();
}

spv::Id SPIRVShaderTranslator::PWLGammaToLinear(spv::Id gamma, bool gamma_pre_saturated) {
  spv::Id value_type = builder_->getTypeId(gamma);
  REX_ASSERT(builder_->isFloatType(builder_->getScalarTypeId(value_type)));
  bool is_vector = builder_->isVectorType(value_type);
  REX_ASSERT(is_vector || builder_->isFloatType(value_type));
  int num_components = builder_->getNumTypeComponents(value_type);
  REX_ASSERT(num_components < 4);
  spv::Id bool_type = type_bool_vectors[num_components - 1];

  spv::Id const_vector_0 = const_float_vectors_0[num_components - 1];
  spv::Id const_vector_1 =
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(1.0f), value_type);

  if (!gamma_pre_saturated) {
    gamma = builder_->CreateTriBuiltinCall(value_type, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                                           gamma, const_vector_0, const_vector_1);
  }

  spv::Id is_piece_at_least_3 = builder_->createBinOp(
      spv::OpFOrdGreaterThanEqual, bool_type, gamma,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(192.0f / 255.0f), value_type));
  spv::Id scale_3_or_2 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_3,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(8.0f / 1024.0f), value_type),
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(4.0f / 1024.0f), value_type));
  spv::Id offset_3_or_2 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_3,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(-1024.0f), value_type),
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(-256.0f), value_type));

  spv::Id is_piece_at_least_1 = builder_->createBinOp(
      spv::OpFOrdGreaterThanEqual, bool_type, gamma,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(64.0f / 255.0f), value_type));
  spv::Id scale_1_or_0 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_1,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(2.0f / 1024.0f), value_type),
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(1.0f / 1024.0f), value_type));
  spv::Id offset_1_or_0 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_1,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(-64.0f), value_type),
      const_vector_0);

  spv::Id is_piece_at_least_2 = builder_->createBinOp(
      spv::OpFOrdGreaterThanEqual, bool_type, gamma,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(96.0f / 255.0f), value_type));
  spv::Id scale = builder_->createTriOp(spv::OpSelect, value_type, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
  spv::Id offset = builder_->createTriOp(spv::OpSelect, value_type, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);

  spv::Op value_times_scalar_opcode = is_vector ? spv::OpVectorTimesScalar : spv::OpFMul;
  spv::Id linear = builder_->CreateNoContractionBinOp(
      spv::OpFAdd, value_type,
      builder_->CreateNoContractionBinOp(
          spv::OpFMul, value_type,
          builder_->CreateNoContractionBinOp(value_times_scalar_opcode, value_type, gamma,
                                             builder_->makeFloatConstant(255.0f * 1024.0f)),
          scale),
      offset);
  linear = builder_->CreateNoContractionBinOp(
      spv::OpFAdd, value_type, linear,
      builder_->CreateUnaryBuiltinCall(
          value_type, ext_inst_glsl_std_450_, GLSLstd450Trunc,
          builder_->CreateNoContractionBinOp(spv::OpFMul, value_type, linear, scale)));
  linear = builder_->CreateNoContractionBinOp(value_times_scalar_opcode, value_type, linear,
                                              builder_->makeFloatConstant(1.0f / 1023.0f));
  return linear;
}

spv::Id SPIRVShaderTranslator::LinearToPWLGamma(spv::Id linear, bool linear_pre_saturated) {
  spv::Id value_type = builder_->getTypeId(linear);
  REX_ASSERT(builder_->isFloatType(builder_->getScalarTypeId(value_type)));
  bool is_vector = builder_->isVectorType(value_type);
  REX_ASSERT(is_vector || builder_->isFloatType(value_type));
  int num_components = builder_->getNumTypeComponents(value_type);
  REX_ASSERT(num_components < 4);
  spv::Id bool_type = type_bool_vectors[num_components - 1];

  spv::Id const_vector_0 = const_float_vectors_0[num_components - 1];
  spv::Id const_vector_1 =
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(1.0f), value_type);

  if (!linear_pre_saturated) {
    linear = builder_->CreateTriBuiltinCall(value_type, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                                            linear, const_vector_0, const_vector_1);
  }

  spv::Id is_piece_at_least_3 = builder_->createBinOp(
      spv::OpFOrdGreaterThanEqual, bool_type, linear,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(512.0f / 1023.0f), value_type));
  spv::Id scale_3_or_2 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_3,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(1023.0f / 8.0f), value_type),
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(1023.0f / 4.0f), value_type));
  spv::Id offset_3_or_2 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_3,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(128.0f / 255.0f), value_type),
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(64.0f / 255.0f), value_type));

  spv::Id is_piece_at_least_1 = builder_->createBinOp(
      spv::OpFOrdGreaterThanEqual, bool_type, linear,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(64.0f / 1023.0f), value_type));
  spv::Id scale_1_or_0 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_1,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(1023.0f / 2.0f), value_type),
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(1023.0f), value_type));
  spv::Id offset_1_or_0 = builder_->createTriOp(
      spv::OpSelect, value_type, is_piece_at_least_1,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(32.0f / 255.0f), value_type),
      const_vector_0);

  spv::Id is_piece_at_least_2 = builder_->createBinOp(
      spv::OpFOrdGreaterThanEqual, bool_type, linear,
      SPIRVSmearScalarResultOrConstant(builder_->makeFloatConstant(128.0f / 1023.0f), value_type));
  spv::Id scale = builder_->createTriOp(spv::OpSelect, value_type, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
  spv::Id offset = builder_->createTriOp(spv::OpSelect, value_type, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);

  return builder_->CreateNoContractionBinOp(
      spv::OpFAdd, value_type,
      builder_->CreateNoContractionBinOp(
          is_vector ? spv::OpVectorTimesScalar : spv::OpFMul, value_type,
          builder_->CreateUnaryBuiltinCall(
              value_type, ext_inst_glsl_std_450_, GLSLstd450Trunc,
              builder_->CreateNoContractionBinOp(spv::OpFMul, value_type, linear, scale)),
          builder_->makeFloatConstant(1.0f / 255.0f)),
      offset);
}

}  // namespace rex::gpu
