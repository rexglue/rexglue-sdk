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

#include <cstdint>
#include <memory>
#include <utility>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/render_target/cache.h"
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include "gpu/xenos/draw_state.h"
#include <rex/math.h>

namespace rex::gpu {

spv::Id SPIRVShaderTranslator::PreClampedFloat32To7e3(SPIRVBuilder& builder, spv::Id f32_scalar,
                                                      spv::Id ext_inst_glsl_std_450) {
  spv::Id type_uint = builder.makeUintType(32);

  {
    spv::Id source_type = builder.getTypeId(f32_scalar);
    REX_ASSERT(builder.isScalarType(source_type));
    if (!builder.isUintType(source_type)) {
      f32_scalar = builder.createUnaryOp(spv::OpBitcast, type_uint, f32_scalar);
    }
  }

  spv::Id denormal_biased_f32;
  {
    spv::Instruction* denormal_insert_instruction =
        new spv::Instruction(builder.getUniqueId(), type_uint, spv::OpBitFieldInsert);
    denormal_insert_instruction->addIdOperand(f32_scalar);
    denormal_insert_instruction->addIdOperand(builder.makeUintConstant(1));
    denormal_insert_instruction->addIdOperand(builder.makeUintConstant(23));
    denormal_insert_instruction->addIdOperand(builder.makeUintConstant(9));
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(denormal_insert_instruction));
    denormal_biased_f32 = denormal_insert_instruction->getResultId();
  }
  // Shift overflow is undefined in SPIR-V, so clamp the shift amount.
  spv::Id denormal_biased_f32_shift_amount;
  {
    spv::Instruction* denormal_shift_amount_instruction =
        new spv::Instruction(builder.getUniqueId(), type_uint, spv::OpExtInst);
    denormal_shift_amount_instruction->addIdOperand(ext_inst_glsl_std_450);
    denormal_shift_amount_instruction->addImmediateOperand(GLSLstd450UMin);
    denormal_shift_amount_instruction->addIdOperand(
        builder.createBinOp(spv::OpISub, type_uint, builder.makeUintConstant(125),
                            builder.createBinOp(spv::OpShiftRightLogical, type_uint, f32_scalar,
                                                builder.makeUintConstant(23))));
    denormal_shift_amount_instruction->addIdOperand(builder.makeUintConstant(24));
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(denormal_shift_amount_instruction));
    denormal_biased_f32_shift_amount = denormal_shift_amount_instruction->getResultId();
  }
  denormal_biased_f32 = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                            denormal_biased_f32, denormal_biased_f32_shift_amount);

  spv::Id normal_biased_f32 = builder.createBinOp(spv::OpISub, type_uint, f32_scalar,
                                                  builder.makeUintConstant(UINT32_C(124) << 23));

  spv::Id biased_f32 =
      builder.createTriOp(spv::OpSelect, type_uint,
                          builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), f32_scalar,
                                              builder.makeUintConstant(0x3E800000)),
                          denormal_biased_f32, normal_biased_f32);

  return builder.createTriOp(
      spv::OpBitFieldUExtract, type_uint,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIAdd, type_uint, biased_f32, builder.makeUintConstant(0x7FFF)),
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, biased_f32,
                              builder.makeUintConstant(16), builder.makeUintConstant(1))),
      builder.makeUintConstant(16), builder.makeUintConstant(10));
}

spv::Id SPIRVShaderTranslator::UnclampedFloat32To7e3(SPIRVBuilder& builder, spv::Id f32_scalar,
                                                     spv::Id ext_inst_glsl_std_450) {
  spv::Id type_float = builder.makeFloatType(32);

  {
    spv::Id source_type = builder.getTypeId(f32_scalar);
    REX_ASSERT(builder.isScalarType(source_type));
    if (!builder.isFloatType(source_type)) {
      f32_scalar = builder.createUnaryOp(spv::OpBitcast, type_float, f32_scalar);
    }
  }

  {
    spv::Instruction* clamp_instruction =
        new spv::Instruction(builder.getUniqueId(), type_float, spv::OpExtInst);
    clamp_instruction->addIdOperand(ext_inst_glsl_std_450);
    clamp_instruction->addImmediateOperand(GLSLstd450NClamp);
    clamp_instruction->addIdOperand(f32_scalar);
    clamp_instruction->addIdOperand(builder.makeFloatConstant(0.0f));
    clamp_instruction->addIdOperand(builder.makeFloatConstant(31.875f));
    builder.getBuildPoint()->addInstruction(std::unique_ptr<spv::Instruction>(clamp_instruction));
    f32_scalar = clamp_instruction->getResultId();
  }

  return PreClampedFloat32To7e3(builder, f32_scalar, ext_inst_glsl_std_450);
}

spv::Id SPIRVShaderTranslator::Float7e3To32(SPIRVBuilder& builder, spv::Id f10_uint_scalar,
                                            u32 f10_shift, bool result_as_uint,
                                            spv::Id ext_inst_glsl_std_450) {
  REX_ASSERT(builder.isUintType(builder.getTypeId(f10_uint_scalar)));
  REX_ASSERT(f10_shift <= (32 - 10));

  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_uint = builder.makeUintType(32);

  spv::Id f10_unbiased_exponent =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, f10_uint_scalar,
                          builder.makeUintConstant(f10_shift + 7), builder.makeUintConstant(3));
  spv::Id f10_mantissa =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, f10_uint_scalar,
                          builder.makeUintConstant(f10_shift), builder.makeUintConstant(7));

  spv::Id denormal_mantissa_msb;
  {
    spv::Instruction* denormal_mantissa_msb_instruction =
        new spv::Instruction(builder.getUniqueId(), type_int, spv::OpExtInst);
    denormal_mantissa_msb_instruction->addIdOperand(ext_inst_glsl_std_450);
    denormal_mantissa_msb_instruction->addImmediateOperand(GLSLstd450FindUMsb);
    denormal_mantissa_msb_instruction->addIdOperand(f10_mantissa);
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(denormal_mantissa_msb_instruction));
    denormal_mantissa_msb = denormal_mantissa_msb_instruction->getResultId();
  }
  denormal_mantissa_msb = builder.createUnaryOp(spv::OpBitcast, type_uint, denormal_mantissa_msb);
  spv::Id denormal_f32_unbiased_exponent = builder.createBinOp(
      spv::OpISub, type_uint, denormal_mantissa_msb, builder.makeUintConstant(6));
  spv::Id denormal_f32_mantissa =
      builder.createBinOp(spv::OpShiftLeftLogical, type_uint, f10_mantissa,
                          builder.createBinOp(spv::OpISub, type_uint, builder.makeUintConstant(7),
                                              denormal_mantissa_msb));
  spv::Id f10_mantissa_is_nonzero =
      builder.createBinOp(spv::OpINotEqual, type_bool, f10_mantissa, builder.makeUintConstant(0));
  denormal_f32_unbiased_exponent = builder.createTriOp(
      spv::OpSelect, type_uint, f10_mantissa_is_nonzero, denormal_f32_unbiased_exponent,
      builder.makeUintConstant(static_cast<u32>(-124)));
  denormal_f32_mantissa = builder.createTriOp(spv::OpSelect, type_uint, f10_mantissa_is_nonzero,
                                              denormal_f32_mantissa, builder.makeUintConstant(0));

  spv::Id f10_is_normal = builder.createBinOp(spv::OpINotEqual, type_bool, f10_unbiased_exponent,
                                              builder.makeUintConstant(0));
  spv::Id f32_unbiased_exponent =
      builder.createTriOp(spv::OpSelect, type_uint, f10_is_normal, f10_unbiased_exponent,
                          denormal_f32_unbiased_exponent);
  spv::Id f32_mantissa = builder.createTriOp(spv::OpSelect, type_uint, f10_is_normal, f10_mantissa,
                                             denormal_f32_mantissa);

  spv::Id f32_shifted;
  {
    spv::Instruction* f32_insert_instruction =
        new spv::Instruction(builder.getUniqueId(), type_uint, spv::OpBitFieldInsert);
    f32_insert_instruction->addIdOperand(f32_mantissa);
    f32_insert_instruction->addIdOperand(builder.createBinOp(
        spv::OpIAdd, type_uint, f32_unbiased_exponent, builder.makeUintConstant(124)));
    f32_insert_instruction->addIdOperand(builder.makeUintConstant(7));
    f32_insert_instruction->addIdOperand(builder.makeUintConstant(8));
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(f32_insert_instruction));
    f32_shifted = f32_insert_instruction->getResultId();
  }
  spv::Id f32 = builder.createBinOp(spv::OpShiftLeftLogical, type_uint, f32_shifted,
                                    builder.makeUintConstant(23 - 7));

  if (!result_as_uint) {
    f32 = builder.createUnaryOp(spv::OpBitcast, builder.makeFloatType(32), f32);
  }

  return f32;
}

spv::Id SPIRVShaderTranslator::PreClampedDepthTo20e4(SPIRVBuilder& builder, spv::Id f32_scalar,
                                                     bool round_to_nearest_even,
                                                     bool remap_from_0_to_0_5,
                                                     spv::Id ext_inst_glsl_std_450) {
  u32 remap_bias = static_cast<u32>(remap_from_0_to_0_5);

  spv::Id type_uint = builder.makeUintType(32);

  {
    spv::Id source_type = builder.getTypeId(f32_scalar);
    REX_ASSERT(builder.isScalarType(source_type));
    if (!builder.isUintType(source_type)) {
      f32_scalar = builder.createUnaryOp(spv::OpBitcast, type_uint, f32_scalar);
    }
  }

  spv::Id denormal_biased_f32;
  {
    spv::Instruction* denormal_insert_instruction =
        new spv::Instruction(builder.getUniqueId(), type_uint, spv::OpBitFieldInsert);
    denormal_insert_instruction->addIdOperand(f32_scalar);
    denormal_insert_instruction->addIdOperand(builder.makeUintConstant(1));
    denormal_insert_instruction->addIdOperand(builder.makeUintConstant(23));
    denormal_insert_instruction->addIdOperand(builder.makeUintConstant(9));
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(denormal_insert_instruction));
    denormal_biased_f32 = denormal_insert_instruction->getResultId();
  }
  // Shift overflow is undefined in SPIR-V, so clamp the shift amount.
  spv::Id denormal_biased_f32_shift_amount;
  {
    spv::Instruction* denormal_shift_amount_instruction =
        new spv::Instruction(builder.getUniqueId(), type_uint, spv::OpExtInst);
    denormal_shift_amount_instruction->addIdOperand(ext_inst_glsl_std_450);
    denormal_shift_amount_instruction->addImmediateOperand(GLSLstd450UMin);
    denormal_shift_amount_instruction->addIdOperand(
        builder.createBinOp(spv::OpISub, type_uint, builder.makeUintConstant(113 - remap_bias),
                            builder.createBinOp(spv::OpShiftRightLogical, type_uint, f32_scalar,
                                                builder.makeUintConstant(23))));
    denormal_shift_amount_instruction->addIdOperand(builder.makeUintConstant(24));
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(denormal_shift_amount_instruction));
    denormal_biased_f32_shift_amount = denormal_shift_amount_instruction->getResultId();
  }
  denormal_biased_f32 = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                            denormal_biased_f32, denormal_biased_f32_shift_amount);

  spv::Id normal_biased_f32 =
      builder.createBinOp(spv::OpISub, type_uint, f32_scalar,
                          builder.makeUintConstant((UINT32_C(112) - remap_bias) << 23));

  spv::Id biased_f32 = builder.createTriOp(
      spv::OpSelect, type_uint,
      builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), f32_scalar,
                          builder.makeUintConstant(0x38800000 - (remap_bias << 23))),
      denormal_biased_f32, normal_biased_f32);

  if (round_to_nearest_even) {
    biased_f32 = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(spv::OpIAdd, type_uint, biased_f32, builder.makeUintConstant(3)),
        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, biased_f32,
                            builder.makeUintConstant(3), builder.makeUintConstant(1)));
  }
  return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, biased_f32,
                             builder.makeUintConstant(3), builder.makeUintConstant(24));
}

spv::Id SPIRVShaderTranslator::Depth20e4To32(SPIRVBuilder& builder, spv::Id f24_uint_scalar,
                                             u32 f24_shift, bool remap_to_0_to_0_5,
                                             bool result_as_uint, spv::Id ext_inst_glsl_std_450) {
  REX_ASSERT(builder.isUintType(builder.getTypeId(f24_uint_scalar)));
  REX_ASSERT(f24_shift <= (32 - 24));

  u32 remap_bias = static_cast<u32>(remap_to_0_to_0_5);

  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_uint = builder.makeUintType(32);

  spv::Id f24_unbiased_exponent =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, f24_uint_scalar,
                          builder.makeUintConstant(f24_shift + 20), builder.makeUintConstant(4));
  spv::Id f24_mantissa =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, f24_uint_scalar,
                          builder.makeUintConstant(f24_shift), builder.makeUintConstant(20));

  spv::Id denormal_mantissa_msb;
  {
    spv::Instruction* denormal_mantissa_msb_instruction =
        new spv::Instruction(builder.getUniqueId(), type_int, spv::OpExtInst);
    denormal_mantissa_msb_instruction->addIdOperand(ext_inst_glsl_std_450);
    denormal_mantissa_msb_instruction->addImmediateOperand(GLSLstd450FindUMsb);
    denormal_mantissa_msb_instruction->addIdOperand(f24_mantissa);
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(denormal_mantissa_msb_instruction));
    denormal_mantissa_msb = denormal_mantissa_msb_instruction->getResultId();
  }
  denormal_mantissa_msb = builder.createUnaryOp(spv::OpBitcast, type_uint, denormal_mantissa_msb);
  spv::Id denormal_f32_unbiased_exponent = builder.createBinOp(
      spv::OpISub, type_uint, denormal_mantissa_msb, builder.makeUintConstant(19));
  spv::Id denormal_f32_mantissa =
      builder.createBinOp(spv::OpShiftLeftLogical, type_uint, f24_mantissa,
                          builder.createBinOp(spv::OpISub, type_uint, builder.makeUintConstant(20),
                                              denormal_mantissa_msb));
  spv::Id f24_mantissa_is_nonzero =
      builder.createBinOp(spv::OpINotEqual, type_bool, f24_mantissa, builder.makeUintConstant(0));
  denormal_f32_unbiased_exponent = builder.createTriOp(
      spv::OpSelect, type_uint, f24_mantissa_is_nonzero, denormal_f32_unbiased_exponent,
      builder.makeUintConstant(static_cast<u32>(-static_cast<i32>(112 - remap_bias))));
  denormal_f32_mantissa = builder.createTriOp(spv::OpSelect, type_uint, f24_mantissa_is_nonzero,
                                              denormal_f32_mantissa, builder.makeUintConstant(0));

  spv::Id f24_is_normal = builder.createBinOp(spv::OpINotEqual, type_bool, f24_unbiased_exponent,
                                              builder.makeUintConstant(0));
  spv::Id f32_unbiased_exponent =
      builder.createTriOp(spv::OpSelect, type_uint, f24_is_normal, f24_unbiased_exponent,
                          denormal_f32_unbiased_exponent);
  spv::Id f32_mantissa = builder.createTriOp(spv::OpSelect, type_uint, f24_is_normal, f24_mantissa,
                                             denormal_f32_mantissa);

  spv::Id f32_shifted;
  {
    spv::Instruction* f32_insert_instruction =
        new spv::Instruction(builder.getUniqueId(), type_uint, spv::OpBitFieldInsert);
    f32_insert_instruction->addIdOperand(f32_mantissa);
    f32_insert_instruction->addIdOperand(builder.createBinOp(
        spv::OpIAdd, type_uint, f32_unbiased_exponent, builder.makeUintConstant(112 - remap_bias)));
    f32_insert_instruction->addIdOperand(builder.makeUintConstant(20));
    f32_insert_instruction->addIdOperand(builder.makeUintConstant(8));
    builder.getBuildPoint()->addInstruction(
        std::unique_ptr<spv::Instruction>(f32_insert_instruction));
    f32_shifted = f32_insert_instruction->getResultId();
  }
  spv::Id f32 = builder.createBinOp(spv::OpShiftLeftLogical, type_uint, f32_shifted,
                                    builder.makeUintConstant(23 - 20));

  if (!result_as_uint) {
    f32 = builder.createUnaryOp(spv::OpBitcast, builder.makeFloatType(32), f32);
  }

  return f32;
}

void SPIRVShaderTranslator::CompleteFragmentShaderInMain() {
  spv::Id msaa_samples = spv::NoResult;

  if (edram_fragment_shader_interlock_ && !FSI_IsDepthStencilEarly()) {
    if (msaa_samples == spv::NoResult) {
      msaa_samples = LoadMSAASamplesFromFlags();
    }
    FSI_LoadSampleMask(msaa_samples);
  }

  bool fsi_pixel_potentially_killed = false;

  if (current_shader().kills_pixels()) {
    if (edram_fragment_shader_interlock_) {
      fsi_pixel_potentially_killed = true;
      if (!features_.demote_to_helper_invocation) {
        REX_ASSERT(var_main_kill_pixel_ != spv::NoResult);
        main_fsi_sample_mask_ = builder_->createTriOp(
            spv::OpSelect, type_uint_, builder_->createLoad(var_main_kill_pixel_, spv::NoPrecision),
            const_uint_0_, main_fsi_sample_mask_);
      }
    } else {
      if (!features_.demote_to_helper_invocation) {
        REX_ASSERT(var_main_kill_pixel_ != spv::NoResult);
        SPIRVBuilder::IfBuilder kill_pixel_if(
            builder_->createLoad(var_main_kill_pixel_, spv::NoPrecision),
            spv::SelectionControlMaskNone, *builder_);
        builder_->createNoResultOp(spv::OpKill);
        kill_pixel_if.makeEndIf(false);
      }
    }
  }

  u32 color_targets_written = current_shader().writes_color_targets();

  if ((color_targets_written & 0b1) && !IsExecutionModeEarlyFragmentTests()) {
    spv::Id fsi_sample_mask_in_rt_0_alpha_tests = spv::NoResult;
    spv::Block* block_fsi_rt_0_alpha_tests_rt_written_head = nullptr;
    spv::Block* block_fsi_rt_0_alpha_tests_rt_written_merge = nullptr;
    if (!edram_fragment_shader_interlock_ && output_fragment_sample_mask_ != spv::NoResult) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(const_int_0_);
      builder_->createStore(
          builder_->createUnaryOp(spv::OpBitcast, type_int_,
                                  builder_->makeUintConstant(UINT32_MAX)),
          builder_->createAccessChain(spv::StorageClassOutput, output_fragment_sample_mask_,
                                      id_vector_temp_));
    }
    builder_->makeNewBlock();
    if (edram_fragment_shader_interlock_) {
      fsi_sample_mask_in_rt_0_alpha_tests = main_fsi_sample_mask_;
      spv::Id rt_0_written = builder_->createBinOp(
          spv::OpINotEqual, type_bool_,
          builder_->createBinOp(spv::OpBitwiseAnd, type_uint_,
                                builder_->createLoad(var_main_fsi_color_written_, spv::NoPrecision),
                                builder_->makeUintConstant(0b1)),
          const_uint_0_);
      block_fsi_rt_0_alpha_tests_rt_written_head = builder_->getBuildPoint();
      spv::Block& block_fsi_rt_0_alpha_tests_rt_written = builder_->makeNewBlock();
      block_fsi_rt_0_alpha_tests_rt_written_merge = &builder_->makeNewBlock();
      builder_->createSelectionMerge(block_fsi_rt_0_alpha_tests_rt_written_merge,
                                     spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> rt_0_written_branch_conditional_op =
            std::make_unique<spv::Instruction>(spv::OpBranchConditional);
        rt_0_written_branch_conditional_op->addIdOperand(rt_0_written);
        rt_0_written_branch_conditional_op->addIdOperand(
            block_fsi_rt_0_alpha_tests_rt_written.getId());
        rt_0_written_branch_conditional_op->addIdOperand(
            block_fsi_rt_0_alpha_tests_rt_written_merge->getId());
        rt_0_written_branch_conditional_op->addImmediateOperand(2);
        rt_0_written_branch_conditional_op->addImmediateOperand(1);
        builder_->getBuildPoint()->addInstruction(std::move(rt_0_written_branch_conditional_op));
      }
      block_fsi_rt_0_alpha_tests_rt_written.addPredecessor(
          block_fsi_rt_0_alpha_tests_rt_written_head);
      block_fsi_rt_0_alpha_tests_rt_written_merge->addPredecessor(
          block_fsi_rt_0_alpha_tests_rt_written_head);
      builder_->setBuildPoint(&block_fsi_rt_0_alpha_tests_rt_written);
    }

    spv::Id alpha_test_function = builder_->createTriOp(
        spv::OpBitFieldUExtract, type_uint_, main_system_constant_flags_,
        builder_->makeUintConstant(kSysFlag_AlphaPassIfLess_Shift), builder_->makeUintConstant(3));
    SPIRVBuilder::IfBuilder if_alpha_test_function_is_non_always(
        builder_->createBinOp(
            spv::OpINotEqual, type_bool_, alpha_test_function,
            builder_->makeUintConstant(std::to_underlying(xenos::CompareFunction::kAlways))),
        spv::SelectionControlDontFlattenMask, *builder_);
    {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(3));
      spv::Id alpha_test_alpha = builder_->createLoad(
          builder_->createAccessChain(edram_fragment_shader_interlock_ ? spv::StorageClassFunction
                                                                       : spv::StorageClassOutput,
                                      output_or_var_fragment_data_[0], id_vector_temp_),
          spv::NoPrecision);
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantAlphaTestReference));
      spv::Id alpha_test_reference = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      SPIRVBuilder::IfBuilder if_alpha_test_function_is_not_equal(
          builder_->createBinOp(
              spv::OpIEqual, type_bool_, alpha_test_function,
              builder_->makeUintConstant(std::to_underlying(xenos::CompareFunction::kNotEqual))),
          spv::SelectionControlDontFlattenMask, *builder_, 1, 2);
      spv::Id alpha_test_result_not_equal;
      {
        alpha_test_result_not_equal = builder_->createBinOp(spv::OpFUnordNotEqual, type_bool_,
                                                            alpha_test_alpha, alpha_test_reference);
      }
      if_alpha_test_function_is_not_equal.makeBeginElse();
      spv::Id alpha_test_result_non_not_equal;
      {
        static const spv::Op kAlphaTestOps[] = {spv::OpFOrdLessThan, spv::OpFOrdEqual,
                                                spv::OpFOrdGreaterThan};
        for (u32 i = 0; i < 3; ++i) {
          spv::Id alpha_test_comparison_result = builder_->createBinOp(
              spv::OpLogicalAnd, type_bool_,
              builder_->createBinOp(kAlphaTestOps[i], type_bool_, alpha_test_alpha,
                                    alpha_test_reference),
              builder_->createBinOp(
                  spv::OpINotEqual, type_bool_,
                  builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, alpha_test_function,
                                        builder_->makeUintConstant(UINT32_C(1) << i)),
                  const_uint_0_));
          if (i) {
            alpha_test_result_non_not_equal =
                builder_->createBinOp(spv::OpLogicalOr, type_bool_, alpha_test_result_non_not_equal,
                                      alpha_test_comparison_result);
          } else {
            alpha_test_result_non_not_equal = alpha_test_comparison_result;
          }
        }
      }
      if_alpha_test_function_is_not_equal.makeEndIf();
      spv::Id alpha_test_result = if_alpha_test_function_is_not_equal.createMergePhi(
          alpha_test_result_not_equal, alpha_test_result_non_not_equal);
      if (edram_fragment_shader_interlock_ && !features_.demote_to_helper_invocation) {
        fsi_pixel_potentially_killed = true;
        fsi_sample_mask_in_rt_0_alpha_tests =
            builder_->createTriOp(spv::OpSelect, type_uint_, alpha_test_result,
                                  fsi_sample_mask_in_rt_0_alpha_tests, const_uint_0_);
      } else {
        SPIRVBuilder::IfBuilder alpha_test_kill_if(
            builder_->createUnaryOp(spv::OpLogicalNot, type_bool_, alpha_test_result),
            spv::SelectionControlDontFlattenMask, *builder_);
        bool branch_to_alpha_test_kill_merge = true;
        if (edram_fragment_shader_interlock_) {
          REX_ASSERT(features_.demote_to_helper_invocation);
          fsi_pixel_potentially_killed = true;
          builder_->addExtension("SPV_EXT_demote_to_helper_invocation");
          builder_->addCapability(spv::CapabilityDemoteToHelperInvocationEXT);
          builder_->createNoResultOp(spv::OpDemoteToHelperInvocationEXT);
        } else {
          builder_->createNoResultOp(spv::OpKill);
          branch_to_alpha_test_kill_merge = false;
        }
        alpha_test_kill_if.makeEndIf(branch_to_alpha_test_kill_merge);
      }
    }
    if_alpha_test_function_is_non_always.makeEndIf();

    if (edram_fragment_shader_interlock_ || output_fragment_sample_mask_ != spv::NoResult) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantAlphaToMask));
      spv::Id alpha_to_mask = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      SPIRVBuilder::IfBuilder if_alpha_to_coverage_enabled(
          builder_->createBinOp(spv::OpINotEqual, type_bool_, alpha_to_mask, const_uint_0_),
          spv::SelectionControlDontFlattenMask, *builder_);
      {
        if (edram_fragment_shader_interlock_) {
          fsi_pixel_potentially_killed = true;
        }
        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(3));
        spv::Id alpha_to_coverage_alpha = builder_->createLoad(
            builder_->createAccessChain(edram_fragment_shader_interlock_ ? spv::StorageClassFunction
                                                                         : spv::StorageClassOutput,
                                        output_or_var_fragment_data_[0], id_vector_temp_),
            spv::NoPrecision);
        REX_ASSERT(input_fragment_coordinates_ != spv::NoResult);
        spv::Id const_uint_1 = builder_->makeUintConstant(1);
        spv::Id const_uint_2 = builder_->makeUintConstant(2);
        id_vector_temp_.clear();
        id_vector_temp_.push_back(const_int_0_);
        spv::Id alpha_to_coverage_position_x = builder_->createUnaryOp(
            spv::OpConvertFToU, type_uint_,
            builder_->createLoad(
                builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                            id_vector_temp_),
                spv::NoPrecision));
        id_vector_temp_.back() = builder_->makeIntConstant(1);
        spv::Id alpha_to_coverage_position_y = builder_->createUnaryOp(
            spv::OpConvertFToU, type_uint_,
            builder_->createLoad(
                builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                            id_vector_temp_),
                spv::NoPrecision));
        spv::Id alpha_to_coverage_offset = builder_->createTriOp(
            spv::OpBitFieldUExtract, type_uint_, alpha_to_mask,
            builder_->createBinOp(
                spv::OpShiftLeftLogical, type_uint_,
                builder_->createBinOp(
                    spv::OpBitwiseOr, type_uint_,
                    builder_->createBinOp(spv::OpBitwiseAnd, type_uint_,
                                          alpha_to_coverage_position_x, const_uint_1),
                    builder_->createBinOp(
                        spv::OpShiftLeftLogical, type_uint_,
                        builder_->createBinOp(spv::OpBitwiseAnd, type_uint_,
                                              alpha_to_coverage_position_y, const_uint_1),
                        const_uint_1)),
                const_uint_1),
            const_uint_2);
        spv::Id alpha_to_coverage_offset_float =
            builder_->createUnaryOp(spv::OpConvertUToF, type_float_, alpha_to_coverage_offset);
        if (msaa_samples == spv::NoResult) {
          msaa_samples = LoadMSAASamplesFromFlags();
        }
        auto alpha_to_coverage_apply_sample = [&](spv::Id sample_mask, u32 guest_sample_index,
                                                  u32 host_sample_index, f32 threshold_base,
                                                  f32 threshold_offset_scale) -> spv::Id {
          spv::Id sample_threshold = builder_->createBinOp(
              spv::OpFSub, type_float_, builder_->makeFloatConstant(threshold_base),
              builder_->createBinOp(spv::OpFMul, type_float_, alpha_to_coverage_offset_float,
                                    builder_->makeFloatConstant(threshold_offset_scale)));
          spv::Id sample_covered = builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool_,
                                                         alpha_to_coverage_alpha, sample_threshold);
          if (edram_fragment_shader_interlock_) {
            return builder_->createTriOp(
                spv::OpSelect, type_uint_, sample_covered, sample_mask,
                builder_->createBinOp(
                    spv::OpBitwiseAnd, type_uint_, sample_mask,
                    builder_->makeUintConstant(~((UINT32_C(1) << guest_sample_index) |
                                                 (UINT32_C(1) << (guest_sample_index + 4))))));
          }
          return builder_->createTriOp(
              spv::OpSelect, type_uint_, sample_covered,
              builder_->createBinOp(spv::OpBitwiseOr, type_uint_, sample_mask,
                                    builder_->makeUintConstant(UINT32_C(1) << host_sample_index)),
              sample_mask);
        };
        spv::Id alpha_to_coverage_mask =
            edram_fragment_shader_interlock_ ? fsi_sample_mask_in_rt_0_alpha_tests : const_uint_0_;
        spv::Block& block_alpha_to_coverage_msaa_head = *builder_->getBuildPoint();
        spv::Block& block_alpha_to_coverage_msaa_1x = builder_->makeNewBlock();
        spv::Block& block_alpha_to_coverage_msaa_2x = builder_->makeNewBlock();
        spv::Block& block_alpha_to_coverage_msaa_4x = builder_->makeNewBlock();
        spv::Block& block_alpha_to_coverage_msaa_merge = builder_->makeNewBlock();
        builder_->createSelectionMerge(&block_alpha_to_coverage_msaa_merge,
                                       spv::SelectionControlDontFlattenMask);
        {
          std::unique_ptr<spv::Instruction> alpha_to_coverage_msaa_switch_op =
              std::make_unique<spv::Instruction>(spv::OpSwitch);
          alpha_to_coverage_msaa_switch_op->addIdOperand(msaa_samples);
          alpha_to_coverage_msaa_switch_op->addIdOperand(block_alpha_to_coverage_msaa_1x.getId());
          alpha_to_coverage_msaa_switch_op->addImmediateOperand(
              static_cast<i32>(xenos::MSAASamples::k2X));
          alpha_to_coverage_msaa_switch_op->addIdOperand(block_alpha_to_coverage_msaa_2x.getId());
          alpha_to_coverage_msaa_switch_op->addImmediateOperand(
              static_cast<i32>(xenos::MSAASamples::k4X));
          alpha_to_coverage_msaa_switch_op->addIdOperand(block_alpha_to_coverage_msaa_4x.getId());
          builder_->getBuildPoint()->addInstruction(std::move(alpha_to_coverage_msaa_switch_op));
        }
        block_alpha_to_coverage_msaa_1x.addPredecessor(&block_alpha_to_coverage_msaa_head);
        block_alpha_to_coverage_msaa_2x.addPredecessor(&block_alpha_to_coverage_msaa_head);
        block_alpha_to_coverage_msaa_4x.addPredecessor(&block_alpha_to_coverage_msaa_head);
        builder_->setBuildPoint(&block_alpha_to_coverage_msaa_1x);
        spv::Id alpha_to_coverage_mask_1x =
            alpha_to_coverage_apply_sample(alpha_to_coverage_mask, 0, 0, 1.0f, 1.0f / 4.0f);
        builder_->createBranch(&block_alpha_to_coverage_msaa_merge);
        builder_->setBuildPoint(&block_alpha_to_coverage_msaa_2x);
        u32 alpha_to_coverage_host_sample_0 = 0;
        u32 alpha_to_coverage_host_sample_1 = 1;
        if (!edram_fragment_shader_interlock_) {
          if (native_2x_msaa_with_attachments_) {
            alpha_to_coverage_host_sample_0 = 1;
            alpha_to_coverage_host_sample_1 = 0;
          } else {
            alpha_to_coverage_host_sample_0 = 0;
            alpha_to_coverage_host_sample_1 = 3;
          }
        }
        spv::Id alpha_to_coverage_mask_2x = alpha_to_coverage_apply_sample(
            alpha_to_coverage_mask, 0, alpha_to_coverage_host_sample_0, 0.5f, 1.0f / 8.0f);
        alpha_to_coverage_mask_2x = alpha_to_coverage_apply_sample(
            alpha_to_coverage_mask_2x, 1, alpha_to_coverage_host_sample_1, 1.0f, 1.0f / 8.0f);
        builder_->createBranch(&block_alpha_to_coverage_msaa_merge);
        builder_->setBuildPoint(&block_alpha_to_coverage_msaa_4x);
        u32 alpha_to_coverage_host_sample_4x_0 = 0;
        u32 alpha_to_coverage_host_sample_4x_1 = 1;
        u32 alpha_to_coverage_host_sample_4x_2 = 2;
        u32 alpha_to_coverage_host_sample_4x_3 = 3;
        if (!edram_fragment_shader_interlock_) {
          alpha_to_coverage_host_sample_4x_0 = 0;
          alpha_to_coverage_host_sample_4x_1 = 2;
          alpha_to_coverage_host_sample_4x_2 = 1;
          alpha_to_coverage_host_sample_4x_3 = 3;
        }
        spv::Id alpha_to_coverage_mask_4x = alpha_to_coverage_apply_sample(
            alpha_to_coverage_mask, 0, alpha_to_coverage_host_sample_4x_0, 0.75f, 1.0f / 16.0f);
        alpha_to_coverage_mask_4x = alpha_to_coverage_apply_sample(
            alpha_to_coverage_mask_4x, 1, alpha_to_coverage_host_sample_4x_1, 0.25f, 1.0f / 16.0f);
        alpha_to_coverage_mask_4x = alpha_to_coverage_apply_sample(
            alpha_to_coverage_mask_4x, 2, alpha_to_coverage_host_sample_4x_2, 0.5f, 1.0f / 16.0f);
        alpha_to_coverage_mask_4x = alpha_to_coverage_apply_sample(
            alpha_to_coverage_mask_4x, 3, alpha_to_coverage_host_sample_4x_3, 1.0f, 1.0f / 16.0f);
        builder_->createBranch(&block_alpha_to_coverage_msaa_merge);
        builder_->setBuildPoint(&block_alpha_to_coverage_msaa_merge);
        id_vector_temp_.clear();
        id_vector_temp_.reserve(size_t{2} * 3);
        id_vector_temp_.push_back(alpha_to_coverage_mask_1x);
        id_vector_temp_.push_back(block_alpha_to_coverage_msaa_1x.getId());
        id_vector_temp_.push_back(alpha_to_coverage_mask_2x);
        id_vector_temp_.push_back(block_alpha_to_coverage_msaa_2x.getId());
        id_vector_temp_.push_back(alpha_to_coverage_mask_4x);
        id_vector_temp_.push_back(block_alpha_to_coverage_msaa_4x.getId());
        alpha_to_coverage_mask = builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
        if (edram_fragment_shader_interlock_) {
          fsi_sample_mask_in_rt_0_alpha_tests = alpha_to_coverage_mask;
        } else {
          SPIRVBuilder::IfBuilder alpha_to_coverage_kill_if(
              builder_->createBinOp(spv::OpIEqual, type_bool_, alpha_to_coverage_mask,
                                    const_uint_0_),
              spv::SelectionControlDontFlattenMask, *builder_);
          builder_->createNoResultOp(spv::OpKill);
          alpha_to_coverage_kill_if.makeEndIf(false);
          id_vector_temp_.clear();
          id_vector_temp_.push_back(const_int_0_);
          builder_->createStore(
              builder_->createUnaryOp(spv::OpBitcast, type_int_, alpha_to_coverage_mask),
              builder_->createAccessChain(spv::StorageClassOutput, output_fragment_sample_mask_,
                                          id_vector_temp_));
        }
      }
      if_alpha_to_coverage_enabled.makeEndIf();
    }

    if (edram_fragment_shader_interlock_) {
      builder_->createBranch(block_fsi_rt_0_alpha_tests_rt_written_merge);
      spv::Block& block_fsi_rt_0_alpha_tests_rt_written_end = *builder_->getBuildPoint();
      builder_->setBuildPoint(block_fsi_rt_0_alpha_tests_rt_written_merge);
      if (!features_.demote_to_helper_invocation) {
        id_vector_temp_.clear();
        id_vector_temp_.push_back(fsi_sample_mask_in_rt_0_alpha_tests);
        id_vector_temp_.push_back(block_fsi_rt_0_alpha_tests_rt_written_end.getId());
        id_vector_temp_.push_back(main_fsi_sample_mask_);
        id_vector_temp_.push_back(block_fsi_rt_0_alpha_tests_rt_written_head->getId());
        main_fsi_sample_mask_ = builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
      }
    }
  }

  spv::Block* block_fsi_if_after_kill = nullptr;
  spv::Block* block_fsi_if_after_kill_merge = nullptr;

  spv::Block* block_fsi_if_after_depth_stencil = nullptr;
  spv::Block* block_fsi_if_after_depth_stencil_merge = nullptr;

  if (edram_fragment_shader_interlock_) {
    if (fsi_pixel_potentially_killed) {
      if (features_.demote_to_helper_invocation) {
        id_vector_temp_.clear();
        main_fsi_sample_mask_ = builder_->createTriOp(
            spv::OpSelect, type_uint_,
            builder_->createOp(spv::OpIsHelperInvocationEXT, type_bool_, id_vector_temp_),
            const_uint_0_, main_fsi_sample_mask_);
      }
      // Compute the condition before OpSelectionMerge, which must be the penultimate instruction.
      spv::Id pixel_not_killed =
          builder_->createBinOp(spv::OpINotEqual, type_bool_, main_fsi_sample_mask_, const_uint_0_);
      block_fsi_if_after_kill = &builder_->makeNewBlock();
      block_fsi_if_after_kill_merge = &builder_->makeNewBlock();
      builder_->createSelectionMerge(block_fsi_if_after_kill_merge,
                                     spv::SelectionControlDontFlattenMask);
      builder_->createConditionalBranch(pixel_not_killed, block_fsi_if_after_kill,
                                        block_fsi_if_after_kill_merge);
      builder_->setBuildPoint(block_fsi_if_after_kill);
    }

    spv::Id color_write_depth_stencil_condition = spv::NoResult;
    if (FSI_IsDepthStencilEarly()) {
      for (u32 i = 0; i < 4; ++i) {
        spv::Id sample_late_depth_stencil_write_needed = builder_->createBinOp(
            spv::OpINotEqual, type_bool_,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_fsi_sample_mask_,
                                  builder_->makeUintConstant(static_cast<u32>(1) << (4 + i))),
            const_uint_0_);
        SPIRVBuilder::IfBuilder if_sample_late_depth_stencil_write_needed(
            sample_late_depth_stencil_write_needed, spv::SelectionControlDontFlattenMask,
            *builder_);
        spv::Id depth_stencil_sample_address = FSI_AddSampleOffset(main_fsi_address_depth_, i);
        id_vector_temp_.clear();
        id_vector_temp_.push_back(const_int_0_);
        id_vector_temp_.push_back(depth_stencil_sample_address);
        builder_->createStore(main_fsi_late_write_depth_stencil_[i],
                              builder_->createAccessChain(features_.spirv_version >= spv::Spv_1_3
                                                              ? spv::StorageClassStorageBuffer
                                                              : spv::StorageClassUniform,
                                                          buffer_edram_, id_vector_temp_));
        if_sample_late_depth_stencil_write_needed.makeEndIf();
      }
      if (color_targets_written) {
        color_write_depth_stencil_condition = builder_->createBinOp(
            spv::OpINotEqual, type_bool_,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_fsi_sample_mask_,
                                  builder_->makeUintConstant((static_cast<u32>(1) << 4) - 1)),
            const_uint_0_);
      }
    } else {
      if (msaa_samples == spv::NoResult) {
        msaa_samples = LoadMSAASamplesFromFlags();
      }
      FSI_LoadEDRAMOffsets(msaa_samples);
      builder_->createNoResultOp(spv::OpBeginInvocationInterlockEXT);
      FSI_DepthStencilTest(msaa_samples,
                           fsi_pixel_potentially_killed || (color_targets_written & 0b1));
      if (color_targets_written) {
        color_write_depth_stencil_condition = builder_->createBinOp(
            spv::OpINotEqual, type_bool_, main_fsi_sample_mask_, const_uint_0_);
      }
    }

    if (color_write_depth_stencil_condition != spv::NoResult) {
      block_fsi_if_after_depth_stencil = &builder_->makeNewBlock();
      block_fsi_if_after_depth_stencil_merge = &builder_->makeNewBlock();
      builder_->createSelectionMerge(block_fsi_if_after_depth_stencil_merge,
                                     spv::SelectionControlDontFlattenMask);
      builder_->createConditionalBranch(color_write_depth_stencil_condition,
                                        block_fsi_if_after_depth_stencil,
                                        block_fsi_if_after_depth_stencil_merge);
      builder_->setBuildPoint(block_fsi_if_after_depth_stencil);
    }
  }

  if (color_targets_written) {
    spv::Id fsi_color_targets_written = spv::NoResult;
    spv::Id fsi_const_int_1 = spv::NoResult;
    spv::Id fsi_const_edram_size_dwords = spv::NoResult;
    spv::Id fsi_samples_covered[4] = {};
    if (edram_fragment_shader_interlock_) {
      fsi_color_targets_written =
          builder_->createLoad(var_main_fsi_color_written_, spv::NoPrecision);
      fsi_const_int_1 = builder_->makeIntConstant(1);
      u32 tile_width = xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x_;
      u32 tile_height = xenos::kEDRAMTileHeightSamples * draw_resolution_scale_y_;
      fsi_const_edram_size_dwords =
          builder_->makeUintConstant(tile_width * tile_height * xenos::kEDRAMTileCount);
      for (u32 i = 0; i < 4; ++i) {
        fsi_samples_covered[i] = builder_->createBinOp(
            spv::OpINotEqual, type_bool_,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_fsi_sample_mask_,
                                  builder_->makeUintConstant(static_cast<u32>(1) << i)),
            const_uint_0_);
      }
    }
    u32 color_targets_remaining = color_targets_written;
    u32 color_target_index;
    while (rex::bit_scan_forward(color_targets_remaining, &color_target_index)) {
      color_targets_remaining &= ~(UINT32_C(1) << color_target_index);
      spv::Id color_variable = output_or_var_fragment_data_[color_target_index];
      spv::Id color = builder_->createLoad(color_variable, spv::NoPrecision);

      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantColorExpBias));
      id_vector_temp_.push_back(builder_->makeIntConstant(static_cast<i32>(color_target_index)));
      color = builder_->createNoContractionBinOp(
          spv::OpVectorTimesScalar, type_float4_, color,
          builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision));

      if (edram_fragment_shader_interlock_) {
        spv::Id fsi_color_written = builder_->createBinOp(
            spv::OpINotEqual, type_bool_,
            builder_->createBinOp(
                spv::OpBitwiseAnd, type_uint_, fsi_color_targets_written,
                builder_->makeUintConstant(static_cast<u32>(1) << color_target_index)),
            const_uint_0_);
        SPIRVBuilder::IfBuilder if_fsi_color_written(
            fsi_color_written, spv::SelectionControlDontFlattenMask, *builder_, 2, 1);

        // Per render target uint2 arrays are passed as uint4 arrays for std140 element alignment.
        spv::Id rt_uint2_index_array =
            builder_->makeIntConstant(static_cast<int>(color_target_index >> 1));
        spv::Id rt_uint2_index_element[] = {
            builder_->makeIntConstant(static_cast<int>((color_target_index & 1) << 1)),
            builder_->makeIntConstant(static_cast<int>(((color_target_index & 1) << 1) + 1)),
        };

        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMRTKeepMask));
        id_vector_temp_.push_back(rt_uint2_index_array);
        id_vector_temp_.push_back(rt_uint2_index_element[0]);
        spv::Id rt_keep_mask[2];
        rt_keep_mask[0] = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);
        id_vector_temp_.back() = rt_uint2_index_element[1];
        rt_keep_mask[1] = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);

        spv::Id const_uint32_max = builder_->makeUintConstant(UINT32_MAX);
        spv::Id rt_write_mask_not_empty = builder_->createBinOp(
            spv::OpLogicalOr, type_bool_,
            builder_->createBinOp(spv::OpINotEqual, type_bool_, rt_keep_mask[0], const_uint32_max),
            builder_->createBinOp(spv::OpINotEqual, type_bool_, rt_keep_mask[1], const_uint32_max));
        SPIRVBuilder::IfBuilder if_rt_write_mask_not_empty(
            rt_write_mask_not_empty, spv::SelectionControlDontFlattenMask, *builder_);

        spv::Id const_int_rt_index =
            builder_->makeIntConstant(static_cast<int>(color_target_index));

        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMRTFormatFlags));
        id_vector_temp_.push_back(const_int_rt_index);
        spv::Id rt_format_with_flags = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);

        spv::Id rt_is_64bpp = builder_->createBinOp(
            spv::OpINotEqual, type_bool_,
            builder_->createBinOp(
                spv::OpBitwiseAnd, type_uint_, rt_format_with_flags,
                builder_->makeUintConstant(RenderTargetCache::kPSIColorFormatFlag_64bpp)),
            const_uint_0_);

        id_vector_temp_.clear();
        id_vector_temp_.push_back(
            builder_->makeIntConstant(kSystemConstantEDRAMRTBaseDwordsScaled));
        id_vector_temp_.push_back(const_int_rt_index);
        // EDRAM addresses wrap modulo the EDRAM size on the Xenos.
        spv::Id rt_sample_0_address = builder_->createUnaryOp(
            spv::OpBitcast, type_int_,
            builder_->createBinOp(
                spv::OpUMod, type_uint_,
                builder_->createBinOp(
                    spv::OpIAdd, type_uint_,
                    builder_->createLoad(
                        builder_->createAccessChain(spv::StorageClassUniform,
                                                    uniform_system_constants_, id_vector_temp_),
                        spv::NoPrecision),
                    builder_->createTriOp(spv::OpSelect, type_uint_, rt_is_64bpp,
                                          main_fsi_offset_64bpp_, main_fsi_offset_32bpp_)),
                fsi_const_edram_size_dwords));

        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMRTBlendFactorsOps));
        id_vector_temp_.push_back(const_int_rt_index);
        spv::Id rt_blend_factors_equations = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);

        spv::Id rt_blend_enabled =
            builder_->createBinOp(spv::OpINotEqual, type_bool_, rt_blend_factors_equations,
                                  builder_->makeUintConstant(0x00010001));
        SPIRVBuilder::IfBuilder if_rt_blend_enabled(
            rt_blend_enabled, spv::SelectionControlDontFlattenMask, *builder_);
        {
          spv::Id rt_color_is_fixed_point = builder_->createBinOp(
              spv::OpINotEqual, type_bool_,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, rt_format_with_flags,
                                    builder_->makeUintConstant(
                                        RenderTargetCache::kPSIColorFormatFlag_FixedPointColor)),
              const_uint_0_);
          spv::Id rt_alpha_is_fixed_point = builder_->createBinOp(
              spv::OpINotEqual, type_bool_,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, rt_format_with_flags,
                                    builder_->makeUintConstant(
                                        RenderTargetCache::kPSIColorFormatFlag_FixedPointAlpha)),
              const_uint_0_);
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMRTClamp));
          id_vector_temp_.push_back(const_int_rt_index);
          spv::Id rt_clamp = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision);
          spv::Id rt_clamp_color_min = builder_->smearScalar(
              spv::NoPrecision, builder_->createCompositeExtract(rt_clamp, type_float_, 0),
              type_float3_);
          spv::Id rt_clamp_alpha_min = builder_->createCompositeExtract(rt_clamp, type_float_, 1);
          spv::Id rt_clamp_color_max = builder_->smearScalar(
              spv::NoPrecision, builder_->createCompositeExtract(rt_clamp, type_float_, 2),
              type_float3_);
          spv::Id rt_clamp_alpha_max = builder_->createCompositeExtract(rt_clamp, type_float_, 3);

          spv::Id blend_factor_width = builder_->makeUintConstant(5);
          spv::Id blend_equation_width = builder_->makeUintConstant(3);
          spv::Id rt_color_source_factor =
              builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, rt_blend_factors_equations,
                                    const_uint_0_, blend_factor_width);
          spv::Id rt_color_equation =
              builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, rt_blend_factors_equations,
                                    blend_factor_width, blend_equation_width);
          spv::Id rt_color_dest_factor =
              builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, rt_blend_factors_equations,
                                    builder_->makeUintConstant(8), blend_factor_width);
          spv::Id rt_alpha_source_factor =
              builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, rt_blend_factors_equations,
                                    builder_->makeUintConstant(16), blend_factor_width);
          spv::Id rt_alpha_equation =
              builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, rt_blend_factors_equations,
                                    builder_->makeUintConstant(21), blend_equation_width);
          spv::Id rt_alpha_dest_factor =
              builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, rt_blend_factors_equations,
                                    builder_->makeUintConstant(24), blend_factor_width);

          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMBlendConstant));
          spv::Id blend_constant_unclamped = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision);
          uint_vector_temp_.clear();
          uint_vector_temp_.push_back(0);
          uint_vector_temp_.push_back(1);
          uint_vector_temp_.push_back(2);
          spv::Id blend_constant_color_unclamped = builder_->createRvalueSwizzle(
              spv::NoPrecision, type_float3_, blend_constant_unclamped, uint_vector_temp_);
          spv::Id blend_constant_color_clamped = FSI_FlushNaNClampAndInBlending(
              blend_constant_color_unclamped, rt_color_is_fixed_point, rt_clamp_color_min,
              rt_clamp_color_max);
          spv::Id blend_constant_alpha_clamped = FSI_FlushNaNClampAndInBlending(
              builder_->createCompositeExtract(blend_constant_unclamped, type_float_, 3),
              rt_alpha_is_fixed_point, rt_clamp_alpha_min, rt_clamp_alpha_max);

          uint_vector_temp_.clear();
          uint_vector_temp_.push_back(0);
          uint_vector_temp_.push_back(1);
          uint_vector_temp_.push_back(2);
          spv::Id source_color_unclamped = builder_->createRvalueSwizzle(
              spv::NoPrecision, type_float3_, color, uint_vector_temp_);
          spv::Id source_color_clamped =
              FSI_FlushNaNClampAndInBlending(source_color_unclamped, rt_color_is_fixed_point,
                                             rt_clamp_color_min, rt_clamp_color_max);
          spv::Id source_alpha_clamped = FSI_FlushNaNClampAndInBlending(
              builder_->createCompositeExtract(color, type_float_, 3), rt_alpha_is_fixed_point,
              rt_clamp_alpha_min, rt_clamp_alpha_max);

          std::array<spv::Id, 2> rt_replace_mask;
          for (u32 i = 0; i < 2; ++i) {
            rt_replace_mask[i] = builder_->createUnaryOp(spv::OpNot, type_uint_, rt_keep_mask[i]);
          }

          for (u32 i = 0; i < 4; ++i) {
            SPIRVBuilder::IfBuilder if_sample_covered(
                fsi_samples_covered[i], spv::SelectionControlDontFlattenMask, *builder_);

            spv::Id rt_sample_address = FSI_AddSampleOffset(rt_sample_0_address, i, rt_is_64bpp);
            id_vector_temp_.clear();
            id_vector_temp_.push_back(const_int_0_);
            id_vector_temp_.push_back(rt_sample_address);
            spv::Id rt_access_chain_0 = builder_->createAccessChain(
                features_.spirv_version >= spv::Spv_1_3 ? spv::StorageClassStorageBuffer
                                                        : spv::StorageClassUniform,
                buffer_edram_, id_vector_temp_);
            id_vector_temp_.back() =
                builder_->createBinOp(spv::OpIAdd, type_int_, rt_sample_address, fsi_const_int_1);
            spv::Id rt_access_chain_1 = builder_->createAccessChain(
                features_.spirv_version >= spv::Spv_1_3 ? spv::StorageClassStorageBuffer
                                                        : spv::StorageClassUniform,
                buffer_edram_, id_vector_temp_);

            std::array<spv::Id, 2> dest_packed;
            dest_packed[0] = builder_->createLoad(rt_access_chain_0, spv::NoPrecision);
            {
              SPIRVBuilder::IfBuilder if_64bpp(rt_is_64bpp, spv::SelectionControlDontFlattenMask,
                                               *builder_);
              spv::Id dest_packed_64bpp_high =
                  builder_->createLoad(rt_access_chain_1, spv::NoPrecision);
              if_64bpp.makeEndIf();
              dest_packed[1] = if_64bpp.createMergePhi(dest_packed_64bpp_high, const_uint_0_);
            }
            std::array<spv::Id, 4> dest_unpacked =
                FSI_UnpackColor(dest_packed, rt_format_with_flags);
            id_vector_temp_.clear();
            id_vector_temp_.push_back(dest_unpacked[0]);
            id_vector_temp_.push_back(dest_unpacked[1]);
            id_vector_temp_.push_back(dest_unpacked[2]);
            spv::Id dest_color = builder_->createCompositeConstruct(type_float3_, id_vector_temp_);

            spv::Id result_color = FSI_BlendColorOrAlphaWithUnclampedResult(
                rt_color_is_fixed_point, rt_clamp_color_min, rt_clamp_color_max,
                source_color_clamped, source_alpha_clamped, dest_color, dest_unpacked[3],
                blend_constant_color_clamped, blend_constant_alpha_clamped, rt_color_equation,
                rt_color_source_factor, rt_color_dest_factor);
            spv::Id result_alpha = FSI_BlendColorOrAlphaWithUnclampedResult(
                rt_alpha_is_fixed_point, rt_clamp_alpha_min, rt_clamp_alpha_max, spv::NoResult,
                source_alpha_clamped, spv::NoResult, dest_unpacked[3], spv::NoResult,
                blend_constant_alpha_clamped, rt_alpha_equation, rt_alpha_source_factor,
                rt_alpha_dest_factor);

            // Vectors can be concatenated by createCompositeConstruct despite its assertion.
            spv::Id result_float4;
            {
              std::unique_ptr<spv::Instruction> result_composite_construct_op =
                  std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float4_,
                                                     spv::OpCompositeConstruct);
              result_composite_construct_op->addIdOperand(result_color);
              result_composite_construct_op->addIdOperand(result_alpha);
              result_float4 = result_composite_construct_op->getResultId();
              builder_->getBuildPoint()->addInstruction(std::move(result_composite_construct_op));
            }
            std::array<spv::Id, 2> result_packed =
                FSI_ClampAndPackColor(result_float4, rt_format_with_flags);
            builder_->createStore(
                builder_->createBinOp(spv::OpBitwiseOr, type_uint_,
                                      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_,
                                                            dest_packed[0], rt_keep_mask[0]),
                                      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_,
                                                            result_packed[0], rt_replace_mask[0])),
                rt_access_chain_0);
            SPIRVBuilder::IfBuilder if_64bpp(rt_is_64bpp, spv::SelectionControlDontFlattenMask,
                                             *builder_);
            {
              builder_->createStore(
                  builder_->createBinOp(
                      spv::OpBitwiseOr, type_uint_,
                      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, dest_packed[1],
                                            rt_keep_mask[1]),
                      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, result_packed[1],
                                            rt_replace_mask[1])),
                  rt_access_chain_1);
            }
            if_64bpp.makeEndIf();

            if_sample_covered.makeEndIf();
          }
        }
        if_rt_blend_enabled.makeBeginElse();
        {
          std::array<spv::Id, 2> color_packed = FSI_ClampAndPackColor(color, rt_format_with_flags);

          spv::Id rt_keep_mask_not_empty = builder_->createBinOp(
              spv::OpLogicalOr, type_bool_,
              builder_->createBinOp(spv::OpINotEqual, type_bool_, rt_keep_mask[0], const_uint_0_),
              builder_->createBinOp(spv::OpINotEqual, type_bool_, rt_keep_mask[1], const_uint_0_));

          SPIRVBuilder::IfBuilder if_rt_keep_mask_not_empty(
              rt_keep_mask_not_empty, spv::SelectionControlDontFlattenMask, *builder_);
          {
            std::array<spv::Id, 2> color_packed_masked;
            for (u32 i = 0; i < 2; ++i) {
              color_packed_masked[i] = builder_->createBinOp(
                  spv::OpBitwiseAnd, type_uint_, color_packed[i],
                  builder_->createUnaryOp(spv::OpNot, type_uint_, rt_keep_mask[i]));
            }
            for (u32 i = 0; i < 4; ++i) {
              SPIRVBuilder::IfBuilder if_sample_covered(
                  fsi_samples_covered[i], spv::SelectionControlDontFlattenMask, *builder_);
              spv::Id rt_sample_address = FSI_AddSampleOffset(rt_sample_0_address, i, rt_is_64bpp);
              id_vector_temp_.clear();
              id_vector_temp_.push_back(const_int_0_);
              id_vector_temp_.push_back(rt_sample_address);
              spv::Id rt_access_chain_0 = builder_->createAccessChain(
                  features_.spirv_version >= spv::Spv_1_3 ? spv::StorageClassStorageBuffer
                                                          : spv::StorageClassUniform,
                  buffer_edram_, id_vector_temp_);
              builder_->createStore(
                  builder_->createBinOp(
                      spv::OpBitwiseOr, type_uint_,
                      builder_->createBinOp(
                          spv::OpBitwiseAnd, type_uint_,
                          builder_->createLoad(rt_access_chain_0, spv::NoPrecision),
                          rt_keep_mask[0]),
                      color_packed_masked[0]),
                  rt_access_chain_0);
              SPIRVBuilder::IfBuilder if_64bpp(rt_is_64bpp, spv::SelectionControlDontFlattenMask,
                                               *builder_);
              {
                id_vector_temp_.back() = builder_->createBinOp(spv::OpIAdd, type_int_,
                                                               rt_sample_address, fsi_const_int_1);
                spv::Id rt_access_chain_1 = builder_->createAccessChain(
                    features_.spirv_version >= spv::Spv_1_3 ? spv::StorageClassStorageBuffer
                                                            : spv::StorageClassUniform,
                    buffer_edram_, id_vector_temp_);
                builder_->createStore(
                    builder_->createBinOp(
                        spv::OpBitwiseOr, type_uint_,
                        builder_->createBinOp(
                            spv::OpBitwiseAnd, type_uint_,
                            builder_->createLoad(rt_access_chain_1, spv::NoPrecision),
                            rt_keep_mask[1]),
                        color_packed_masked[1]),
                    rt_access_chain_1);
              }
              if_64bpp.makeEndIf();
              if_sample_covered.makeEndIf();
            }
          }
          if_rt_keep_mask_not_empty.makeBeginElse();
          {
            for (u32 i = 0; i < 4; ++i) {
              SPIRVBuilder::IfBuilder if_sample_covered(
                  fsi_samples_covered[i], spv::SelectionControlDontFlattenMask, *builder_);
              spv::Id rt_sample_address = FSI_AddSampleOffset(rt_sample_0_address, i, rt_is_64bpp);
              id_vector_temp_.clear();
              id_vector_temp_.push_back(const_int_0_);
              id_vector_temp_.push_back(rt_sample_address);
              builder_->createStore(color_packed[0], builder_->createAccessChain(
                                                         features_.spirv_version >= spv::Spv_1_3
                                                             ? spv::StorageClassStorageBuffer
                                                             : spv::StorageClassUniform,
                                                         buffer_edram_, id_vector_temp_));
              SPIRVBuilder::IfBuilder if_64bpp(rt_is_64bpp, spv::SelectionControlDontFlattenMask,
                                               *builder_);
              {
                id_vector_temp_.back() = builder_->createBinOp(
                    spv::OpIAdd, type_int_, id_vector_temp_.back(), fsi_const_int_1);
                builder_->createStore(color_packed[1], builder_->createAccessChain(
                                                           features_.spirv_version >= spv::Spv_1_3
                                                               ? spv::StorageClassStorageBuffer
                                                               : spv::StorageClassUniform,
                                                           buffer_edram_, id_vector_temp_));
              }
              if_64bpp.makeEndIf();
              if_sample_covered.makeEndIf();
            }
          }
          if_rt_keep_mask_not_empty.makeEndIf();
        }
        if_rt_blend_enabled.makeEndIf();

        if_rt_write_mask_not_empty.makeEndIf();
        if_fsi_color_written.makeEndIf();
      } else {
        uint_vector_temp_.clear();
        uint_vector_temp_.push_back(0);
        uint_vector_temp_.push_back(1);
        uint_vector_temp_.push_back(2);
        spv::Id color_rgb =
            builder_->createRvalueSwizzle(spv::NoPrecision, type_float3_, color, uint_vector_temp_);
        spv::Id is_gamma = builder_->createBinOp(
            spv::OpINotEqual, type_bool_,
            builder_->createBinOp(
                spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                builder_->makeUintConstant(kSysFlag_ConvertColor0ToGamma << color_target_index)),
            const_uint_0_);
        SPIRVBuilder::IfBuilder if_gamma(is_gamma, spv::SelectionControlDontFlattenMask, *builder_);
        spv::Id color_rgb_gamma = LinearToPWLGamma(color_rgb, false);
        if_gamma.makeEndIf();
        color_rgb = if_gamma.createMergePhi(color_rgb_gamma, color_rgb);
        {
          std::unique_ptr<spv::Instruction> color_rgba_shuffle_op =
              std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float4_,
                                                 spv::OpVectorShuffle);
          color_rgba_shuffle_op->addIdOperand(color_rgb);
          color_rgba_shuffle_op->addIdOperand(color);
          color_rgba_shuffle_op->addImmediateOperand(0);
          color_rgba_shuffle_op->addImmediateOperand(1);
          color_rgba_shuffle_op->addImmediateOperand(2);
          color_rgba_shuffle_op->addImmediateOperand(3 + 3);
          color = color_rgba_shuffle_op->getResultId();
          builder_->getBuildPoint()->addInstruction(std::move(color_rgba_shuffle_op));
        }

        builder_->createStore(color, color_variable);
      }
    }
  }

  if (!edram_fragment_shader_interlock_ && output_fragment_depth_ != spv::NoResult) {
    Modification::DepthStencilMode depth_stencil_mode =
        GetSPIRVShaderModification().pixel.depth_stencil_mode;
    if (depth_stencil_mode == Modification::DepthStencilMode::kFloat24Truncating ||
        depth_stencil_mode == Modification::DepthStencilMode::kFloat24Rounding) {
      spv::Id depth;
      if (current_shader().writes_depth()) {
        depth = builder_->createLoad(output_fragment_depth_, spv::NoPrecision);
      } else {
        REX_ASSERT(input_fragment_coordinates_ != spv::NoResult);
        id_vector_temp_.clear();
        id_vector_temp_.push_back(builder_->makeIntConstant(2));
        depth = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                        id_vector_temp_),
            spv::NoPrecision);
        if (IsSampleRate()) {
          REX_ASSERT(input_sample_id_ != spv::NoResult);
          builder_->createLoad(input_sample_id_, spv::NoPrecision);
        }
        depth = builder_->createTriBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
            builder_->createNoContractionBinOp(spv::OpFMul, type_float_, depth,
                                               builder_->makeFloatConstant(2.0f)),
            const_float_0_, const_float_1_);
      }
      spv::Id depth_float24 = SPIRVShaderTranslator::PreClampedDepthTo20e4(
          *builder_, depth, depth_stencil_mode == Modification::DepthStencilMode::kFloat24Rounding,
          false, ext_inst_glsl_std_450_);
      depth = SPIRVShaderTranslator::Depth20e4To32(*builder_, depth_float24, 0, true, false,
                                                   ext_inst_glsl_std_450_);
      builder_->createStore(depth, output_fragment_depth_);
    }
  }

  if (edram_fragment_shader_interlock_) {
    if (block_fsi_if_after_depth_stencil_merge) {
      builder_->createBranch(block_fsi_if_after_depth_stencil_merge);
      builder_->setBuildPoint(block_fsi_if_after_depth_stencil_merge);
    }

    if (block_fsi_if_after_kill_merge) {
      builder_->createBranch(block_fsi_if_after_kill_merge);
      builder_->setBuildPoint(block_fsi_if_after_kill_merge);
    }

    if (FSI_IsDepthStencilEarly()) {
      builder_->createBranch(main_fsi_early_depth_stencil_execute_quad_merge_);
      builder_->setBuildPoint(main_fsi_early_depth_stencil_execute_quad_merge_);
    }

    builder_->createNoResultOp(spv::OpEndInvocationInterlockEXT);
  }
}

spv::Id SPIRVShaderTranslator::LoadMSAASamplesFromFlags() {
  return builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, main_system_constant_flags_,
                               builder_->makeUintConstant(kSysFlag_MSAASamples_Shift),
                               builder_->makeUintConstant(2));
}

void SPIRVShaderTranslator::FSI_LoadSampleMask(spv::Id msaa_samples) {
  spv::Id const_uint_1 = builder_->makeUintConstant(1);
  spv::Id const_uint_2 = builder_->makeUintConstant(2);

  REX_ASSERT(input_sample_mask_ != spv::NoResult);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(const_int_0_);
  spv::Id input_sample_mask_value = builder_->createUnaryOp(
      spv::OpBitcast, type_uint_,
      builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassInput, input_sample_mask_, id_vector_temp_),
          spv::NoPrecision));

  spv::Block& block_msaa_head = *builder_->getBuildPoint();
  spv::Block& block_msaa_1x = builder_->makeNewBlock();
  spv::Block& block_msaa_2x = builder_->makeNewBlock();
  spv::Block& block_msaa_4x = builder_->makeNewBlock();
  spv::Block& block_msaa_merge = builder_->makeNewBlock();
  builder_->createSelectionMerge(&block_msaa_merge, spv::SelectionControlDontFlattenMask);
  {
    std::unique_ptr<spv::Instruction> msaa_switch_op =
        std::make_unique<spv::Instruction>(spv::OpSwitch);
    msaa_switch_op->addIdOperand(msaa_samples);
    msaa_switch_op->addIdOperand(block_msaa_1x.getId());
    msaa_switch_op->addImmediateOperand(static_cast<i32>(xenos::MSAASamples::k2X));
    msaa_switch_op->addIdOperand(block_msaa_2x.getId());
    msaa_switch_op->addImmediateOperand(static_cast<i32>(xenos::MSAASamples::k4X));
    msaa_switch_op->addIdOperand(block_msaa_4x.getId());
    builder_->getBuildPoint()->addInstruction(std::move(msaa_switch_op));
  }
  block_msaa_1x.addPredecessor(&block_msaa_head);
  block_msaa_2x.addPredecessor(&block_msaa_head);
  block_msaa_4x.addPredecessor(&block_msaa_head);

  builder_->setBuildPoint(&block_msaa_1x);
  builder_->createBranch(&block_msaa_merge);

  builder_->setBuildPoint(&block_msaa_2x);
  spv::Id sample_mask_2x;
  if (native_2x_msaa_no_attachments_) {
    sample_mask_2x = builder_->createBinOp(
        spv::OpShiftRightLogical, type_uint_,
        builder_->createUnaryOp(spv::OpBitReverse, type_uint_, input_sample_mask_value),
        builder_->makeUintConstant(32 - 2));
  } else {
    sample_mask_2x = builder_->createQuadOp(
        spv::OpBitFieldInsert, type_uint_, input_sample_mask_value,
        builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, input_sample_mask_value,
                              const_uint_2, const_uint_1),
        const_uint_1, builder_->makeUintConstant(32 - 1));
  }
  builder_->createBranch(&block_msaa_merge);

  builder_->setBuildPoint(&block_msaa_4x);
  spv::Id sample_mask_4x = builder_->createQuadOp(
      spv::OpBitFieldInsert, type_uint_, input_sample_mask_value,
      builder_->createBinOp(
          spv::OpShiftRightLogical, type_uint_,
          builder_->createUnaryOp(spv::OpBitReverse, type_uint_, input_sample_mask_value),
          builder_->makeUintConstant(32 - 1 - 2)),
      const_uint_1, const_uint_2);
  builder_->createBranch(&block_msaa_merge);

  builder_->setBuildPoint(&block_msaa_merge);
  id_vector_temp_.clear();
  id_vector_temp_.reserve(size_t{2} * 3);
  id_vector_temp_.push_back(input_sample_mask_value);
  id_vector_temp_.push_back(block_msaa_1x.getId());
  id_vector_temp_.push_back(sample_mask_2x);
  id_vector_temp_.push_back(block_msaa_2x.getId());
  id_vector_temp_.push_back(sample_mask_4x);
  id_vector_temp_.push_back(block_msaa_4x.getId());
  main_fsi_sample_mask_ = builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
}

void SPIRVShaderTranslator::FSI_LoadEDRAMOffsets(spv::Id msaa_samples) {
  REX_ASSERT(input_fragment_coordinates_ != spv::NoResult);
  spv::Id axes_have_two_msaa_samples[2];
  spv::Id sample_coordinates[2];
  spv::Id const_uint_1 = builder_->makeUintConstant(1);
  for (u32 i = 0; i < 2; ++i) {
    spv::Id axis_has_two_msaa_samples = builder_->createBinOp(
        spv::OpUGreaterThanEqual, type_bool_, msaa_samples,
        builder_->makeUintConstant(
            static_cast<u32>(i ? xenos::MSAASamples::k2X : xenos::MSAASamples::k4X)));
    axes_have_two_msaa_samples[i] = axis_has_two_msaa_samples;
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(static_cast<i32>(i)));
    sample_coordinates[i] = builder_->createBinOp(
        spv::OpShiftLeftLogical, type_uint_,
        builder_->createUnaryOp(
            spv::OpConvertFToU, type_uint_,
            builder_->createLoad(
                builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                            id_vector_temp_),
                spv::NoPrecision)),
        builder_->createTriOp(spv::OpSelect, type_uint_, axis_has_two_msaa_samples, const_uint_1,
                              const_uint_0_));
  }

  u32 tile_width = xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x_;
  spv::Id const_tile_half_width = builder_->makeUintConstant(tile_width >> 1);
  u32 tile_height = xenos::kEDRAMTileHeightSamples * draw_resolution_scale_y_;
  spv::Id const_tile_height = builder_->makeUintConstant(tile_height);
  spv::Id tile_half_index[2], tile_half_sample_coordinates[2];
  for (u32 i = 0; i < 2; ++i) {
    spv::Id sample_x_or_y = sample_coordinates[i];
    spv::Id tile_half_width_or_height = i ? const_tile_height : const_tile_half_width;
    tile_half_index[i] =
        builder_->createBinOp(spv::OpUDiv, type_uint_, sample_x_or_y, tile_half_width_or_height);
    tile_half_sample_coordinates[i] =
        builder_->createBinOp(spv::OpUMod, type_uint_, sample_x_or_y, tile_half_width_or_height);
  }

  spv::Id const_tile_width = builder_->makeUintConstant(tile_width);
  spv::Id row_offset_in_tile_at_32bpp = builder_->createBinOp(
      spv::OpIMul, type_uint_, tile_half_sample_coordinates[1], const_tile_width);

  id_vector_temp_.clear();
  id_vector_temp_.push_back(
      builder_->makeIntConstant(kSystemConstantEDRAM32bppTilePitchDwordsScaled));
  spv::Id tile_row_offset_at_32bpp = builder_->createBinOp(
      spv::OpIMul, type_uint_,
      builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants_, id_vector_temp_),
                           spv::NoPrecision),
      tile_half_index[1]);

  u32 tile_size = tile_width * tile_height;
  spv::Id const_tile_size = builder_->makeUintConstant(tile_size);

  spv::Id offset_in_first_tile_half_at_32bpp = builder_->createBinOp(
      spv::OpIAdd, type_uint_,
      builder_->createBinOp(
          spv::OpIAdd, type_uint_, tile_row_offset_at_32bpp,
          builder_->createBinOp(
              spv::OpIAdd, type_uint_,
              builder_->createBinOp(spv::OpIMul, type_uint_, const_tile_size,
                                    builder_->createBinOp(spv::OpShiftRightLogical, type_uint_,
                                                          tile_half_index[0], const_uint_1)),
              row_offset_in_tile_at_32bpp)),
      tile_half_sample_coordinates[0]);

  spv::Id is_second_tile_half = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, tile_half_index[0], const_uint_1),
      const_uint_0_);

  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMDepthBaseDwordsScaled));
  main_fsi_address_depth_ = builder_->createBinOp(
      spv::OpUMod, type_uint_,
      builder_->createBinOp(
          spv::OpIAdd, type_uint_,
          builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision),
          builder_->createBinOp(
              spv::OpIAdd, type_uint_, offset_in_first_tile_half_at_32bpp,
              builder_->createTriOp(spv::OpSelect, type_uint_, is_second_tile_half, const_uint_0_,
                                    const_tile_half_width))),
      builder_->makeUintConstant(tile_size * xenos::kEDRAMTileCount));

  if (current_shader().writes_color_targets()) {
    main_fsi_offset_32bpp_ =
        builder_->createBinOp(spv::OpIAdd, type_uint_, offset_in_first_tile_half_at_32bpp,
                              builder_->createTriOp(spv::OpSelect, type_uint_, is_second_tile_half,
                                                    const_tile_half_width, const_uint_0_));

    main_fsi_offset_64bpp_ = builder_->createBinOp(
        spv::OpIAdd, type_uint_,
        builder_->createBinOp(
            spv::OpIAdd, type_uint_,
            builder_->createBinOp(spv::OpShiftLeftLogical, type_uint_, tile_row_offset_at_32bpp,
                                  const_uint_1),
            builder_->createBinOp(
                spv::OpIAdd, type_uint_,
                builder_->createBinOp(spv::OpIMul, type_uint_, const_tile_size, tile_half_index[0]),
                row_offset_in_tile_at_32bpp)),
        builder_->createBinOp(spv::OpShiftLeftLogical, type_uint_, tile_half_sample_coordinates[0],
                              const_uint_1));
  }
}

spv::Id SPIRVShaderTranslator::FSI_AddSampleOffset(spv::Id sample_0_address, u32 sample_index,
                                                   spv::Id is_64bpp) {
  if (!sample_index) {
    return sample_0_address;
  }
  spv::Id sample_offset;
  u32 tile_width = xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x_;
  if (sample_index == 1) {
    sample_offset = builder_->makeIntConstant(static_cast<int>(tile_width));
  } else {
    spv::Id sample_offset_32bpp = builder_->makeIntConstant(
        static_cast<int>(tile_width * (sample_index & 1) + (sample_index >> 1)));
    if (is_64bpp != spv::NoResult) {
      sample_offset = builder_->createTriOp(
          spv::OpSelect, type_int_, is_64bpp,
          builder_->makeIntConstant(
              static_cast<int>(tile_width * (sample_index & 1) + 2 * (sample_index >> 1))),
          sample_offset_32bpp);
    } else {
      sample_offset = sample_offset_32bpp;
    }
  }
  return builder_->createBinOp(spv::OpIAdd, type_int_, sample_0_address, sample_offset);
}

void SPIRVShaderTranslator::FSI_DepthStencilTest(spv::Id msaa_samples,
                                                 bool sample_mask_potentially_narrowed_previouly) {
  bool is_early = FSI_IsDepthStencilEarly();
  bool shader_writes_depth = current_shader().writes_depth();
  bool implicit_early_z_write_allowed = current_shader().implicit_early_z_write_allowed();
  spv::Id const_uint_1 = builder_->makeUintConstant(1);
  spv::Id const_uint_8 = builder_->makeUintConstant(8);

  spv::Id depth_stencil_enabled = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                            builder_->makeUintConstant(kSysFlag_FSIDepthStencil)),
      const_uint_0_);
  SPIRVBuilder::IfBuilder if_depth_stencil_enabled(depth_stencil_enabled,
                                                   spv::SelectionControlDontFlattenMask, *builder_);

  spv::Id center_depth32_unbiased;
  std::array<spv::Id, 2> depth_dxy = {const_float_0_, const_float_0_};
  if (shader_writes_depth) {
    REX_ASSERT(var_main_fragment_depth_ != spv::NoResult);
    center_depth32_unbiased = builder_->createLoad(var_main_fragment_depth_, spv::NoPrecision);
  } else {
    REX_ASSERT(input_fragment_coordinates_ != spv::NoResult);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(2));
    center_depth32_unbiased = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassInput, input_fragment_coordinates_,
                                    id_vector_temp_),
        spv::NoPrecision);
    builder_->addCapability(spv::CapabilityDerivativeControl);
    depth_dxy[0] = builder_->createUnaryOp(spv::OpDPdxCoarse, type_float_, center_depth32_unbiased);
    depth_dxy[1] = builder_->createUnaryOp(spv::OpDPdyCoarse, type_float_, center_depth32_unbiased);
  }

  spv::Block* block_any_sample_covered_head = nullptr;
  spv::Block* block_any_sample_covered = nullptr;
  spv::Block* block_any_sample_covered_merge = nullptr;
  if (sample_mask_potentially_narrowed_previouly) {
    spv::Id any_sample_covered =
        builder_->createBinOp(spv::OpINotEqual, type_bool_, main_fsi_sample_mask_, const_uint_0_);
    block_any_sample_covered_head = builder_->getBuildPoint();
    block_any_sample_covered = &builder_->makeNewBlock();
    block_any_sample_covered_merge = &builder_->makeNewBlock();
    builder_->createSelectionMerge(block_any_sample_covered_merge,
                                   spv::SelectionControlDontFlattenMask);
    builder_->createConditionalBranch(any_sample_covered, block_any_sample_covered,
                                      block_any_sample_covered_merge);
    builder_->setBuildPoint(block_any_sample_covered);
  }

  spv::Id msaa_is_2x_4x = builder_->createBinOp(
      spv::OpUGreaterThanEqual, type_bool_, msaa_samples,
      builder_->makeUintConstant(std::to_underlying(xenos::MSAASamples::k2X)));
  spv::Id msaa_is_4x = builder_->createBinOp(
      spv::OpUGreaterThanEqual, type_bool_, msaa_samples,
      builder_->makeUintConstant(std::to_underlying(xenos::MSAASamples::k4X)));
  spv::Id depth_is_float24 = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                            builder_->makeUintConstant(kSysFlag_DepthFloat24)),
      const_uint_0_);
  spv::Id depth_pass_if_less = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                            builder_->makeUintConstant(kSysFlag_FSIDepthPassIfLess)),
      const_uint_0_);
  spv::Id depth_pass_if_equal = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                            builder_->makeUintConstant(kSysFlag_FSIDepthPassIfEqual)),
      const_uint_0_);
  spv::Id depth_pass_if_greater = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                            builder_->makeUintConstant(kSysFlag_FSIDepthPassIfGreater)),
      const_uint_0_);
  spv::Id depth_write = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                            builder_->makeUintConstant(kSysFlag_FSIDepthWrite)),
      const_uint_0_);
  spv::Id stencil_enabled = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                            builder_->makeUintConstant(kSysFlag_FSIStencilTest)),
      const_uint_0_);
  spv::Id early_write =
      (is_early && implicit_early_z_write_allowed)
          ? builder_->createBinOp(
                spv::OpINotEqual, type_bool_,
                builder_->createBinOp(
                    spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                    builder_->makeUintConstant(kSysFlag_FSIDepthStencilEarlyWrite)),
                const_uint_0_)
          : spv::NoResult;
  spv::Id not_early_write =
      (is_early && implicit_early_z_write_allowed)
          ? builder_->createUnaryOp(spv::OpLogicalNot, type_bool_, early_write)
          : spv::NoResult;
  REX_ASSERT(input_front_facing_ != spv::NoResult);
  spv::Id front_facing = builder_->createLoad(input_front_facing_, spv::NoPrecision);
  spv::Id poly_offset_scale, poly_offset_offset, stencil_parameters;
  {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMPolyOffsetFrontScale));
    spv::Id poly_offset_front_scale = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMPolyOffsetBackScale));
    spv::Id poly_offset_back_scale = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    poly_offset_scale = builder_->createTriOp(spv::OpSelect, type_float_, front_facing,
                                              poly_offset_front_scale, poly_offset_back_scale);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMPolyOffsetFrontOffset));
    spv::Id poly_offset_front_offset = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMPolyOffsetBackOffset));
    spv::Id poly_offset_back_offset = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    poly_offset_offset = builder_->createTriOp(spv::OpSelect, type_float_, front_facing,
                                               poly_offset_front_offset, poly_offset_back_offset);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMStencilFront));
    spv::Id stencil_parameters_front = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantEDRAMStencilBack));
    spv::Id stencil_parameters_back = builder_->createLoad(
        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                    id_vector_temp_),
        spv::NoPrecision);
    stencil_parameters =
        builder_->createTriOp(spv::OpSelect, type_uint2_,
                              builder_->smearScalar(spv::NoPrecision, front_facing, type_bool2_),
                              stencil_parameters_front, stencil_parameters_back);
  }
  spv::Id stencil_reference_masks =
      builder_->createCompositeExtract(stencil_parameters, type_uint_, 0);
  spv::Id stencil_reference = builder_->createTriOp(
      spv::OpBitFieldUExtract, type_uint_, stencil_reference_masks, const_uint_0_, const_uint_8);
  spv::Id stencil_read_mask = builder_->createTriOp(
      spv::OpBitFieldUExtract, type_uint_, stencil_reference_masks, const_uint_8, const_uint_8);
  spv::Id stencil_reference_read_masked =
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, stencil_reference, stencil_read_mask);
  spv::Id stencil_write_mask =
      builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, stencil_reference_masks,
                            builder_->makeUintConstant(16), const_uint_8);
  spv::Id stencil_write_keep_mask =
      builder_->createUnaryOp(spv::OpNot, type_uint_, stencil_write_mask);
  spv::Id stencil_func_ops = builder_->createCompositeExtract(stencil_parameters, type_uint_, 1);
  spv::Id stencil_pass_if_less = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, stencil_func_ops,
                            builder_->makeUintConstant(static_cast<u32>(1) << 0)),
      const_uint_0_);
  spv::Id stencil_pass_if_equal = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, stencil_func_ops,
                            builder_->makeUintConstant(static_cast<u32>(1) << 1)),
      const_uint_0_);
  spv::Id stencil_pass_if_greater = builder_->createBinOp(
      spv::OpINotEqual, type_bool_,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, stencil_func_ops,
                            builder_->makeUintConstant(static_cast<u32>(1) << 2)),
      const_uint_0_);

  spv::Id center_depth32_biased = center_depth32_unbiased;
  if (!shader_writes_depth) {
    std::array<spv::Id, 2> depth_dxy_abs;
    for (u32 i = 0; i < 2; ++i) {
      depth_dxy_abs[i] = builder_->createUnaryBuiltinCall(type_float_, ext_inst_glsl_std_450_,
                                                          GLSLstd450FAbs, depth_dxy[i]);
    }
    spv::Id depth_max_slope = builder_->createBinBuiltinCall(
        type_float_, ext_inst_glsl_std_450_, GLSLstd450FMax, depth_dxy_abs[0], depth_dxy_abs[1]);
    spv::Id slope_scaled_poly_offset = builder_->createNoContractionBinOp(
        spv::OpFMul, type_float_, poly_offset_scale, depth_max_slope);
    spv::Id poly_offset = builder_->createNoContractionBinOp(
        spv::OpFAdd, type_float_, slope_scaled_poly_offset, poly_offset_offset);
    center_depth32_biased = builder_->createNoContractionBinOp(
        spv::OpFAdd, type_float_, center_depth32_unbiased, poly_offset);
  }

  spv::Id new_sample_mask = main_fsi_sample_mask_;
  std::array<spv::Id, 4> late_write_depth_stencil{};
  for (u32 i = 0; i < 4; ++i) {
    spv::Id sample_covered = builder_->createBinOp(
        spv::OpINotEqual, type_bool_,
        builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, new_sample_mask,
                              builder_->makeUintConstant(static_cast<u32>(1) << i)),
        const_uint_0_);
    SPIRVBuilder::IfBuilder if_sample_covered(sample_covered, spv::SelectionControlDontFlattenMask,
                                              *builder_);

    spv::Id sample_address = FSI_AddSampleOffset(main_fsi_address_depth_, i);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_int_0_);
    id_vector_temp_.push_back(sample_address);
    spv::Id sample_access_chain = builder_->createAccessChain(
        features_.spirv_version >= spv::Spv_1_3 ? spv::StorageClassStorageBuffer
                                                : spv::StorageClassUniform,
        buffer_edram_, id_vector_temp_);
    spv::Id old_depth_stencil = builder_->createLoad(sample_access_chain, spv::NoPrecision);

    spv::Id sample_depth32;
    if (shader_writes_depth) {
      sample_depth32 =
          builder_->createTriBuiltinCall(type_float_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                                         center_depth32_biased, const_float_0_, const_float_1_);
    } else {
      std::array<spv::Id, 2> sample_location;
      switch (i) {
        case 0: {
          for (u32 j = 0; j < 2; ++j) {
            sample_location[j] = builder_->makeFloatConstant(
                static_cast<f32>(draw_state::kD3D10StandardSamplePositions4x[0][j]) *
                (1.0f / 16.0f));
          }
          if (native_2x_msaa_no_attachments_) {
            for (u32 j = 0; j < 2; ++j) {
              sample_location[j] = builder_->createTriOp(
                  spv::OpSelect, type_float_, msaa_is_4x, sample_location[j],
                  builder_->makeFloatConstant(
                      static_cast<f32>(draw_state::kD3D10StandardSamplePositions2x[1][j]) *
                      (1.0f / 16.0f)));
            }
          }
          for (u32 j = 0; j < 2; ++j) {
            sample_location[j] = builder_->createTriOp(spv::OpSelect, type_float_, msaa_is_2x_4x,
                                                       sample_location[j], const_float_0_);
          }
        } break;
        case 1: {
          for (u32 j = 0; j < 2; ++j) {
            sample_location[j] = builder_->createTriOp(
                spv::OpSelect, type_float_, msaa_is_4x,
                builder_->makeFloatConstant(
                    static_cast<f32>(draw_state::kD3D10StandardSamplePositions4x[2][j]) *
                    (1.0f / 16.0f)),
                builder_->makeFloatConstant(
                    static_cast<f32>(native_2x_msaa_no_attachments_
                                         ? draw_state::kD3D10StandardSamplePositions2x[0][j]
                                         : draw_state::kD3D10StandardSamplePositions4x[3][j]) *
                    (1.0f / 16.0f)));
          }
        } break;
        default: {
          const i8* sample_location_int =
              draw_state::kD3D10StandardSamplePositions4x[i ^ (((i & 1) ^ (i >> 1)) * 0b11)];
          for (u32 j = 0; j < 2; ++j) {
            sample_location[j] = builder_->makeFloatConstant(
                static_cast<f32>(sample_location_int[j]) * (1.0f / 16.0f));
          }
        } break;
      }
      std::array<spv::Id, 2> sample_depth_dxy;
      for (u32 j = 0; j < 2; ++j) {
        sample_depth_dxy[j] = builder_->createNoContractionBinOp(spv::OpFMul, type_float_,
                                                                 sample_location[j], depth_dxy[j]);
      }
      sample_depth32 = builder_->createTriBuiltinCall(
          type_float_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
          builder_->createNoContractionBinOp(
              spv::OpFAdd, type_float_, center_depth32_biased,
              builder_->createNoContractionBinOp(spv::OpFAdd, type_float_, sample_depth_dxy[0],
                                                 sample_depth_dxy[1])),
          const_float_0_, const_float_1_);
    }

    SPIRVBuilder::IfBuilder depth_format_if(depth_is_float24, spv::SelectionControlDontFlattenMask,
                                            *builder_);
    spv::Id sample_depth_float24 = SPIRVShaderTranslator::PreClampedDepthTo20e4(
        *builder_, sample_depth32, true, false, ext_inst_glsl_std_450_);
    depth_format_if.makeBeginElse();
    spv::Id sample_depth_unorm24 =
        builder_->createUnaryOp(spv::OpConvertFToU, type_uint_,
                                builder_->createUnaryBuiltinCall(
                                    type_float_, ext_inst_glsl_std_450_, GLSLstd450RoundEven,
                                    builder_->createNoContractionBinOp(
                                        spv::OpFMul, type_float_, sample_depth32,
                                        builder_->makeFloatConstant(static_cast<f32>(0xFFFFFF)))));
    depth_format_if.makeEndIf();
    spv::Id sample_depth24 =
        depth_format_if.createMergePhi(sample_depth_float24, sample_depth_unorm24);

    spv::Id old_depth = builder_->createBinOp(spv::OpShiftRightLogical, type_uint_,
                                              old_depth_stencil, const_uint_8);
    spv::Id depth_passed = builder_->createBinOp(
        spv::OpLogicalAnd, type_bool_, depth_pass_if_less,
        builder_->createBinOp(spv::OpULessThan, type_bool_, sample_depth24, old_depth));
    depth_passed = builder_->createBinOp(
        spv::OpLogicalOr, type_bool_, depth_passed,
        builder_->createBinOp(
            spv::OpLogicalAnd, type_bool_, depth_pass_if_equal,
            builder_->createBinOp(spv::OpIEqual, type_bool_, sample_depth24, old_depth)));
    depth_passed = builder_->createBinOp(
        spv::OpLogicalOr, type_bool_, depth_passed,
        builder_->createBinOp(
            spv::OpLogicalAnd, type_bool_, depth_pass_if_greater,
            builder_->createBinOp(spv::OpUGreaterThan, type_bool_, sample_depth24, old_depth)));

    SPIRVBuilder::IfBuilder stencil_if(stencil_enabled, spv::SelectionControlDontFlattenMask,
                                       *builder_);
    spv::Id stencil_passed_if_enabled;
    spv::Id new_stencil_and_old_depth_if_stencil_enabled;
    {
      spv::Id old_stencil_read_masked = builder_->createBinOp(spv::OpBitwiseAnd, type_uint_,
                                                              old_depth_stencil, stencil_read_mask);
      stencil_passed_if_enabled = builder_->createBinOp(
          spv::OpLogicalAnd, type_bool_, stencil_pass_if_less,
          builder_->createBinOp(spv::OpULessThan, type_bool_, stencil_reference_read_masked,
                                old_stencil_read_masked));
      stencil_passed_if_enabled = builder_->createBinOp(
          spv::OpLogicalOr, type_bool_, stencil_passed_if_enabled,
          builder_->createBinOp(
              spv::OpLogicalAnd, type_bool_, stencil_pass_if_equal,
              builder_->createBinOp(spv::OpIEqual, type_bool_, stencil_reference_read_masked,
                                    old_stencil_read_masked)));
      stencil_passed_if_enabled = builder_->createBinOp(
          spv::OpLogicalOr, type_bool_, stencil_passed_if_enabled,
          builder_->createBinOp(
              spv::OpLogicalAnd, type_bool_, stencil_pass_if_greater,
              builder_->createBinOp(spv::OpUGreaterThan, type_bool_, stencil_reference_read_masked,
                                    old_stencil_read_masked)));
      spv::Id stencil_op = builder_->createTriOp(
          spv::OpBitFieldUExtract, type_uint_, stencil_func_ops,
          builder_->createTriOp(
              spv::OpSelect, type_uint_, stencil_passed_if_enabled,
              builder_->createTriOp(spv::OpSelect, type_uint_, depth_passed,
                                    builder_->makeUintConstant(6), builder_->makeUintConstant(9)),
              builder_->makeUintConstant(3)),
          builder_->makeUintConstant(3));
      spv::Block& block_stencil_op_head = *builder_->getBuildPoint();
      spv::Block& block_stencil_op_keep = builder_->makeNewBlock();
      spv::Block& block_stencil_op_zero = builder_->makeNewBlock();
      spv::Block& block_stencil_op_replace = builder_->makeNewBlock();
      spv::Block& block_stencil_op_increment_clamp = builder_->makeNewBlock();
      spv::Block& block_stencil_op_decrement_clamp = builder_->makeNewBlock();
      spv::Block& block_stencil_op_invert = builder_->makeNewBlock();
      spv::Block& block_stencil_op_increment_wrap = builder_->makeNewBlock();
      spv::Block& block_stencil_op_decrement_wrap = builder_->makeNewBlock();
      spv::Block& block_stencil_op_merge = builder_->makeNewBlock();
      builder_->createSelectionMerge(&block_stencil_op_merge, spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> stencil_op_switch_op =
            std::make_unique<spv::Instruction>(spv::OpSwitch);
        stencil_op_switch_op->addIdOperand(stencil_op);
        stencil_op_switch_op->addIdOperand(block_stencil_op_keep.getId());
        stencil_op_switch_op->addImmediateOperand(static_cast<i32>(xenos::StencilOp::kZero));
        stencil_op_switch_op->addIdOperand(block_stencil_op_zero.getId());
        stencil_op_switch_op->addImmediateOperand(static_cast<i32>(xenos::StencilOp::kReplace));
        stencil_op_switch_op->addIdOperand(block_stencil_op_replace.getId());
        stencil_op_switch_op->addImmediateOperand(
            static_cast<i32>(xenos::StencilOp::kIncrementClamp));
        stencil_op_switch_op->addIdOperand(block_stencil_op_increment_clamp.getId());
        stencil_op_switch_op->addImmediateOperand(
            static_cast<i32>(xenos::StencilOp::kDecrementClamp));
        stencil_op_switch_op->addIdOperand(block_stencil_op_decrement_clamp.getId());
        stencil_op_switch_op->addImmediateOperand(static_cast<i32>(xenos::StencilOp::kInvert));
        stencil_op_switch_op->addIdOperand(block_stencil_op_invert.getId());
        stencil_op_switch_op->addImmediateOperand(
            static_cast<i32>(xenos::StencilOp::kIncrementWrap));
        stencil_op_switch_op->addIdOperand(block_stencil_op_increment_wrap.getId());
        stencil_op_switch_op->addImmediateOperand(
            static_cast<i32>(xenos::StencilOp::kDecrementWrap));
        stencil_op_switch_op->addIdOperand(block_stencil_op_decrement_wrap.getId());
        builder_->getBuildPoint()->addInstruction(std::move(stencil_op_switch_op));
      }
      block_stencil_op_keep.addPredecessor(&block_stencil_op_head);
      block_stencil_op_zero.addPredecessor(&block_stencil_op_head);
      block_stencil_op_replace.addPredecessor(&block_stencil_op_head);
      block_stencil_op_increment_clamp.addPredecessor(&block_stencil_op_head);
      block_stencil_op_decrement_clamp.addPredecessor(&block_stencil_op_head);
      block_stencil_op_invert.addPredecessor(&block_stencil_op_head);
      block_stencil_op_increment_wrap.addPredecessor(&block_stencil_op_head);
      block_stencil_op_decrement_wrap.addPredecessor(&block_stencil_op_head);
      builder_->setBuildPoint(&block_stencil_op_keep);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_zero);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_replace);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_increment_clamp);
      spv::Id new_stencil_in_low_bits_increment_clamp = builder_->createBinOp(
          spv::OpIAdd, type_uint_,
          builder_->createBinBuiltinCall(
              type_uint_, ext_inst_glsl_std_450_, GLSLstd450UMin,
              builder_->makeUintConstant(UINT8_MAX - 1),
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, old_depth_stencil,
                                    builder_->makeUintConstant(UINT8_MAX))),
          const_uint_1);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_decrement_clamp);
      spv::Id new_stencil_in_low_bits_decrement_clamp = builder_->createBinOp(
          spv::OpISub, type_uint_,
          builder_->createBinBuiltinCall(
              type_uint_, ext_inst_glsl_std_450_, GLSLstd450UMax, const_uint_1,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, old_depth_stencil,
                                    builder_->makeUintConstant(UINT8_MAX))),
          const_uint_1);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_invert);
      spv::Id new_stencil_in_low_bits_invert =
          builder_->createUnaryOp(spv::OpNot, type_uint_, old_depth_stencil);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_increment_wrap);
      spv::Id new_stencil_in_low_bits_increment_wrap =
          builder_->createBinOp(spv::OpIAdd, type_uint_, old_depth_stencil, const_uint_1);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_decrement_wrap);
      spv::Id new_stencil_in_low_bits_decrement_wrap =
          builder_->createBinOp(spv::OpISub, type_uint_, old_depth_stencil, const_uint_1);
      builder_->createBranch(&block_stencil_op_merge);
      builder_->setBuildPoint(&block_stencil_op_merge);
      id_vector_temp_.clear();
      id_vector_temp_.reserve(size_t{2} * 8);
      id_vector_temp_.push_back(old_depth_stencil);
      id_vector_temp_.push_back(block_stencil_op_keep.getId());
      id_vector_temp_.push_back(const_uint_0_);
      id_vector_temp_.push_back(block_stencil_op_zero.getId());
      id_vector_temp_.push_back(stencil_reference);
      id_vector_temp_.push_back(block_stencil_op_replace.getId());
      id_vector_temp_.push_back(new_stencil_in_low_bits_increment_clamp);
      id_vector_temp_.push_back(block_stencil_op_increment_clamp.getId());
      id_vector_temp_.push_back(new_stencil_in_low_bits_decrement_clamp);
      id_vector_temp_.push_back(block_stencil_op_decrement_clamp.getId());
      id_vector_temp_.push_back(new_stencil_in_low_bits_invert);
      id_vector_temp_.push_back(block_stencil_op_invert.getId());
      id_vector_temp_.push_back(new_stencil_in_low_bits_increment_wrap);
      id_vector_temp_.push_back(block_stencil_op_increment_wrap.getId());
      id_vector_temp_.push_back(new_stencil_in_low_bits_decrement_wrap);
      id_vector_temp_.push_back(block_stencil_op_decrement_wrap.getId());
      spv::Id new_stencil_in_low_bits_if_enabled =
          builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
      new_stencil_and_old_depth_if_stencil_enabled = builder_->createBinOp(
          spv::OpBitwiseOr, type_uint_,
          builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, old_depth_stencil,
                                stencil_write_keep_mask),
          builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, new_stencil_in_low_bits_if_enabled,
                                stencil_write_mask));
    }
    stencil_if.makeEndIf();
    spv::Id stencil_passed =
        stencil_if.createMergePhi(stencil_passed_if_enabled, builder_->makeBoolConstant(true));
    spv::Id new_stencil_and_old_depth =
        stencil_if.createMergePhi(new_stencil_and_old_depth_if_stencil_enabled, old_depth_stencil);

    spv::Id depth_stencil_passed =
        builder_->createBinOp(spv::OpLogicalAnd, type_bool_, depth_passed, stencil_passed);
    spv::Id new_sample_mask_after_sample = builder_->createTriOp(
        spv::OpSelect, type_uint_, depth_stencil_passed, new_sample_mask,
        builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, new_sample_mask,
                              builder_->makeUintConstant(~(static_cast<u32>(1) << i))));

    spv::Id new_stencil_and_unconditional_new_depth =
        builder_->createQuadOp(spv::OpBitFieldInsert, type_uint_, new_stencil_and_old_depth,
                               sample_depth24, const_uint_8, builder_->makeUintConstant(24));
    spv::Id new_depth_stencil = builder_->createTriOp(
        spv::OpSelect, type_uint_,
        builder_->createBinOp(spv::OpLogicalAnd, type_bool_, depth_stencil_passed, depth_write),
        new_stencil_and_unconditional_new_depth, new_stencil_and_old_depth);

    spv::Id new_depth_stencil_different =
        builder_->createBinOp(spv::OpINotEqual, type_bool_, new_depth_stencil, old_depth_stencil);
    spv::Id new_depth_stencil_write_condition = spv::NoResult;
    if (is_early) {
      if (implicit_early_z_write_allowed) {
        new_sample_mask_after_sample = builder_->createTriOp(
            spv::OpSelect, type_uint_,
            builder_->createBinOp(spv::OpLogicalAnd, type_bool_, new_depth_stencil_different,
                                  not_early_write),
            builder_->createBinOp(spv::OpBitwiseOr, type_uint_, new_sample_mask_after_sample,
                                  builder_->makeUintConstant(static_cast<u32>(1) << (4 + i))),
            new_sample_mask_after_sample);
        new_depth_stencil_write_condition = builder_->createBinOp(
            spv::OpLogicalAnd, type_bool_, new_depth_stencil_different, early_write);
      } else {
        new_sample_mask_after_sample = builder_->createTriOp(
            spv::OpSelect, type_uint_, new_depth_stencil_different,
            builder_->createBinOp(spv::OpBitwiseOr, type_uint_, new_sample_mask_after_sample,
                                  builder_->makeUintConstant(static_cast<u32>(1) << (4 + i))),
            new_sample_mask_after_sample);
      }
    } else {
      new_depth_stencil_write_condition = new_depth_stencil_different;
    }
    if (new_depth_stencil_write_condition != spv::NoResult) {
      SPIRVBuilder::IfBuilder new_depth_stencil_write_if(
          new_depth_stencil_write_condition, spv::SelectionControlDontFlattenMask, *builder_);
      builder_->createStore(new_depth_stencil, sample_access_chain);
      new_depth_stencil_write_if.makeEndIf();
    }

    if_sample_covered.makeEndIf();
    new_sample_mask =
        if_sample_covered.createMergePhi(new_sample_mask_after_sample, new_sample_mask);
    if (is_early) {
      late_write_depth_stencil[i] =
          if_sample_covered.createMergePhi(new_depth_stencil, const_uint_0_);
    }
  }

  if (block_any_sample_covered_merge) {
    builder_->createBranch(block_any_sample_covered_merge);
    spv::Block& block_any_sample_covered_end = *builder_->getBuildPoint();
    builder_->setBuildPoint(block_any_sample_covered_merge);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(new_sample_mask);
    id_vector_temp_.push_back(block_any_sample_covered_end.getId());
    id_vector_temp_.push_back(main_fsi_sample_mask_);
    id_vector_temp_.push_back(block_any_sample_covered_head->getId());
    new_sample_mask = builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
    if (is_early) {
      for (u32 i = 0; i < 4; ++i) {
        id_vector_temp_.clear();
        id_vector_temp_.push_back(late_write_depth_stencil[i]);
        id_vector_temp_.push_back(block_any_sample_covered_end.getId());
        id_vector_temp_.push_back(const_uint_0_);
        id_vector_temp_.push_back(block_any_sample_covered_head->getId());
        late_write_depth_stencil[i] = builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
      }
    }
  }
  if_depth_stencil_enabled.makeEndIf();
  main_fsi_sample_mask_ =
      if_depth_stencil_enabled.createMergePhi(new_sample_mask, main_fsi_sample_mask_);
  if (is_early) {
    for (u32 i = 0; i < 4; ++i) {
      main_fsi_late_write_depth_stencil_[i] =
          if_depth_stencil_enabled.createMergePhi(late_write_depth_stencil[i], const_uint_0_);
    }
  }
}

std::array<spv::Id, 2> SPIRVShaderTranslator::FSI_ClampAndPackColor(spv::Id color_float4,
                                                                    spv::Id format_with_flags) {
  spv::Block& block_format_head = *builder_->getBuildPoint();
  spv::Block& block_format_8_8_8_8 = builder_->makeNewBlock();
  spv::Block& block_format_8_8_8_8_gamma = builder_->makeNewBlock();
  spv::Block& block_format_2_10_10_10 = builder_->makeNewBlock();
  spv::Block& block_format_2_10_10_10_float = builder_->makeNewBlock();
  spv::Block& block_format_16 = builder_->makeNewBlock();
  spv::Block& block_format_16_float = builder_->makeNewBlock();
  spv::Block& block_format_32_float = builder_->makeNewBlock();
  spv::Block& block_format_merge = builder_->makeNewBlock();
  builder_->createSelectionMerge(&block_format_merge, spv::SelectionControlDontFlattenMask);
  {
    std::unique_ptr<spv::Instruction> format_switch_op =
        std::make_unique<spv::Instruction>(spv::OpSwitch);
    format_switch_op->addIdOperand(format_with_flags);
    format_switch_op->addIdOperand(block_format_32_float.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_8_8_8_8)));
    format_switch_op->addIdOperand(block_format_8_8_8_8.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)));
    format_switch_op->addIdOperand(block_format_8_8_8_8_gamma.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_2_10_10_10)));
    format_switch_op->addIdOperand(block_format_2_10_10_10.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)));
    format_switch_op->addIdOperand(block_format_2_10_10_10.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)));
    format_switch_op->addIdOperand(block_format_2_10_10_10_float.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat ::k_2_10_10_10_FLOAT_AS_16_16_16_16)));
    format_switch_op->addIdOperand(block_format_2_10_10_10_float.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_16_16)));
    format_switch_op->addIdOperand(block_format_16.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_16_16_16_16)));
    format_switch_op->addIdOperand(block_format_16.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_16_16_FLOAT)));
    format_switch_op->addIdOperand(block_format_16_float.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT)));
    format_switch_op->addIdOperand(block_format_16_float.getId());
    builder_->getBuildPoint()->addInstruction(std::move(format_switch_op));
  }
  block_format_8_8_8_8.addPredecessor(&block_format_head);
  block_format_8_8_8_8_gamma.addPredecessor(&block_format_head);
  block_format_2_10_10_10.addPredecessor(&block_format_head);
  block_format_2_10_10_10_float.addPredecessor(&block_format_head);
  block_format_16.addPredecessor(&block_format_head);
  block_format_16_float.addPredecessor(&block_format_head);
  block_format_32_float.addPredecessor(&block_format_head);

  spv::Id unorm_round_offset_float = builder_->makeFloatConstant(0.5f);
  id_vector_temp_.clear();
  id_vector_temp_.resize(4, unorm_round_offset_float);
  spv::Id unorm_round_offset_float4 =
      builder_->makeCompositeConstant(type_float4_, id_vector_temp_);

  spv::Id packed_8_8_8_8;
  {
    builder_->setBuildPoint(&block_format_8_8_8_8);
    spv::Id color_scaled = builder_->createNoContractionBinOp(
        spv::OpVectorTimesScalar, type_float4_,
        builder_->createTriBuiltinCall(type_float4_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                                       color_float4, const_float4_0_, const_float4_1_),
        builder_->makeFloatConstant(255.0f));
    spv::Id color_offset = builder_->createNoContractionBinOp(
        spv::OpFAdd, type_float4_, color_scaled, unorm_round_offset_float4);
    spv::Id color_uint4 = builder_->createUnaryOp(spv::OpConvertFToU, type_uint4_, color_offset);
    packed_8_8_8_8 = builder_->createCompositeExtract(color_uint4, type_uint_, 0);
    spv::Id component_width = builder_->makeUintConstant(8);
    for (u32 i = 1; i < 4; ++i) {
      packed_8_8_8_8 =
          builder_->createQuadOp(spv::OpBitFieldInsert, type_uint_, packed_8_8_8_8,
                                 builder_->createCompositeExtract(color_uint4, type_uint_, i),
                                 builder_->makeUintConstant(8 * i), component_width);
    }
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_8_8_8_8_end = *builder_->getBuildPoint();

  spv::Id packed_8_8_8_8_gamma;
  {
    builder_->setBuildPoint(&block_format_8_8_8_8_gamma);
    uint_vector_temp_.clear();
    uint_vector_temp_.push_back(0);
    uint_vector_temp_.push_back(1);
    uint_vector_temp_.push_back(2);
    spv::Id color_rgb = builder_->createRvalueSwizzle(spv::NoPrecision, type_float3_, color_float4,
                                                      uint_vector_temp_);
    spv::Id rgb_gamma = LinearToPWLGamma(color_rgb, false);
    spv::Id alpha_clamped = builder_->createTriBuiltinCall(
        type_float_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
        builder_->createCompositeExtract(color_float4, type_float_, 3), const_float_0_,
        const_float_1_);
    // Vectors can be concatenated by createCompositeConstruct despite its assertion.
    spv::Id color_gamma;
    {
      std::unique_ptr<spv::Instruction> color_gamma_composite_construct_op =
          std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float4_,
                                             spv::OpCompositeConstruct);
      color_gamma_composite_construct_op->addIdOperand(rgb_gamma);
      color_gamma_composite_construct_op->addIdOperand(alpha_clamped);
      color_gamma = color_gamma_composite_construct_op->getResultId();
      builder_->getBuildPoint()->addInstruction(std::move(color_gamma_composite_construct_op));
    }
    spv::Id color_scaled = builder_->createNoContractionBinOp(
        spv::OpVectorTimesScalar, type_float4_, color_gamma, builder_->makeFloatConstant(255.0f));
    spv::Id color_offset = builder_->createNoContractionBinOp(
        spv::OpFAdd, type_float4_, color_scaled, unorm_round_offset_float4);
    spv::Id color_uint4 = builder_->createUnaryOp(spv::OpConvertFToU, type_uint4_, color_offset);
    packed_8_8_8_8_gamma = builder_->createCompositeExtract(color_uint4, type_uint_, 0);
    spv::Id component_width = builder_->makeUintConstant(8);
    for (u32 i = 1; i < 4; ++i) {
      packed_8_8_8_8_gamma =
          builder_->createQuadOp(spv::OpBitFieldInsert, type_uint_, packed_8_8_8_8_gamma,
                                 builder_->createCompositeExtract(color_uint4, type_uint_, i),
                                 builder_->makeUintConstant(8 * i), component_width);
    }
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_8_8_8_8_gamma_end = *builder_->getBuildPoint();

  spv::Id packed_2_10_10_10;
  {
    builder_->setBuildPoint(&block_format_2_10_10_10);
    spv::Id color_clamped =
        builder_->createTriBuiltinCall(type_float4_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                                       color_float4, const_float4_0_, const_float4_1_);
    id_vector_temp_.clear();
    id_vector_temp_.resize(3, builder_->makeFloatConstant(1023.0f));
    id_vector_temp_.push_back(builder_->makeFloatConstant(3.0f));
    spv::Id color_scaled = builder_->createNoContractionBinOp(
        spv::OpFMul, type_float4_, color_clamped,
        builder_->makeCompositeConstant(type_float4_, id_vector_temp_));
    spv::Id color_offset = builder_->createNoContractionBinOp(
        spv::OpFAdd, type_float4_, color_scaled, unorm_round_offset_float4);
    spv::Id color_uint4 = builder_->createUnaryOp(spv::OpConvertFToU, type_uint4_, color_offset);
    packed_2_10_10_10 = builder_->createCompositeExtract(color_uint4, type_uint_, 0);
    spv::Id rgb_width = builder_->makeUintConstant(10);
    spv::Id alpha_width = builder_->makeUintConstant(2);
    for (u32 i = 1; i < 4; ++i) {
      packed_2_10_10_10 = builder_->createQuadOp(
          spv::OpBitFieldInsert, type_uint_, packed_2_10_10_10,
          builder_->createCompositeExtract(color_uint4, type_uint_, i),
          builder_->makeUintConstant(10 * i), i == 3 ? alpha_width : rgb_width);
    }
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_2_10_10_10_end = *builder_->getBuildPoint();

  spv::Id packed_2_10_10_10_float;
  {
    builder_->setBuildPoint(&block_format_2_10_10_10_float);
    std::array<spv::Id, 4> color_components;
    for (u32 i = 0; i < 3; ++i) {
      color_components[i] = UnclampedFloat32To7e3(
          *builder_, builder_->createCompositeExtract(color_float4, type_float_, i),
          ext_inst_glsl_std_450_);
    }
    spv::Id alpha_scaled = builder_->createNoContractionBinOp(
        spv::OpFMul, type_float_,
        builder_->createTriBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
            builder_->createCompositeExtract(color_float4, type_float_, 3), const_float_0_,
            const_float_1_),
        builder_->makeFloatConstant(3.0f));
    spv::Id alpha_offset = builder_->createNoContractionBinOp(
        spv::OpFAdd, type_float_, alpha_scaled, unorm_round_offset_float);
    color_components[3] = builder_->createUnaryOp(spv::OpConvertFToU, type_uint_, alpha_offset);
    packed_2_10_10_10_float = color_components[0];
    spv::Id rgb_width = builder_->makeUintConstant(10);
    for (u32 i = 1; i < 3; ++i) {
      packed_2_10_10_10_float = builder_->createQuadOp(
          spv::OpBitFieldInsert, type_uint_, packed_2_10_10_10_float, color_components[i],
          builder_->makeUintConstant(10 * i), rgb_width);
    }
    packed_2_10_10_10_float = builder_->createQuadOp(
        spv::OpBitFieldInsert, type_uint_, packed_2_10_10_10_float, color_components[3],
        builder_->makeUintConstant(30), builder_->makeUintConstant(2));
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_2_10_10_10_float_end = *builder_->getBuildPoint();

  std::array<spv::Id, 2> packed_16;
  {
    builder_->setBuildPoint(&block_format_16);
    id_vector_temp_.clear();
    id_vector_temp_.resize(4, builder_->makeFloatConstant(-32.0f));
    spv::Id const_float4_minus_32 = builder_->makeCompositeConstant(type_float4_, id_vector_temp_);
    id_vector_temp_.clear();
    id_vector_temp_.resize(4, builder_->makeFloatConstant(32.0f));
    spv::Id const_float4_32 = builder_->makeCompositeConstant(type_float4_, id_vector_temp_);
    id_vector_temp_.clear();
    spv::Id color_scaled = builder_->createNoContractionBinOp(
        spv::OpVectorTimesScalar, type_float4_,
        builder_->createTriBuiltinCall(
            type_float4_, ext_inst_glsl_std_450_, GLSLstd450FClamp,
            builder_->createTriOp(spv::OpSelect, type_float4_,
                                  builder_->createUnaryOp(spv::OpIsNan, type_bool4_, color_float4),
                                  const_float4_0_, color_float4),
            const_float4_minus_32, const_float4_32),
        builder_->makeFloatConstant(32767.0f / 32.0f));
    id_vector_temp_.clear();
    id_vector_temp_.resize(4, builder_->makeFloatConstant(-0.5f));
    spv::Id unorm_round_offset_negative_float4 =
        builder_->makeCompositeConstant(type_float4_, id_vector_temp_);
    spv::Id color_offset = builder_->createNoContractionBinOp(
        spv::OpFAdd, type_float4_, color_scaled,
        builder_->createTriOp(
            spv::OpSelect, type_float4_,
            builder_->createBinOp(spv::OpFOrdLessThan, type_bool4_, color_scaled, const_float4_0_),
            unorm_round_offset_negative_float4, unorm_round_offset_float4));
    spv::Id color_uint4 = builder_->createUnaryOp(
        spv::OpBitcast, type_uint4_,
        builder_->createUnaryOp(spv::OpConvertFToS, type_int4_, color_offset));
    spv::Id component_offset_width = builder_->makeUintConstant(16);
    for (u32 i = 0; i < 2; ++i) {
      packed_16[i] = builder_->createQuadOp(
          spv::OpBitFieldInsert, type_uint_,
          builder_->createCompositeExtract(color_uint4, type_uint_, 2 * i),
          builder_->createCompositeExtract(color_uint4, type_uint_, 2 * i + 1),
          component_offset_width, component_offset_width);
    }
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_16_end = *builder_->getBuildPoint();

  std::array<spv::Id, 2> packed_16_float;
  {
    builder_->setBuildPoint(&block_format_16_float);
    id_vector_temp_.clear();
    id_vector_temp_.resize(4, builder_->makeFloatConstant(-65504.0f));
    spv::Id const_float4_minus_float16_max =
        builder_->makeCompositeConstant(type_float4_, id_vector_temp_);
    id_vector_temp_.clear();
    id_vector_temp_.resize(4, builder_->makeFloatConstant(65504.0f));
    spv::Id const_float4_float16_max =
        builder_->makeCompositeConstant(type_float4_, id_vector_temp_);
    spv::Id color_clamped = builder_->createTriBuiltinCall(
        type_float4_, ext_inst_glsl_std_450_, GLSLstd450FClamp,
        builder_->createTriOp(spv::OpSelect, type_float4_,
                              builder_->createUnaryOp(spv::OpIsNan, type_bool4_, color_float4),
                              const_float4_0_, color_float4),
        const_float4_minus_float16_max, const_float4_float16_max);
    for (u32 i = 0; i < 2; ++i) {
      uint_vector_temp_.clear();
      uint_vector_temp_.push_back(2 * i);
      uint_vector_temp_.push_back(2 * i + 1);
      packed_16_float[i] = builder_->createUnaryBuiltinCall(
          type_uint_, ext_inst_glsl_std_450_, GLSLstd450PackHalf2x16,
          builder_->createRvalueSwizzle(spv::NoPrecision, type_float2_, color_clamped,
                                        uint_vector_temp_));
    }
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_16_float_end = *builder_->getBuildPoint();

  std::array<spv::Id, 2> packed_32_float;
  {
    builder_->setBuildPoint(&block_format_32_float);
    for (u32 i = 0; i < 2; ++i) {
      packed_32_float[i] =
          builder_->createUnaryOp(spv::OpBitcast, type_uint_,
                                  builder_->createCompositeExtract(color_float4, type_float_, i));
    }
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_32_float_end = *builder_->getBuildPoint();

  builder_->setBuildPoint(&block_format_merge);
  std::array<spv::Id, 2> packed;
  id_vector_temp_.reserve(size_t{2} * 7);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(packed_8_8_8_8);
  id_vector_temp_.push_back(block_format_8_8_8_8_end.getId());
  id_vector_temp_.push_back(packed_8_8_8_8_gamma);
  id_vector_temp_.push_back(block_format_8_8_8_8_gamma_end.getId());
  id_vector_temp_.push_back(packed_2_10_10_10);
  id_vector_temp_.push_back(block_format_2_10_10_10_end.getId());
  id_vector_temp_.push_back(packed_2_10_10_10_float);
  id_vector_temp_.push_back(block_format_2_10_10_10_float_end.getId());
  id_vector_temp_.push_back(packed_16[0]);
  id_vector_temp_.push_back(block_format_16_end.getId());
  id_vector_temp_.push_back(packed_16_float[0]);
  id_vector_temp_.push_back(block_format_16_float_end.getId());
  id_vector_temp_.push_back(packed_32_float[0]);
  id_vector_temp_.push_back(block_format_32_float_end.getId());
  packed[0] = builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(const_uint_0_);
  id_vector_temp_.push_back(block_format_8_8_8_8_end.getId());
  id_vector_temp_.push_back(const_uint_0_);
  id_vector_temp_.push_back(block_format_8_8_8_8_gamma_end.getId());
  id_vector_temp_.push_back(const_uint_0_);
  id_vector_temp_.push_back(block_format_2_10_10_10_end.getId());
  id_vector_temp_.push_back(const_uint_0_);
  id_vector_temp_.push_back(block_format_2_10_10_10_float_end.getId());
  id_vector_temp_.push_back(packed_16[1]);
  id_vector_temp_.push_back(block_format_16_end.getId());
  id_vector_temp_.push_back(packed_16_float[1]);
  id_vector_temp_.push_back(block_format_16_float_end.getId());
  id_vector_temp_.push_back(packed_32_float[1]);
  id_vector_temp_.push_back(block_format_32_float_end.getId());
  packed[1] = builder_->createOp(spv::OpPhi, type_uint_, id_vector_temp_);
  return packed;
}

std::array<spv::Id, 4> SPIRVShaderTranslator::FSI_UnpackColor(std::array<spv::Id, 2> color_packed,
                                                              spv::Id format_with_flags) {
  spv::Block& block_format_head = *builder_->getBuildPoint();
  spv::Block& block_format_8_8_8_8 = builder_->makeNewBlock();
  spv::Block& block_format_8_8_8_8_gamma = builder_->makeNewBlock();
  spv::Block& block_format_2_10_10_10 = builder_->makeNewBlock();
  spv::Block& block_format_2_10_10_10_float = builder_->makeNewBlock();
  spv::Block& block_format_16_16 = builder_->makeNewBlock();
  spv::Block& block_format_16_16_16_16 = builder_->makeNewBlock();
  spv::Block& block_format_16_16_float = builder_->makeNewBlock();
  spv::Block& block_format_16_16_16_16_float = builder_->makeNewBlock();
  spv::Block& block_format_32_float = builder_->makeNewBlock();
  spv::Block& block_format_32_32_float = builder_->makeNewBlock();
  spv::Block& block_format_merge = builder_->makeNewBlock();
  builder_->createSelectionMerge(&block_format_merge, spv::SelectionControlDontFlattenMask);
  {
    std::unique_ptr<spv::Instruction> format_switch_op =
        std::make_unique<spv::Instruction>(spv::OpSwitch);
    format_switch_op->addIdOperand(format_with_flags);
    format_switch_op->addIdOperand(block_format_32_float.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_8_8_8_8)));
    format_switch_op->addIdOperand(block_format_8_8_8_8.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)));
    format_switch_op->addIdOperand(block_format_8_8_8_8_gamma.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_2_10_10_10)));
    format_switch_op->addIdOperand(block_format_2_10_10_10.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)));
    format_switch_op->addIdOperand(block_format_2_10_10_10.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)));
    format_switch_op->addIdOperand(block_format_2_10_10_10_float.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat ::k_2_10_10_10_FLOAT_AS_16_16_16_16)));
    format_switch_op->addIdOperand(block_format_2_10_10_10_float.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_16_16)));
    format_switch_op->addIdOperand(block_format_16_16.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_16_16_16_16)));
    format_switch_op->addIdOperand(block_format_16_16_16_16.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_16_16_FLOAT)));
    format_switch_op->addIdOperand(block_format_16_16_float.getId());
    format_switch_op->addImmediateOperand(
        static_cast<i32>(RenderTargetCache::AddPSIColorFormatFlags(
            xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT)));
    format_switch_op->addIdOperand(block_format_16_16_16_16_float.getId());
    format_switch_op->addImmediateOperand(static_cast<i32>(
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_32_32_FLOAT)));
    format_switch_op->addIdOperand(block_format_32_32_float.getId());
    builder_->getBuildPoint()->addInstruction(std::move(format_switch_op));
  }
  block_format_8_8_8_8.addPredecessor(&block_format_head);
  block_format_8_8_8_8_gamma.addPredecessor(&block_format_head);
  block_format_2_10_10_10.addPredecessor(&block_format_head);
  block_format_2_10_10_10_float.addPredecessor(&block_format_head);
  block_format_16_16.addPredecessor(&block_format_head);
  block_format_16_16_16_16.addPredecessor(&block_format_head);
  block_format_16_16_float.addPredecessor(&block_format_head);
  block_format_16_16_16_16_float.addPredecessor(&block_format_head);
  block_format_32_float.addPredecessor(&block_format_head);
  block_format_32_32_float.addPredecessor(&block_format_head);

  std::array<std::array<spv::Id, 4>, 2> unpacked_8_8_8_8_and_gamma;
  std::array<spv::Block*, 2> block_format_8_8_8_8_and_gamma_end;
  {
    spv::Id component_width = builder_->makeUintConstant(8);
    spv::Id component_scale = builder_->makeFloatConstant(1.0f / 255.0f);
    for (u32 i = 0; i < 2; ++i) {
      builder_->setBuildPoint(i ? &block_format_8_8_8_8_gamma : &block_format_8_8_8_8);
      for (u32 j = 0; j < 4; ++j) {
        spv::Id component = builder_->createNoContractionBinOp(
            spv::OpFMul, type_float_,
            builder_->createUnaryOp(
                spv::OpConvertUToF, type_float_,
                builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, color_packed[0],
                                      builder_->makeUintConstant(8 * j), component_width)),
            component_scale);
        if (i && j <= 2) {
          component = PWLGammaToLinear(component, true);
        }
        unpacked_8_8_8_8_and_gamma[i][j] = component;
      }
      builder_->createBranch(&block_format_merge);
      block_format_8_8_8_8_and_gamma_end[i] = builder_->getBuildPoint();
    }
  }

  std::array<spv::Id, 4> unpacked_2_10_10_10;
  {
    builder_->setBuildPoint(&block_format_2_10_10_10);
    spv::Id rgb_width = builder_->makeUintConstant(10);
    spv::Id alpha_width = builder_->makeUintConstant(2);
    spv::Id rgb_scale = builder_->makeFloatConstant(1.0f / 1023.0f);
    spv::Id alpha_scale = builder_->makeFloatConstant(1.0f / 3.0f);
    for (u32 i = 0; i < 4; ++i) {
      unpacked_2_10_10_10[i] = builder_->createNoContractionBinOp(
          spv::OpFMul, type_float_,
          builder_->createUnaryOp(
              spv::OpConvertUToF, type_float_,
              builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, color_packed[0],
                                    builder_->makeUintConstant(10 * i),
                                    i == 3 ? alpha_width : rgb_width)),
          i == 3 ? alpha_scale : rgb_scale);
    }
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_2_10_10_10_end = *builder_->getBuildPoint();

  std::array<spv::Id, 4> unpacked_2_10_10_10_float;
  {
    builder_->setBuildPoint(&block_format_2_10_10_10_float);
    spv::Id rgb_width = builder_->makeUintConstant(10);
    for (u32 i = 0; i < 3; ++i) {
      unpacked_2_10_10_10_float[i] =
          Float7e3To32(*builder_,
                       builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, color_packed[0],
                                             builder_->makeUintConstant(10 * i), rgb_width),
                       0, false, ext_inst_glsl_std_450_);
    }
    unpacked_2_10_10_10_float[3] = builder_->createNoContractionBinOp(
        spv::OpFMul, type_float_,
        builder_->createUnaryOp(
            spv::OpConvertUToF, type_float_,
            builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, color_packed[0],
                                  builder_->makeUintConstant(30), builder_->makeUintConstant(2))),
        builder_->makeFloatConstant(1.0f / 3.0f));
    builder_->createBranch(&block_format_merge);
  }
  spv::Block& block_format_2_10_10_10_float_end = *builder_->getBuildPoint();

  std::array<std::array<spv::Id, 4>, 2> unpacked_16;
  unpacked_16[0][2] = const_float_0_;
  unpacked_16[0][3] = const_float_1_;
  std::array<spv::Block*, 2> block_format_16_end;
  {
    spv::Id component_width = builder_->makeUintConstant(16);
    spv::Id component_scale = builder_->makeFloatConstant(32.0f / 32767.0f);
    spv::Id component_min = builder_->makeFloatConstant(-1.0f);
    for (u32 i = 0; i < 2; ++i) {
      builder_->setBuildPoint(i ? &block_format_16_16_16_16 : &block_format_16_16);
      std::array<spv::Id, 2> color_packed_signed;
      for (u32 j = 0; j <= i; ++j) {
        color_packed_signed[j] =
            builder_->createUnaryOp(spv::OpBitcast, type_int_, color_packed[j]);
      }
      for (u32 j = 0; j < u32(i ? 4 : 2); ++j) {
        spv::Id component = builder_->createNoContractionBinOp(
            spv::OpFMul, type_float_,
            builder_->createUnaryOp(
                spv::OpConvertSToF, type_float_,
                builder_->createTriOp(spv::OpBitFieldSExtract, type_int_,
                                      color_packed_signed[j >> 1],
                                      builder_->makeUintConstant(16 * (j & 1)), component_width)),
            component_scale);
        component = builder_->createBinBuiltinCall(type_float_, ext_inst_glsl_std_450_,
                                                   GLSLstd450FMax, component_min, component);
        unpacked_16[i][j] = component;
      }
      builder_->createBranch(&block_format_merge);
      block_format_16_end[i] = builder_->getBuildPoint();
    }
  }

  std::array<std::array<spv::Id, 4>, 2> unpacked_16_float;
  unpacked_16_float[0][2] = const_float_0_;
  unpacked_16_float[0][3] = const_float_1_;
  std::array<spv::Block*, 2> block_format_16_float_end;
  {
    for (u32 i = 0; i < 2; ++i) {
      builder_->setBuildPoint(i ? &block_format_16_16_16_16_float : &block_format_16_16_float);
      for (u32 j = 0; j <= i; ++j) {
        spv::Id components_float2 = builder_->createUnaryBuiltinCall(
            type_float2_, ext_inst_glsl_std_450_, GLSLstd450UnpackHalf2x16, color_packed[j]);
        for (u32 k = 0; k < 2; ++k) {
          unpacked_16_float[i][2 * j + k] =
              builder_->createCompositeExtract(components_float2, type_float_, k);
        }
      }
      builder_->createBranch(&block_format_merge);
      block_format_16_float_end[i] = builder_->getBuildPoint();
    }
  }

  std::array<std::array<spv::Id, 4>, 2> unpacked_32_float;
  unpacked_32_float[0][1] = const_float_0_;
  unpacked_32_float[0][2] = const_float_0_;
  unpacked_32_float[0][3] = const_float_1_;
  unpacked_32_float[1][2] = const_float_0_;
  unpacked_32_float[1][3] = const_float_1_;
  std::array<spv::Block*, 2> block_format_32_float_end;
  {
    for (u32 i = 0; i < 2; ++i) {
      builder_->setBuildPoint(i ? &block_format_32_32_float : &block_format_32_float);
      for (u32 j = 0; j <= i; ++j) {
        unpacked_32_float[i][j] =
            builder_->createUnaryOp(spv::OpBitcast, type_float_, color_packed[j]);
      }
      builder_->createBranch(&block_format_merge);
      block_format_32_float_end[i] = builder_->getBuildPoint();
    }
  }

  builder_->setBuildPoint(&block_format_merge);
  std::array<spv::Id, 4> unpacked;
  id_vector_temp_.reserve(size_t{2} * 10);
  for (u32 i = 0; i < 4; ++i) {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(unpacked_8_8_8_8_and_gamma[0][i]);
    id_vector_temp_.push_back(block_format_8_8_8_8_and_gamma_end[0]->getId());
    id_vector_temp_.push_back(unpacked_8_8_8_8_and_gamma[1][i]);
    id_vector_temp_.push_back(block_format_8_8_8_8_and_gamma_end[1]->getId());
    id_vector_temp_.push_back(unpacked_2_10_10_10[i]);
    id_vector_temp_.push_back(block_format_2_10_10_10_end.getId());
    id_vector_temp_.push_back(unpacked_2_10_10_10_float[i]);
    id_vector_temp_.push_back(block_format_2_10_10_10_float_end.getId());
    id_vector_temp_.push_back(unpacked_16[0][i]);
    id_vector_temp_.push_back(block_format_16_end[0]->getId());
    id_vector_temp_.push_back(unpacked_16[1][i]);
    id_vector_temp_.push_back(block_format_16_end[1]->getId());
    id_vector_temp_.push_back(unpacked_16_float[0][i]);
    id_vector_temp_.push_back(block_format_16_float_end[0]->getId());
    id_vector_temp_.push_back(unpacked_16_float[1][i]);
    id_vector_temp_.push_back(block_format_16_float_end[1]->getId());
    id_vector_temp_.push_back(unpacked_32_float[0][i]);
    id_vector_temp_.push_back(block_format_32_float_end[0]->getId());
    id_vector_temp_.push_back(unpacked_32_float[1][i]);
    id_vector_temp_.push_back(block_format_32_float_end[1]->getId());
    unpacked[i] = builder_->createOp(spv::OpPhi, type_float_, id_vector_temp_);
  }
  return unpacked;
}

spv::Id SPIRVShaderTranslator::FSI_FlushNaNClampAndInBlending(spv::Id color_or_alpha,
                                                              spv::Id is_fixed_point,
                                                              spv::Id min_value,
                                                              spv::Id max_value) {
  spv::Id color_or_alpha_type = builder_->getTypeId(color_or_alpha);
  u32 component_count = static_cast<u32>(builder_->getNumTypeConstituents(color_or_alpha_type));
  REX_ASSERT(builder_->isScalarType(color_or_alpha_type) ||
             builder_->isVectorType(color_or_alpha_type));
  REX_ASSERT(builder_->isFloatType(builder_->getScalarTypeId(color_or_alpha_type)));
  REX_ASSERT(builder_->getTypeId(min_value) == color_or_alpha_type);
  REX_ASSERT(builder_->getTypeId(max_value) == color_or_alpha_type);

  SPIRVBuilder::IfBuilder if_fixed_point(is_fixed_point, spv::SelectionControlDontFlattenMask,
                                         *builder_);
  spv::Id color_or_alpha_clamped;
  {
    color_or_alpha_clamped = builder_->createTriBuiltinCall(
        color_or_alpha_type, ext_inst_glsl_std_450_, GLSLstd450FClamp,
        builder_->createTriOp(
            spv::OpSelect, color_or_alpha_type,
            builder_->createUnaryOp(spv::OpIsNan, type_bool_vectors_[component_count - 1],
                                    color_or_alpha),
            const_float_vectors_0_[component_count - 1], color_or_alpha),
        min_value, max_value);
  }
  if_fixed_point.makeEndIf();

  return if_fixed_point.createMergePhi(color_or_alpha_clamped, color_or_alpha);
}

spv::Id SPIRVShaderTranslator::FSI_ApplyColorBlendFactor(
    spv::Id value, spv::Id is_fixed_point, spv::Id clamp_min_value, spv::Id clamp_max_value,
    spv::Id factor, spv::Id source_color, spv::Id source_alpha, spv::Id dest_color,
    spv::Id dest_alpha, spv::Id constant_color, spv::Id constant_alpha) {
  // Compute the condition before OpSelectionMerge, which must be the penultimate instruction.
  SPIRVBuilder::IfBuilder factor_not_zero_if(
      builder_->createBinOp(
          spv::OpINotEqual, type_bool_, factor,
          builder_->makeUintConstant(std::to_underlying(xenos::BlendFactor::kZero))),
      spv::SelectionControlDontFlattenMask, *builder_);

  spv::Block& block_factor_head = *builder_->getBuildPoint();
  spv::Block& block_factor_one = builder_->makeNewBlock();
  std::array<spv::Block*, 3> color_factor_blocks;
  std::array<spv::Block*, 3> one_minus_color_factor_blocks;
  std::array<spv::Block*, 3> alpha_factor_blocks;
  std::array<spv::Block*, 3> one_minus_alpha_factor_blocks;
  color_factor_blocks[0] = &builder_->makeNewBlock();
  one_minus_color_factor_blocks[0] = &builder_->makeNewBlock();
  alpha_factor_blocks[0] = &builder_->makeNewBlock();
  one_minus_alpha_factor_blocks[0] = &builder_->makeNewBlock();
  color_factor_blocks[1] = &builder_->makeNewBlock();
  one_minus_color_factor_blocks[1] = &builder_->makeNewBlock();
  alpha_factor_blocks[1] = &builder_->makeNewBlock();
  one_minus_alpha_factor_blocks[1] = &builder_->makeNewBlock();
  color_factor_blocks[2] = &builder_->makeNewBlock();
  one_minus_color_factor_blocks[2] = &builder_->makeNewBlock();
  alpha_factor_blocks[2] = &builder_->makeNewBlock();
  one_minus_alpha_factor_blocks[2] = &builder_->makeNewBlock();
  spv::Block& block_factor_source_alpha_saturate = builder_->makeNewBlock();
  spv::Block& block_factor_merge = builder_->makeNewBlock();
  builder_->createSelectionMerge(&block_factor_merge, spv::SelectionControlDontFlattenMask);
  {
    std::unique_ptr<spv::Instruction> factor_switch_op =
        std::make_unique<spv::Instruction>(spv::OpSwitch);
    factor_switch_op->addIdOperand(factor);
    factor_switch_op->addIdOperand(block_factor_one.getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kSrcColor));
    factor_switch_op->addIdOperand(color_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusSrcColor));
    factor_switch_op->addIdOperand(one_minus_color_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kSrcAlpha));
    factor_switch_op->addIdOperand(alpha_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusSrcAlpha));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kDstColor));
    factor_switch_op->addIdOperand(color_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusDstColor));
    factor_switch_op->addIdOperand(one_minus_color_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kDstAlpha));
    factor_switch_op->addIdOperand(alpha_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusDstAlpha));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kConstantColor));
    factor_switch_op->addIdOperand(color_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(
        static_cast<i32>(xenos::BlendFactor::kOneMinusConstantColor));
    factor_switch_op->addIdOperand(one_minus_color_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kConstantAlpha));
    factor_switch_op->addIdOperand(alpha_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(
        static_cast<i32>(xenos::BlendFactor::kOneMinusConstantAlpha));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kSrcAlphaSaturate));
    factor_switch_op->addIdOperand(block_factor_source_alpha_saturate.getId());
    builder_->getBuildPoint()->addInstruction(std::move(factor_switch_op));
  }
  block_factor_one.addPredecessor(&block_factor_head);
  for (u32 i = 0; i < 3; ++i) {
    color_factor_blocks[i]->addPredecessor(&block_factor_head);
    one_minus_color_factor_blocks[i]->addPredecessor(&block_factor_head);
    alpha_factor_blocks[i]->addPredecessor(&block_factor_head);
    one_minus_alpha_factor_blocks[i]->addPredecessor(&block_factor_head);
  }
  block_factor_source_alpha_saturate.addPredecessor(&block_factor_head);

  builder_->setBuildPoint(&block_factor_one);
  builder_->createBranch(&block_factor_merge);

  std::array<spv::Id, 3> color_factors = {
      source_color,
      dest_color,
      constant_color,
  };
  std::array<spv::Id, 3> alpha_factors = {
      source_alpha,
      dest_alpha,
      constant_alpha,
  };
  std::array<spv::Id, 3> color_factor_results;
  std::array<spv::Id, 3> one_minus_color_factor_results;
  std::array<spv::Id, 3> alpha_factor_results;
  std::array<spv::Id, 3> one_minus_alpha_factor_results;
  for (u32 i = 0; i < 3; ++i) {
    spv::Id color_factor = color_factors[i];
    spv::Id alpha_factor = alpha_factors[i];

    {
      builder_->setBuildPoint(color_factor_blocks[i]);
      color_factor_results[i] =
          builder_->createNoContractionBinOp(spv::OpFMul, type_float3_, value, color_factor);
      builder_->createBranch(&block_factor_merge);
    }

    {
      builder_->setBuildPoint(one_minus_color_factor_blocks[i]);
      one_minus_color_factor_results[i] = builder_->createNoContractionBinOp(
          spv::OpFMul, type_float3_, value,
          builder_->createNoContractionBinOp(spv::OpFSub, type_float3_, const_float3_1_,
                                             color_factor));
      builder_->createBranch(&block_factor_merge);
    }

    {
      builder_->setBuildPoint(alpha_factor_blocks[i]);
      alpha_factor_results[i] = builder_->createNoContractionBinOp(
          spv::OpVectorTimesScalar, type_float3_, value, alpha_factor);
      builder_->createBranch(&block_factor_merge);
    }

    {
      builder_->setBuildPoint(one_minus_alpha_factor_blocks[i]);
      one_minus_alpha_factor_results[i] = builder_->createNoContractionBinOp(
          spv::OpVectorTimesScalar, type_float3_, value,
          builder_->createNoContractionBinOp(spv::OpFSub, type_float_, const_float_1_,
                                             alpha_factor));
      builder_->createBranch(&block_factor_merge);
    }
  }

  spv::Id result_source_alpha_saturate;
  {
    builder_->setBuildPoint(&block_factor_source_alpha_saturate);
    result_source_alpha_saturate = builder_->createNoContractionBinOp(
        spv::OpVectorTimesScalar, type_float3_, value,
        builder_->createBinBuiltinCall(type_float_, ext_inst_glsl_std_450_, GLSLstd450NMin,
                                       source_alpha,
                                       builder_->createNoContractionBinOp(
                                           spv::OpFSub, type_float_, const_float_1_, dest_alpha)));
    builder_->createBranch(&block_factor_merge);
  }

  builder_->setBuildPoint(&block_factor_merge);
  id_vector_temp_.clear();
  id_vector_temp_.reserve(size_t{2} * 14);
  id_vector_temp_.push_back(value);
  id_vector_temp_.push_back(block_factor_one.getId());
  for (u32 i = 0; i < 3; ++i) {
    id_vector_temp_.push_back(color_factor_results[i]);
    id_vector_temp_.push_back(color_factor_blocks[i]->getId());
    id_vector_temp_.push_back(one_minus_color_factor_results[i]);
    id_vector_temp_.push_back(one_minus_color_factor_blocks[i]->getId());
    id_vector_temp_.push_back(alpha_factor_results[i]);
    id_vector_temp_.push_back(alpha_factor_blocks[i]->getId());
    id_vector_temp_.push_back(one_minus_alpha_factor_results[i]);
    id_vector_temp_.push_back(one_minus_alpha_factor_blocks[i]->getId());
  }
  id_vector_temp_.push_back(result_source_alpha_saturate);
  id_vector_temp_.push_back(block_factor_source_alpha_saturate.getId());
  spv::Id result_unclamped = builder_->createOp(spv::OpPhi, type_float3_, id_vector_temp_);
  spv::Id result = FSI_FlushNaNClampAndInBlending(result_unclamped, is_fixed_point, clamp_min_value,
                                                  clamp_max_value);

  factor_not_zero_if.makeEndIf();

  return factor_not_zero_if.createMergePhi(result, const_float3_0_);
}

spv::Id SPIRVShaderTranslator::FSI_ApplyAlphaBlendFactor(spv::Id value, spv::Id is_fixed_point,
                                                         spv::Id clamp_min_value,
                                                         spv::Id clamp_max_value, spv::Id factor,
                                                         spv::Id source_alpha, spv::Id dest_alpha,
                                                         spv::Id constant_alpha) {
  // Compute the condition before OpSelectionMerge, which must be the penultimate instruction.
  SPIRVBuilder::IfBuilder factor_not_zero_if(
      builder_->createBinOp(
          spv::OpINotEqual, type_bool_, factor,
          builder_->makeUintConstant(std::to_underlying(xenos::BlendFactor::kZero))),
      spv::SelectionControlDontFlattenMask, *builder_);

  spv::Block& block_factor_head = *builder_->getBuildPoint();
  spv::Block& block_factor_one = builder_->makeNewBlock();
  std::array<spv::Block*, 3> alpha_factor_blocks;
  std::array<spv::Block*, 3> one_minus_alpha_factor_blocks;
  alpha_factor_blocks[0] = &builder_->makeNewBlock();
  one_minus_alpha_factor_blocks[0] = &builder_->makeNewBlock();
  alpha_factor_blocks[1] = &builder_->makeNewBlock();
  one_minus_alpha_factor_blocks[1] = &builder_->makeNewBlock();
  alpha_factor_blocks[2] = &builder_->makeNewBlock();
  one_minus_alpha_factor_blocks[2] = &builder_->makeNewBlock();
  spv::Block& block_factor_source_alpha_saturate = builder_->makeNewBlock();
  spv::Block& block_factor_merge = builder_->makeNewBlock();
  builder_->createSelectionMerge(&block_factor_merge, spv::SelectionControlDontFlattenMask);
  {
    std::unique_ptr<spv::Instruction> factor_switch_op =
        std::make_unique<spv::Instruction>(spv::OpSwitch);
    factor_switch_op->addIdOperand(factor);
    factor_switch_op->addIdOperand(block_factor_one.getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kSrcColor));
    factor_switch_op->addIdOperand(alpha_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusSrcColor));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kSrcAlpha));
    factor_switch_op->addIdOperand(alpha_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusSrcAlpha));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[0]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kDstColor));
    factor_switch_op->addIdOperand(alpha_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusDstColor));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kDstAlpha));
    factor_switch_op->addIdOperand(alpha_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kOneMinusDstAlpha));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[1]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kConstantColor));
    factor_switch_op->addIdOperand(alpha_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(
        static_cast<i32>(xenos::BlendFactor::kOneMinusConstantColor));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kConstantAlpha));
    factor_switch_op->addIdOperand(alpha_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(
        static_cast<i32>(xenos::BlendFactor::kOneMinusConstantAlpha));
    factor_switch_op->addIdOperand(one_minus_alpha_factor_blocks[2]->getId());
    factor_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendFactor::kSrcAlphaSaturate));
    factor_switch_op->addIdOperand(block_factor_source_alpha_saturate.getId());
    builder_->getBuildPoint()->addInstruction(std::move(factor_switch_op));
  }
  block_factor_one.addPredecessor(&block_factor_head);
  for (u32 i = 0; i < 3; ++i) {
    alpha_factor_blocks[i]->addPredecessor(&block_factor_head);
    one_minus_alpha_factor_blocks[i]->addPredecessor(&block_factor_head);
  }
  block_factor_source_alpha_saturate.addPredecessor(&block_factor_head);

  builder_->setBuildPoint(&block_factor_one);
  builder_->createBranch(&block_factor_merge);

  std::array<spv::Id, 3> alpha_factors = {
      source_alpha,
      dest_alpha,
      constant_alpha,
  };
  std::array<spv::Id, 3> alpha_factor_results;
  std::array<spv::Id, 3> one_minus_alpha_factor_results;
  for (u32 i = 0; i < 3; ++i) {
    spv::Id alpha_factor = alpha_factors[i];

    {
      builder_->setBuildPoint(alpha_factor_blocks[i]);
      alpha_factor_results[i] =
          builder_->createNoContractionBinOp(spv::OpFMul, type_float_, value, alpha_factor);
      builder_->createBranch(&block_factor_merge);
    }

    {
      builder_->setBuildPoint(one_minus_alpha_factor_blocks[i]);
      one_minus_alpha_factor_results[i] = builder_->createNoContractionBinOp(
          spv::OpFMul, type_float_, value,
          builder_->createNoContractionBinOp(spv::OpFSub, type_float_, const_float_1_,
                                             alpha_factor));
      builder_->createBranch(&block_factor_merge);
    }
  }

  spv::Id result_source_alpha_saturate;
  {
    builder_->setBuildPoint(&block_factor_source_alpha_saturate);
    result_source_alpha_saturate = builder_->createNoContractionBinOp(
        spv::OpFMul, type_float_, value,
        builder_->createBinBuiltinCall(type_float_, ext_inst_glsl_std_450_, GLSLstd450NMin,
                                       source_alpha,
                                       builder_->createNoContractionBinOp(
                                           spv::OpFSub, type_float_, const_float_1_, dest_alpha)));
    builder_->createBranch(&block_factor_merge);
  }

  builder_->setBuildPoint(&block_factor_merge);
  id_vector_temp_.clear();
  id_vector_temp_.reserve(size_t{2} * 8);
  id_vector_temp_.push_back(value);
  id_vector_temp_.push_back(block_factor_one.getId());
  for (u32 i = 0; i < 3; ++i) {
    id_vector_temp_.push_back(alpha_factor_results[i]);
    id_vector_temp_.push_back(alpha_factor_blocks[i]->getId());
    id_vector_temp_.push_back(one_minus_alpha_factor_results[i]);
    id_vector_temp_.push_back(one_minus_alpha_factor_blocks[i]->getId());
  }
  id_vector_temp_.push_back(result_source_alpha_saturate);
  id_vector_temp_.push_back(block_factor_source_alpha_saturate.getId());
  spv::Id result_unclamped = builder_->createOp(spv::OpPhi, type_float_, id_vector_temp_);
  spv::Id result = FSI_FlushNaNClampAndInBlending(result_unclamped, is_fixed_point, clamp_min_value,
                                                  clamp_max_value);

  factor_not_zero_if.makeEndIf();

  return factor_not_zero_if.createMergePhi(result, const_float_0_);
}

spv::Id SPIRVShaderTranslator::FSI_BlendColorOrAlphaWithUnclampedResult(
    spv::Id is_fixed_point, spv::Id clamp_min_value, spv::Id clamp_max_value,
    spv::Id source_color_clamped, spv::Id source_alpha_clamped, spv::Id dest_color,
    spv::Id dest_alpha, spv::Id constant_color_clamped, spv::Id constant_alpha_clamped,
    spv::Id equation, spv::Id source_factor, spv::Id dest_factor) {
  bool is_alpha = source_color_clamped == spv::NoResult;
  REX_ASSERT(
      !(!is_alpha && (dest_color == spv::NoResult || constant_color_clamped == spv::NoResult)));
  REX_ASSERT(
      !(is_alpha && (dest_color != spv::NoResult || constant_color_clamped != spv::NoResult)));
  spv::Id value_type = is_alpha ? type_float_ : type_float3_;

  spv::Block& block_min_max_head = *builder_->getBuildPoint();
  spv::Block& block_min_max_min = builder_->makeNewBlock();
  spv::Block& block_min_max_max = builder_->makeNewBlock();
  spv::Block& block_min_max_default = builder_->makeNewBlock();
  spv::Block& block_min_max_merge = builder_->makeNewBlock();
  builder_->createSelectionMerge(&block_min_max_merge, spv::SelectionControlDontFlattenMask);
  {
    std::unique_ptr<spv::Instruction> min_max_switch_op =
        std::make_unique<spv::Instruction>(spv::OpSwitch);
    min_max_switch_op->addIdOperand(equation);
    min_max_switch_op->addIdOperand(block_min_max_default.getId());
    min_max_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendOp::kMin));
    min_max_switch_op->addIdOperand(block_min_max_min.getId());
    min_max_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendOp::kMax));
    min_max_switch_op->addIdOperand(block_min_max_max.getId());
    builder_->getBuildPoint()->addInstruction(std::move(min_max_switch_op));
  }
  block_min_max_default.addPredecessor(&block_min_max_head);
  block_min_max_min.addPredecessor(&block_min_max_head);
  block_min_max_max.addPredecessor(&block_min_max_head);

  builder_->setBuildPoint(&block_min_max_min);
  spv::Id result_min = builder_->createBinBuiltinCall(
      value_type, ext_inst_glsl_std_450_, GLSLstd450FMin,
      is_alpha ? source_alpha_clamped : source_color_clamped, is_alpha ? dest_alpha : dest_color);
  builder_->createBranch(&block_min_max_merge);

  builder_->setBuildPoint(&block_min_max_max);
  spv::Id result_max = builder_->createBinBuiltinCall(
      value_type, ext_inst_glsl_std_450_, GLSLstd450FMax,
      is_alpha ? source_alpha_clamped : source_color_clamped, is_alpha ? dest_alpha : dest_color);
  builder_->createBranch(&block_min_max_merge);

  spv::Id result_factors;
  {
    builder_->setBuildPoint(&block_min_max_default);

    spv::Id term_source, term_dest;
    if (is_alpha) {
      term_source = FSI_ApplyAlphaBlendFactor(source_alpha_clamped, is_fixed_point, clamp_min_value,
                                              clamp_max_value, source_factor, source_alpha_clamped,
                                              dest_alpha, constant_alpha_clamped);
      term_dest = FSI_ApplyAlphaBlendFactor(dest_alpha, is_fixed_point, clamp_min_value,
                                            clamp_max_value, dest_factor, source_alpha_clamped,
                                            dest_alpha, constant_alpha_clamped);
    } else {
      term_source = FSI_ApplyColorBlendFactor(source_color_clamped, is_fixed_point, clamp_min_value,
                                              clamp_max_value, source_factor, source_color_clamped,
                                              source_alpha_clamped, dest_color, dest_alpha,
                                              constant_color_clamped, constant_alpha_clamped);
      term_dest = FSI_ApplyColorBlendFactor(dest_color, is_fixed_point, clamp_min_value,
                                            clamp_max_value, dest_factor, source_color_clamped,
                                            source_alpha_clamped, dest_color, dest_alpha,
                                            constant_color_clamped, constant_alpha_clamped);
    }

    spv::Block& block_signs_head = *builder_->getBuildPoint();
    spv::Block& block_signs_add = builder_->makeNewBlock();
    spv::Block& block_signs_subtract = builder_->makeNewBlock();
    spv::Block& block_signs_reverse_subtract = builder_->makeNewBlock();
    spv::Block& block_signs_merge = builder_->makeNewBlock();
    builder_->createSelectionMerge(&block_signs_merge, spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> signs_switch_op =
          std::make_unique<spv::Instruction>(spv::OpSwitch);
      signs_switch_op->addIdOperand(equation);
      signs_switch_op->addIdOperand(block_signs_add.getId());
      signs_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendOp::kSubtract));
      signs_switch_op->addIdOperand(block_signs_subtract.getId());
      signs_switch_op->addImmediateOperand(static_cast<i32>(xenos::BlendOp::kRevSubtract));
      signs_switch_op->addIdOperand(block_signs_reverse_subtract.getId());
      builder_->getBuildPoint()->addInstruction(std::move(signs_switch_op));
    }
    block_signs_add.addPredecessor(&block_signs_head);
    block_signs_subtract.addPredecessor(&block_signs_head);
    block_signs_reverse_subtract.addPredecessor(&block_signs_head);

    builder_->setBuildPoint(&block_signs_add);
    spv::Id result_add =
        builder_->createNoContractionBinOp(spv::OpFAdd, value_type, term_source, term_dest);
    builder_->createBranch(&block_signs_merge);

    builder_->setBuildPoint(&block_signs_subtract);
    spv::Id result_subtract =
        builder_->createNoContractionBinOp(spv::OpFSub, value_type, term_source, term_dest);
    builder_->createBranch(&block_signs_merge);

    builder_->setBuildPoint(&block_signs_reverse_subtract);
    spv::Id result_reverse_subtract =
        builder_->createNoContractionBinOp(spv::OpFSub, value_type, term_dest, term_source);
    builder_->createBranch(&block_signs_merge);

    builder_->setBuildPoint(&block_signs_merge);
    id_vector_temp_.clear();
    id_vector_temp_.reserve(size_t{2} * 3);
    id_vector_temp_.push_back(result_add);
    id_vector_temp_.push_back(block_signs_add.getId());
    id_vector_temp_.push_back(result_subtract);
    id_vector_temp_.push_back(block_signs_subtract.getId());
    id_vector_temp_.push_back(result_reverse_subtract);
    id_vector_temp_.push_back(block_signs_reverse_subtract.getId());
    result_factors = builder_->createOp(spv::OpPhi, value_type, id_vector_temp_);
    builder_->createBranch(&block_min_max_merge);
  }
  spv::Block& block_min_max_default_end = *builder_->getBuildPoint();

  builder_->setBuildPoint(&block_min_max_merge);
  id_vector_temp_.clear();
  id_vector_temp_.reserve(size_t{2} * 3);
  id_vector_temp_.push_back(result_min);
  id_vector_temp_.push_back(block_min_max_min.getId());
  id_vector_temp_.push_back(result_max);
  id_vector_temp_.push_back(block_min_max_max.getId());
  id_vector_temp_.push_back(result_factors);
  id_vector_temp_.push_back(block_min_max_default_end.getId());
  return builder_->createOp(spv::OpPhi, value_type, id_vector_temp_);
}

}  // namespace rex::gpu
